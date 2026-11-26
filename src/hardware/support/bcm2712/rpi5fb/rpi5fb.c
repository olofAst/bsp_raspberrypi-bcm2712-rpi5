/*
 * Raspberry Pi 5 firmware framebuffer access (mailbox property channel).
 * Shared by fbdemo-rpi5 and camgrab-rpi5.
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
#define MBOX_TAG_GETPHYWH   0x00040003U
#define MBOX_TAG_GETDEPTH   0x00040005U
#define MBOX_TAG_GETPITCH   0x00040008U
#define MBOX_TAG_SETPHYWH   0x00048003U
#define MBOX_TAG_SETVIRTWH  0x00048004U
#define MBOX_TAG_SETDEPTH   0x00048005U
#define MBOX_TAG_SETPXLORDR 0x00048006U
#define MBOX_TAG_SETVIRTOFF 0x00048009U

static volatile uint32_t *mbox_regs;

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

int rpi5fb_open(struct rpi5fb *out, unsigned req_w, unsigned req_h, unsigned req_depth)
{
    off_t offset;
    int i = 0;
    unsigned width, height, pitch, fb_size, depth, bytespp;
    unsigned char *fb;

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

    /*
     * One message with the mode, the allocation and the read-backs. The
     * firmware is free to give something else than what was asked for (the
     * Pi 5 keeps its 16 bit per pixel boot framebuffer when the depth is set
     * in a separate message), so everything is read back and the drawing
     * code works from what was actually granted.
     */
    mbox[i++] = 0;                  /* total size, filled in below */
    mbox[i++] = MBOX_REQUEST;

    /*
     * Only ask for a mode when the user gave one. By default the mode the
     * firmware already set up for the attached monitor is used as it is: the
     * Pi 5 firmware does not change resolution or depth on request anyway.
     */
    if ((req_w != 0) && (req_h != 0)) {
        mbox[i++] = MBOX_TAG_SETPHYWH;
        mbox[i++] = 8;
        mbox[i++] = 0;
        mbox[i++] = req_w;
        mbox[i++] = req_h;

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
    }
    if (req_depth != 0) {
        mbox[i++] = MBOX_TAG_SETDEPTH;
        mbox[i++] = 4;
        mbox[i++] = 0;
        mbox[i++] = req_depth;

        mbox[i++] = MBOX_TAG_SETPXLORDR;
        mbox[i++] = 4;
        mbox[i++] = 0;
        mbox[i++] = 0;              /* 0 = BGR: a 32-bit pixel word is 0xAARRGGBB */
    }

    const int allocbuf = i;
    mbox[i++] = MBOX_TAG_ALLOCFB;
    mbox[i++] = 8;
    mbox[i++] = 0;
    mbox[i++] = 16;                 /* alignment; response: address */
    mbox[i++] = 0;                  /* response: size */

    /* read back what is really in effect */
    const int getphys = i;
    mbox[i++] = MBOX_TAG_GETPHYWH;
    mbox[i++] = 8;
    mbox[i++] = 0;
    mbox[i++] = 0;
    mbox[i++] = 0;

    const int getdepth = i;
    mbox[i++] = MBOX_TAG_GETDEPTH;
    mbox[i++] = 4;
    mbox[i++] = 0;
    mbox[i++] = 0;

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
    width = mbox[getphys + 3];
    height = mbox[getphys + 4];
    depth = mbox[getdepth + 3];

    /* The firmware returns a VC bus address: strip the alias bits */
    const uint32_t fb_addr = mbox[allocbuf + 3] & 0x3FFFFFFFU;
    fb_size = mbox[allocbuf + 4];
    pitch = mbox[getpitch + 3];

    if ((fb_addr == 0) || (fb_size == 0) || (pitch == 0) || (width == 0) || (height == 0)) {
        fprintf(stderr, "Firmware did not provide a framebuffer (addr 0x%x size %u pitch %u %ux%u).\n"
                        "Is a monitor connected?\n", fb_addr, fb_size, pitch, width, height);
        goto fail;
    }

    /*
     * The geometry is what counts: bytes per pixel = pitch / width. The depth
     * the firmware reports is only used when that does not work out.
     */
    bytespp = pitch / width;
    if (((bytespp != 2) && (bytespp != 3) && (bytespp != 4)) || ((pitch % width) != 0)) {
        bytespp = depth / 8;
        if ((bytespp != 2) && (bytespp != 3) && (bytespp != 4)) {
            fprintf(stderr, "Unsupported framebuffer format: %ux%u pitch %u depth %u\n", width, height, pitch, depth);
            goto fail;
        }
        width = pitch / bytespp;
    }
    /* never draw past what the firmware allocated */
    if ((height * pitch) > fb_size) {
        height = fb_size / pitch;
    }

    /*
     * PROT_NOCACHE: the display controller reads this memory directly, so the
     * pixels must not linger in the CPU cache (seen as missing lines and dots).
     */
    fb = mmap_device_memory(NULL, fb_size, PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, fb_addr);
    if (fb == MAP_FAILED) {
        perror("mmap_device_memory (framebuffer)");
        goto fail;
    }

    out->width = width;
    out->height = height;
    out->pitch = pitch;
    out->size = fb_size;
    out->bytespp = bytespp;
    out->mem = fb;

    munmap(mbox, 0x1000);
    close(fd);
    return 0;

fail:
    munmap(mbox, 0x1000);
    close(fd);
    return -1;
}

void rpi5fb_close(struct rpi5fb *fb)
{
    if (fb->mem != NULL) {
        munmap(fb->mem, fb->size);
        fb->mem = NULL;
    }
    if (mbox_regs != NULL) {
        munmap((void *)mbox_regs, 0x80);
        mbox_regs = NULL;
    }
}
