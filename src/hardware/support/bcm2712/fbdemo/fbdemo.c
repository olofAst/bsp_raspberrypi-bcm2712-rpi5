/*
 * Simple drawing demo for the Raspberry Pi 5 firmware framebuffer.
 *
 * The framebuffer is requested from the VideoCore firmware through the
 * mailbox property channel and then mapped into this process. Based on the
 * Raspberry Pi 4 version by Olof Astrand (itself inspired by rpi4os.com);
 * the only Pi 5 specific part is the mailbox address.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 */

#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/neutrino.h>

/* Mailbox (ARM -> VC), BCM2712: 0x10_7c01_3880 (Pi 4: 0xfe00b880) */
#define MBOX_BASE           0x107c013880ULL
#define MBOX_READ           0           /* register indices (32-bit words) */
#define MBOX_STATUS         6
#define MBOX_WRITE          8
#define MBOX_STATUS_FULL    0x80000000U
#define MBOX_STATUS_EMPTY   0x40000000U
#define MBOX_CH_PROP        8

#define MBOX_REQUEST        0x00000000U
#define MBOX_RESPONSE       0x80000000U

/* Property tags */
#define MBOX_TAG_LAST       0
#define MBOX_TAG_ALLOCFB    0x00040001U
#define MBOX_TAG_GETPITCH   0x00040008U
#define MBOX_TAG_SETPHYWH   0x00048003U
#define MBOX_TAG_SETVIRTWH  0x00048004U
#define MBOX_TAG_SETDEPTH   0x00048005U
#define MBOX_TAG_SETPXLORDR 0x00048006U
#define MBOX_TAG_SETVIRTOFF 0x00048009U

static unsigned width, height, pitch, fb_size;
static unsigned char *fb;
static volatile uint32_t *mbox_regs;

/* VGA palette (16 colors), stored as 0xAARRGGBB */
static const unsigned vgapal[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF, 0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

static int mbox_call(uint32_t *buffer, uint32_t buf_phys, uint8_t channel)
{
    if (buf_phys & 0xF) {
        fprintf(stderr, "Buffer address not 16-byte aligned\n");
        return -1;
    }

    while (mbox_regs[MBOX_STATUS] & MBOX_STATUS_FULL)
        ;
    mbox_regs[MBOX_WRITE] = (buf_phys & ~0xFU) | (channel & 0xF);

    for (;;) {
        while (mbox_regs[MBOX_STATUS] & MBOX_STATUS_EMPTY)
            ;
        const uint32_t resp = mbox_regs[MBOX_READ];
        if (((resp & 0xF) == channel) && ((resp & ~0xFU) == (buf_phys & ~0xFU))) {
            if (buffer[1] == MBOX_RESPONSE) {
                return 0;
            }
            fprintf(stderr, "Mailbox response error (0x%x)\n", buffer[1]);
            return -1;
        }
    }
}

static int fb_init(unsigned req_w, unsigned req_h)
{
    off_t offset;
    int i = 0;

    /* The firmware takes a 32-bit address: the message must live below 1G */
    const int fd = posix_typed_mem_open("/sysram&below1G", O_RDWR, POSIX_TYPED_MEM_ALLOCATE_CONTIG);
    if (fd < 0) {
        fprintf(stderr, "Failed to open typed memory (%s)\n", strerror(errno));
        return -1;
    }
    uint32_t * const mbox = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE, MAP_SHARED, fd, 0);
    if (mbox == MAP_FAILED) {
        fprintf(stderr, "Failed to allocate mailbox buffer (%s)\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (mem_offset(mbox, NOFD, 1, &offset, 0) != EOK) {
        fprintf(stderr, "Failed to obtain physical address\n");
        goto fail;
    }
    const uint32_t buf_phys = (uint32_t)offset;

    mbox_regs = mmap_device_memory(0, 0x80, PROT_NOCACHE | PROT_READ | PROT_WRITE, 0, MBOX_BASE);
    if (mbox_regs == MAP_FAILED) {
        fprintf(stderr, "Failed to map mailbox registers (%s)\n", strerror(errno));
        goto fail;
    }

    /* 1st message: display mode */
    mbox[i++] = 0;                  /* total size, filled in below */
    mbox[i++] = MBOX_REQUEST;

    mbox[i++] = MBOX_TAG_SETPHYWH;
    mbox[i++] = 8;
    mbox[i++] = 0;
    mbox[i++] = req_w;
    mbox[i++] = req_h;

    const int setvirtwh = i;
    mbox[i++] = MBOX_TAG_SETVIRTWH;
    mbox[i++] = 8;
    mbox[i++] = 0;
    mbox[i++] = req_w;
    mbox[i++] = req_h;

    mbox[i++] = MBOX_TAG_SETVIRTOFF;
    mbox[i++] = 8;
    mbox[i++] = 0;
    mbox[i++] = 0;
    mbox[i++] = 0;

    mbox[i++] = MBOX_TAG_SETDEPTH;
    mbox[i++] = 4;
    mbox[i++] = 0;
    mbox[i++] = 32;

    mbox[i++] = MBOX_TAG_SETPXLORDR;
    mbox[i++] = 4;
    mbox[i++] = 0;
    mbox[i++] = 0;                  /* 0 = BGR: a pixel word is 0xAARRGGBB */

    mbox[i++] = MBOX_TAG_LAST;
    while (i & 3) {
        mbox[i++] = 0;
    }
    mbox[0] = i * sizeof(uint32_t);

    if (mbox_call(mbox, buf_phys, MBOX_CH_PROP) != 0) {
        goto fail;
    }
    width = mbox[setvirtwh + 3];
    height = mbox[setvirtwh + 4];

    /* 2nd message: allocate the buffer and ask for its pitch */
    i = 0;
    mbox[i++] = 0;
    mbox[i++] = MBOX_REQUEST;

    const int allocbuf = i;
    mbox[i++] = MBOX_TAG_ALLOCFB;
    mbox[i++] = 8;
    mbox[i++] = 0;
    mbox[i++] = 16;                 /* alignment; response: address */
    mbox[i++] = 0;                  /* response: size */

    const int getpitch = i;
    mbox[i++] = MBOX_TAG_GETPITCH;
    mbox[i++] = 4;
    mbox[i++] = 0;
    mbox[i++] = 0;

    mbox[i++] = MBOX_TAG_LAST;
    while (i & 3) {
        mbox[i++] = 0;
    }
    mbox[0] = i * sizeof(uint32_t);

    if (mbox_call(mbox, buf_phys, MBOX_CH_PROP) != 0) {
        goto fail;
    }

    /* The firmware returns a VC bus address: strip the alias bits */
    const uint32_t fb_addr = mbox[allocbuf + 3] & 0x3FFFFFFFU;
    fb_size = mbox[allocbuf + 4];
    pitch = mbox[getpitch + 3];

    if ((fb_addr == 0) || (fb_size == 0) || (pitch == 0)) {
        fprintf(stderr, "Firmware did not provide a framebuffer (addr 0x%x size %u pitch %u).\n"
                        "Is a monitor connected, and does it support %ux%u?\n",
                        fb_addr, fb_size, pitch, req_w, req_h);
        goto fail;
    }

    fb = mmap_device_memory(NULL, fb_size, PROT_READ | PROT_WRITE, 0, fb_addr);
    if (fb == MAP_FAILED) {
        perror("mmap_device_memory (framebuffer)");
        goto fail;
    }
    printf("Framebuffer %ux%u, pitch %u, %u bytes at physical 0x%x\n", width, height, pitch, fb_size, fb_addr);

    munmap(mbox, 0x1000);
    close(fd);
    return 0;

fail:
    munmap(mbox, 0x1000);
    close(fd);
    return -1;
}

static void drawPixel(int x, int y, unsigned char attr)
{
    if (x < 0 || x >= (int)width || y < 0 || y >= (int)height) return;
    *((unsigned int *)(fb + y * pitch + x * 4)) = vgapal[attr & 0x0f];
}

static void drawRect(int x1, int y1, int x2, int y2, unsigned char attr, int fill)
{
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            if ((x == x1 || x == x2) || (y == y1 || y == y2))
                drawPixel(x, y, attr);
            else if (fill)
                drawPixel(x, y, (attr & 0xf0) >> 4);
        }
    }
}

static void drawLine(int x1, int y1, int x2, int y2, unsigned char attr)
{
    const int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    const int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        drawPixel(x1, y1, attr);
        if (x1 == x2 && y1 == y2) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

static void drawCircle(int x0, int y0, int radius, unsigned char attr, int fill)
{
    int x = radius, y = 0, err = 0;

    while (x >= y) {
        if (fill) {
            drawLine(x0 - y, y0 + x, x0 + y, y0 + x, (attr & 0xf0) >> 4);
            drawLine(x0 - x, y0 + y, x0 + x, y0 + y, (attr & 0xf0) >> 4);
            drawLine(x0 - x, y0 - y, x0 + x, y0 - y, (attr & 0xf0) >> 4);
            drawLine(x0 - y, y0 - x, x0 + y, y0 - x, (attr & 0xf0) >> 4);
        }
        drawPixel(x0 - y, y0 + x, attr);
        drawPixel(x0 + y, y0 + x, attr);
        drawPixel(x0 - x, y0 + y, attr);
        drawPixel(x0 + x, y0 + y, attr);
        drawPixel(x0 - x, y0 - y, attr);
        drawPixel(x0 + x, y0 - y, attr);
        drawPixel(x0 - y, y0 - x, attr);
        drawPixel(x0 + y, y0 - x, attr);

        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

/* Small sin/cos so that the demo does not need libm */
static float my_fmod(float x, float y)
{
    if (y == 0.0f) return x;
    const float div = x / y;
    const float q = div >= 0.0f ? (float)((int)div) : (float)((int)div - 1);
    return x - y * q;
}

static float normalize_angle(float x)
{
    x = my_fmod(x, 2 * 3.14159f);
    if (x > 3.14159f)
        x -= 2 * 3.14159f;
    else if (x < -3.14159f)
        x += 2 * 3.14159f;
    return x;
}

static float my_cos(float x)
{
    x = normalize_angle(x);
    const float x2 = x * x, x4 = x2 * x2;
    return 1.0f - x2 / 2.0f + x4 / 24.0f - (x2 * x4) / 720.0f + (x4 * x4) / 40320.0f;
}

static float my_sin(float x)
{
    x = normalize_angle(x);
    const float x2 = x * x, x3 = x2 * x, x5 = x2 * x3, x7 = x2 * x5;
    return x - x3 / 6.0f + x5 / 120.0f - x7 / 5040.0f + (x2 * x7) / 362880.0f;
}

int main(int argc, char *argv[])
{
    unsigned req_w = 640, req_h = 480;
    int opt, wait_key = 1;

    while ((opt = getopt(argc, argv, "w:h:n")) != -1) {
        switch (opt) {
            case 'w': req_w = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'h': req_h = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'n': wait_key = 0; break;
            default:  return EXIT_FAILURE;
        }
    }

    if (fb_init(req_w, req_h) != 0) {
        return EXIT_FAILURE;
    }

    memset(fb, 0, height * pitch);

    drawRect(100, 100, 300, 200, 0x1E, 1);      /* yellow frame, blue fill */
    drawLine(50, 50, 400, 400, 0x0F);           /* white diagonal */
    drawCircle(500, 300, 100, 0x0C, 0);         /* red circle */
    drawCircle(500, 300, 40, 0x2A, 1);          /* green disc */

    /* spokes */
    for (int i = 0; i < 360; i += 15) {
        const int x = 200 + (int)(100 * my_cos(i * 3.14159f / 180));
        const int y = 300 + (int)(100 * my_sin(i * 3.14159f / 180));
        drawLine(200, 300, x, y, 0x0D);
    }

    /* the 16 palette colours along the bottom */
    for (int c = 0; c < 16; c++) {
        const int w = (int)width / 16;
        drawRect(c * w, (int)height - 40, (c + 1) * w - 1, (int)height - 1, (unsigned char)((c << 4) | c), 1);
    }

    if (wait_key) {
        printf("Press Enter to exit...\n");
        (void)getchar();
    }

    munmap(fb, fb_size);
    munmap((void *)mbox_regs, 0x80);
    return EXIT_SUCCESS;
}
