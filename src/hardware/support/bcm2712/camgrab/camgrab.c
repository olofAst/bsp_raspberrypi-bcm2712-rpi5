/*
 * camgrab-rpi5 - grab frames from a USB (UVC) camera and show them on the
 * firmware framebuffer.
 *
 * The UVC work is done by the SDP's video capture driver
 * (libcapture-usb-uvc.so); this program drives it through the capture API,
 * reports what the camera delivers and blits the luma of each frame to HDMI.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <vcapture/capture.h>
#include "rpi5fb.h"

/* Screen pixel format numbers (screen/screen.h), used by the capture API */
#define FMT_BYTE    1
#define FMT_RGB565  6
#define FMT_UYVY    14
#define FMT_YUY2    15

#define NBUFFERS    4

enum layout { L_UNKNOWN, L_Y8, L_YUY2, L_UYVY_Y16 };

static const struct {
    const char *name;
    int         screen_fmt;
    enum layout layout;
} formats[] = {
    { "yuy2", FMT_YUY2, L_YUY2 },
    { "uyvy", FMT_UYVY, L_UYVY_Y16 },
    { "y8",   FMT_BYTE, L_Y8 },
    { "y16",  0,        L_UYVY_Y16 },   /* little endian: the high byte is the second one */
};

static const struct { unsigned w, h; } common_sizes[] = {
    { 1920, 1200 }, { 1920, 1080 }, { 1600, 1200 }, { 1280, 960 }, { 1280, 720 },
    { 1024, 768 }, { 960, 600 }, { 800, 600 }, { 640, 480 }, { 320, 240 },
};

static void set_i(capture_context_t ctx, uint32_t prop, const char *name, int32_t val)
{
    if (!capture_is_property(ctx, prop)) {
        printf("  (driver has no %s property)\n", name);
        return;
    }
    if (capture_set_property_i(ctx, prop, val) != 0) {
        printf("  setting %s = %d failed: %s\n", name, val, strerror(errno));
    }
}

static int get_i(capture_context_t ctx, uint32_t prop, int32_t *val)
{
    *val = 0;
    if (!capture_is_property(ctx, prop)) {
        return -1;
    }
    return capture_get_property_i(ctx, prop, val);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1e9);
}

int main(int argc, char *argv[])
{
    int             dev = 0, verbose = 0, use_fb = 1, opt;
    unsigned        req_w = 0, req_h = 0, max_frames = 0;
    const char     *outfile = NULL, *fmt_name = NULL;
    int             screen_fmt = 0;
    enum layout     layout = L_UNKNOWN;
    struct rpi5fb   fb;
    unsigned        i;

    while ((opt = getopt(argc, argv, "d:w:h:f:n:o:xv")) != -1) {
        switch (opt) {
            case 'd': dev = atoi(optarg); break;
            case 'w': req_w = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'h': req_h = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'f': fmt_name = optarg; break;
            case 'n': max_frames = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'o': outfile = optarg; break;
            case 'x': use_fb = 0; break;
            case 'v': verbose++; break;
            default:  return EXIT_FAILURE;
        }
    }
    if (fmt_name != NULL) {
        for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
            if (strcmp(fmt_name, formats[i].name) == 0) {
                screen_fmt = formats[i].screen_fmt;
                layout = formats[i].layout;
                break;
            }
        }
        if (layout == L_UNKNOWN) {
            fprintf(stderr, "camgrab: unknown format '%s' (yuy2, uyvy, y8, y16)\n", fmt_name);
            return EXIT_FAILURE;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);

    /* the capture driver waits forever for the USB stack if it is not there */
    if (access("/dev/usb/io-usb-otg", F_OK) != 0) {
        fprintf(stderr, "camgrab: the USB stack (io-usb-otg) is not running\n");
        return EXIT_FAILURE;
    }

    printf("Opening the USB camera driver (waits until a camera is plugged in; Ctrl-C to stop)\n");
    capture_context_t ctx = capture_create_context(0);
    if (ctx == NULL) {
        fprintf(stderr, "camgrab: capture_create_context failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Configuring camera %d\n", dev);
    if (verbose) {
        set_i(ctx, CAPTURE_PROPERTY_VERBOSITY, "VERBOSITY", verbose);
    }
    set_i(ctx, CAPTURE_PROPERTY_DEVICE, "DEVICE", dev);
    if (screen_fmt != 0) {
        set_i(ctx, CAPTURE_PROPERTY_DST_FORMAT, "DST_FORMAT", screen_fmt);
    }
    if ((req_w != 0) && (req_h != 0)) {
        set_i(ctx, CAPTURE_PROPERTY_SRC_WIDTH, "SRC_WIDTH", (int32_t)req_w);
        set_i(ctx, CAPTURE_PROPERTY_SRC_HEIGHT, "SRC_HEIGHT", (int32_t)req_h);
        set_i(ctx, CAPTURE_PROPERTY_DST_WIDTH, "DST_WIDTH", (int32_t)req_w);
        set_i(ctx, CAPTURE_PROPERTY_DST_HEIGHT, "DST_HEIGHT", (int32_t)req_h);
    }

    /* Frame buffers: large enough for 1920x1200 at 2 bytes per pixel unless told otherwise */
    const unsigned buf_bytes = ((req_w != 0) && (req_h != 0)) ? (req_w * req_h * 2U) : (1920U * 1200U * 2U);
    static void     *bufs[NBUFFERS];
    static uint32_t  nbytes[NBUFFERS], seqno[NBUFFERS], flags[NBUFFERS];
    void           **frame_bufs = NULL;

    set_i(ctx, CAPTURE_PROPERTY_DST_NBYTES, "DST_NBYTES", (int32_t)buf_bytes);
    set_i(ctx, CAPTURE_PROPERTY_FRAME_NBUFFERS, "FRAME_NBUFFERS", NBUFFERS);
    if (capture_create_buffers(ctx, CAPTURE_PROPERTY_FRAME_BUFFERS) != 0) {
        printf("  capture_create_buffers failed (%s), using own buffers\n", strerror(errno));
        for (i = 0; i < NBUFFERS; i++) {
            bufs[i] = malloc(buf_bytes);
            if (bufs[i] == NULL) {
                fprintf(stderr, "camgrab: out of memory\n");
                return EXIT_FAILURE;
            }
        }
        if (capture_set_property_p(ctx, CAPTURE_PROPERTY_FRAME_BUFFERS, bufs) != 0) {
            printf("  setting FRAME_BUFFERS failed: %s\n", strerror(errno));
        }
    }
    if (capture_is_property(ctx, CAPTURE_PROPERTY_FRAME_NBYTES)) {
        (void)capture_set_property_p(ctx, CAPTURE_PROPERTY_FRAME_NBYTES, nbytes);
    }
    if (capture_is_property(ctx, CAPTURE_PROPERTY_FRAME_SEQNO)) {
        (void)capture_set_property_p(ctx, CAPTURE_PROPERTY_FRAME_SEQNO, seqno);
    }
    if (capture_is_property(ctx, CAPTURE_PROPERTY_FRAME_FLAGS)) {
        (void)capture_set_property_p(ctx, CAPTURE_PROPERTY_FRAME_FLAGS, flags);
    }

    printf("Starting capture\n");
    if (capture_set_property_i(ctx, CAPTURE_ENABLE, 1) != 0) {
        fprintf(stderr, "camgrab: enabling capture failed: %s\n", strerror(errno));
    }
    if (capture_update(ctx, 0) != 0) {
        fprintf(stderr, "camgrab: capture_update failed: %s\n"
                        "         (is the camera plugged in and io-usb-otg running? try -v and slog2info)\n",
                strerror(errno));
        capture_destroy_context(ctx);
        return EXIT_FAILURE;
    }
    if (capture_get_property_p(ctx, CAPTURE_PROPERTY_FRAME_BUFFERS, (void **)&frame_bufs) != 0) {
        frame_bufs = bufs;
    }

    int32_t cw, ch, cfmt, cstride;
    (void)get_i(ctx, CAPTURE_PROPERTY_DST_WIDTH, &cw);
    (void)get_i(ctx, CAPTURE_PROPERTY_DST_HEIGHT, &ch);
    (void)get_i(ctx, CAPTURE_PROPERTY_DST_FORMAT, &cfmt);
    (void)get_i(ctx, CAPTURE_PROPERTY_DST_STRIDE, &cstride);
    if ((cw == 0) || (ch == 0)) {
        (void)get_i(ctx, CAPTURE_PROPERTY_SRC_WIDTH, &cw);
        (void)get_i(ctx, CAPTURE_PROPERTY_SRC_HEIGHT, &ch);
    }
    printf("Driver reports: %dx%d, format %d, stride %d\n", cw, ch, cfmt, cstride);

    unsigned width = (cw > 0) ? (unsigned)cw : req_w;
    unsigned height = (ch > 0) ? (unsigned)ch : req_h;
    if (layout == L_UNKNOWN) {
        if (cfmt == FMT_YUY2) layout = L_YUY2;
        else if (cfmt == FMT_UYVY) layout = L_UYVY_Y16;
        else if (cfmt == FMT_BYTE) layout = L_Y8;
    }

    int have_fb = 0;
    uint16_t lut16[256];
    uint32_t lut32[256];
    if (use_fb && (rpi5fb_open(&fb, 0, 0, 0) == 0)) {
        have_fb = 1;
        memset(fb.mem, 0, fb.height * fb.pitch);
        for (i = 0; i < 256; i++) {
            const uint32_t grey = 0xff000000U | (i << 16) | (i << 8) | i;
            lut16[i] = (uint16_t)rpi5fb_color(&fb, grey);
            lut32[i] = grey;
        }
    }

    unsigned    frames = 0, errors = 0;
    double      t0 = now();
    uint8_t    *line = NULL;

    while ((max_frames == 0) || (frames < max_frames)) {
        const int idx = capture_get_frame(ctx, 3000000000ULL, 0);
        if (idx < 0) {
            printf("capture_get_frame: %s\n", strerror(errno));
            if (++errors >= 5) {
                printf("giving up after %u failures\n", errors);
                break;
            }
            continue;
        }
        if ((idx >= NBUFFERS) || (frame_bufs == NULL) || (frame_bufs[idx] == NULL)) {
            printf("capture_get_frame returned unusable buffer %d\n", idx);
            break;
        }
        const uint8_t * const data = frame_bufs[idx];
        unsigned len = nbytes[idx];

        if (frames == 0) {
            /* work out the geometry from what actually arrived */
            if (len == 0) {
                len = (width != 0) ? (width * height * 2U) : buf_bytes;
                printf("driver does not report the frame length, assuming %u bytes\n", len);
            }
            if ((width == 0) || (height == 0)) {
                for (i = 0; i < sizeof(common_sizes) / sizeof(common_sizes[0]); i++) {
                    const unsigned px = common_sizes[i].w * common_sizes[i].h;
                    if ((len == px) || (len == px * 2U)) {
                        width = common_sizes[i].w;
                        height = common_sizes[i].h;
                        break;
                    }
                }
            }
            if ((layout == L_UNKNOWN) && (width != 0)) {
                layout = (len >= width * height * 2U) ? L_YUY2 : L_Y8;
            }
            printf("First frame: buffer %d, %u bytes, seq %u, flags 0x%x -> showing as %ux%u %s\n",
                   idx, len, seqno[idx], flags[idx], width, height,
                   (layout == L_Y8) ? "8-bit grey" : (layout == L_YUY2) ? "YUY2 (luma first)" :
                   (layout == L_UYVY_Y16) ? "UYVY/Y16 (luma second)" : "unknown");
            printf("  data:");
            for (i = 0; i < 16; i++) {
                printf(" %02x", data[i]);
            }
            printf("\n");
            if (outfile != NULL) {
                const int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if ((fd == -1) || (write(fd, data, len) != (ssize_t)len)) {
                    printf("  saving to %s failed: %s\n", outfile, strerror(errno));
                } else {
                    printf("  saved to %s\n", outfile);
                }
                if (fd != -1) {
                    close(fd);
                }
            }
            if (have_fb) {
                line = malloc(fb.pitch);
            }
        }

        if (have_fb && (line != NULL) && (width != 0) && (layout != L_UNKNOWN)) {
            const unsigned step = (layout == L_Y8) ? 1U : 2U;
            const unsigned first = (layout == L_UYVY_Y16) ? 1U : 0U;
            const unsigned stride = ((cstride > 0) && ((unsigned)cstride >= width * step)) ? (unsigned)cstride : (width * step);
            const unsigned w = (width < fb.width) ? width : fb.width;
            const unsigned h = (height < fb.height) ? height : fb.height;
            unsigned x, y;

            /* build each row in normal memory, then copy it to the (uncached) framebuffer */
            for (y = 0; y < h; y++) {
                const uint8_t *src = data + (y * stride) + first;
                if (fb.bytespp == 2) {
                    uint16_t * const dst = (uint16_t *)line;
                    for (x = 0; x < w; x++, src += step) {
                        dst[x] = lut16[*src];
                    }
                } else if (fb.bytespp == 4) {
                    uint32_t * const dst = (uint32_t *)line;
                    for (x = 0; x < w; x++, src += step) {
                        dst[x] = lut32[*src];
                    }
                } else {
                    for (x = 0; x < w; x++, src += step) {
                        line[x * 3] = line[(x * 3) + 1] = line[(x * 3) + 2] = *src;
                    }
                }
                memcpy(fb.mem + (y * fb.pitch), line, w * fb.bytespp);
            }
        }

        (void)capture_release_frame(ctx, (uint32_t)idx);
        frames++;
        if ((frames % 30U) == 0U) {
            const double t = now();
            printf("%u frames, %.1f fps\n", frames, 30.0 / (t - t0));
            t0 = t;
        }
    }

    printf("%u frames captured\n", frames);
    (void)capture_set_property_i(ctx, CAPTURE_ENABLE, 0);
    (void)capture_update(ctx, 0);
    capture_destroy_context(ctx);
    if (have_fb) {
        rpi5fb_close(&fb);
    }
    return (frames > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
