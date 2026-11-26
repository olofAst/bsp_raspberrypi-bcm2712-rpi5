/*
 * peek - read or write physical (device) memory. Bring-up aid.
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
#include <unistd.h>
#include <sys/mman.h>
#include <hw/inout.h>

int main(int argc, char *argv[])
{
    unsigned width = 4;
    int opt;

    while ((opt = getopt(argc, argv, "w:")) != -1) {
        if (opt == 'w') {
            width = (unsigned)strtoul(optarg, NULL, 0);
        } else {
            return EXIT_FAILURE;
        }
    }
    if ((optind >= argc) || ((width != 1) && (width != 2) && (width != 4) && (width != 8))) {
        fprintf(stderr, "usage: peek [-w 1|2|4|8] address [count]\n       peek [-w 1|2|4|8] address = value\n");
        return EXIT_FAILURE;
    }

    const uint64_t addr = strtoull(argv[optind], NULL, 0);
    const int writing = ((optind + 2) < argc) && (strcmp(argv[optind + 1], "=") == 0);
    const uint64_t count = (!writing && ((optind + 1) < argc)) ? strtoull(argv[optind + 1], NULL, 0) : 1;
    const uint64_t pg = addr & ~0xfffULL;
    const size_t len = (size_t)((addr - pg) + (count * width) + 0xfffULL) & ~(size_t)0xfff;
    uint8_t * const base = mmap_device_memory(NULL, len, PROT_NOCACHE | PROT_READ | PROT_WRITE, 0, pg);

    if (base == MAP_FAILED) {
        fprintf(stderr, "peek: cannot map 0x%llx: %s\n", (unsigned long long)addr, strerror(errno));
        return EXIT_FAILURE;
    }

    volatile uint8_t * const p = base + (addr - pg);
    if (writing) {
        const uint64_t v = strtoull(argv[optind + 2], NULL, 0);
        switch (width) {
            case 1: *(volatile uint8_t *)p = (uint8_t)v; break;
            case 2: *(volatile uint16_t *)p = (uint16_t)v; break;
            case 4: *(volatile uint32_t *)p = (uint32_t)v; break;
            default: *(volatile uint64_t *)p = v; break;
        }
        return EXIT_SUCCESS;
    }

    for (uint64_t i = 0; i < count; i++) {
        volatile uint8_t * const q = p + (i * width);
        const unsigned long long a = (unsigned long long)(addr + (i * width));
        switch (width) {
            case 1: printf("%llx: %02x\n", a, *(volatile uint8_t *)q); break;
            case 2: printf("%llx: %04x\n", a, *(volatile uint16_t *)q); break;
            case 4: printf("%llx: %08x\n", a, *(volatile uint32_t *)q); break;
            default: printf("%llx: %016llx\n", a, (unsigned long long)*(volatile uint64_t *)q); break;
        }
    }
    return EXIT_SUCCESS;
}
