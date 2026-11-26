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
#include "rpi5fb.h"

static struct rpi5fb fbi;
static unsigned width, height;

/* VGA palette (16 colors), stored as 0xAARRGGBB */
static const unsigned vgapal[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF, 0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

static void drawPixel(int x, int y, unsigned char attr)
{
    rpi5fb_pixel(&fbi, x, y, vgapal[attr & 0x0f]);
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
    unsigned req_w = 0, req_h = 0, req_depth = 0;     /* 0 = keep the firmware's mode */
    int opt, wait_key = 1;

    while ((opt = getopt(argc, argv, "w:h:b:n")) != -1) {
        switch (opt) {
            case 'w': req_w = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'h': req_h = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'b': req_depth = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'n': wait_key = 0; break;
            default:  return EXIT_FAILURE;
        }
    }

    if (rpi5fb_open(&fbi, req_w, req_h, req_depth) != 0) {
        return EXIT_FAILURE;
    }

    width = fbi.width;
    height = fbi.height;
    printf("Framebuffer %ux%u, %u bpp, pitch %u\n", fbi.width, fbi.height, fbi.bytespp * 8, fbi.pitch);

    memset(fbi.mem, 0, fbi.height * fbi.pitch);

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

    rpi5fb_close(&fbi);
    return EXIT_SUCCESS;
}
