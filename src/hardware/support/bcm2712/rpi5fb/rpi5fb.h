/*
 * Raspberry Pi 5 firmware framebuffer access (mailbox property channel).
 * Shared by fbdemo-rpi5 and camgrab-rpi5.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 */
#ifndef RPI5FB_H
#define RPI5FB_H

#include <stdint.h>

struct rpi5fb {
    unsigned        width, height;  /* pixels */
    unsigned        pitch;          /* bytes per row */
    unsigned        size;           /* bytes mapped */
    unsigned        bytespp;        /* 2 (RGB565), 3 or 4 */
    unsigned char  *mem;
};

/*
 * Get the firmware framebuffer and map it. req_w/req_h/req_depth = 0 keeps
 * the mode the firmware set up for the monitor (the Pi 5 firmware does not
 * change mode on request anyway). Returns 0 on success.
 */
int  rpi5fb_open(struct rpi5fb *fb, unsigned req_w, unsigned req_h, unsigned req_depth);
void rpi5fb_close(struct rpi5fb *fb);

/* 0xAARRGGBB -> the framebuffer's own pixel value (for 2 and 4 bytes per pixel) */
static inline uint32_t rpi5fb_color(const struct rpi5fb *fb, uint32_t argb)
{
    if (fb->bytespp == 2) {
        return ((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) | ((argb >> 3) & 0x001f);
    }
    return argb;
}

static inline void rpi5fb_pixel(const struct rpi5fb *fb, int x, int y, uint32_t argb)
{
    if ((x < 0) || (x >= (int)fb->width) || (y < 0) || (y >= (int)fb->height)) {
        return;
    }
    unsigned char * const p = fb->mem + ((unsigned)y * fb->pitch) + ((unsigned)x * fb->bytespp);
    switch (fb->bytespp) {
        case 4:
            *(uint32_t *)p = argb;
            break;
        case 3:
            p[0] = (unsigned char)argb;
            p[1] = (unsigned char)(argb >> 8);
            p[2] = (unsigned char)(argb >> 16);
            break;
        default:
            *(uint16_t *)p = (uint16_t)rpi5fb_color(fb, argb);
            break;
    }
}

#endif
