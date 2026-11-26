/*
 * $QNXLicenseC:
 * Copyright 2025 QNX Software Systems.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as
 * contributors under the License or as licensors under other terms.
 * Please review this entire file for other proprietary rights or license
 * notices, as well as the QNX Development Suite License Guide at
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */

/*
 * RP1 MSI-X configuration utility, SDP 7.1 version.
 *
 * The SDP 8.0 BSP does this work with pci-server and the pci_hw-bcm2712-rpi5.so
 * hardware module. Neither a BCM2712 PCI hardware module nor a need for a
 * general PCI bus exist in this 7.1 port, so this utility talks to the hardware
 * directly:
 *
 *   RP1 peripheral irq n
 *     -> RP1 MSI-X vector n (table in RP1 BAR0, per-vector MSIX_CFG in RP1 BAR1)
 *     -> PCIe memory write of <n> to the doorbell address 0xff_ffff_f000
 *     -> BCM2712 PCIe2 root complex inbound window
 *     -> MIP0 interrupt controller (0x10_0013_0000)
 *     -> GIC interrupt 160 + n (edge triggered)
 *
 * It relies on the Pi 5 firmware having trained the PCIe link and assigned the
 * RP1 BARs, which requires "pciex4_reset=0" in config.txt. The root complex is
 * NOT reset or re-enumerated here; only what is missing for interrupts is added.
 *
 * Register definitions follow the Raspberry Pi Linux drivers
 * (pcie-brcmstb.c, irq-bcm2712-mip.c, mfd/rp1.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <hw/inout.h>

/* ---- BCM2712 PCIe2 root complex (the one RP1 hangs off) ---- */
#define RC_PADDR                        0x1000120000ULL
#define RC_SIZE                         0xa000U

#define RC_CFG_COMMAND                  0x04U
#define RC_CFG_BUSES                    0x18U
#define RC_CFG_MEM_BASE_LIMIT           0x20U

#define RC_CPU_2_PCIE_MEM_WIN0_LO       0x400cU
#define RC_CPU_2_PCIE_MEM_WIN0_HI       0x4010U
#define RC_PCIE_STATUS                  0x4068U
#define  RC_PCIE_STATUS_DL_ACTIVE       0x20U
#define  RC_PCIE_STATUS_PHYLINKUP       0x10U
#define RC_MEM_WIN0_BASE_LIMIT          0x4070U
#define RC_MEM_WIN0_BASE_HI             0x4080U
#define RC_EXT_CFG_DATA                 0x8000U
#define RC_EXT_CFG_INDEX                0x9000U

/* Inbound windows ("RC BARs"): PCI address + size, and the CPU address they remap to */
#define RC_NUM_INBOUND_WINS             10U
#define RC_BAR_ENABLE_REMAP             0x1U
#define RAM_PCI_ADDR                    0x1000000000ULL     /* default PCI address of host RAM */

/* ---- BCM2712 MIP0 (MSI-X interrupt peripheral for PCIe2) ---- */
#define MIP_PADDR                       0x1000130000ULL
#define MIP_SIZE                        0x1000U
#define MIP_INT_CFGL_HOST               0x20U
#define MIP_INT_CFGH_HOST               0x30U
#define MIP_INT_MASKL_HOST              0x40U
#define MIP_INT_MASKH_HOST              0x50U
#define MIP_INT_MASKL_VPU               0x60U
#define MIP_INT_MASKH_VPU               0x70U
#define MIP_MSI_PCI_ADDR                0xfffffff000ULL     /* doorbell as seen from PCIe */
#define MIP_GIC_IRQ_BASE                160U                /* GIC SPI 128 */

/* ---- GIC distributor ---- */
#define GICD_PADDR                      0x107fff9000ULL
#define GICD_ICFGR                      0xc00U

/* ---- RP1 ---- */
#define RP1_VID_DID                     0x00011de4U
#define RP1_NUM_IRQS                    64U
#define RP1_MSIX_CFG_OFFSET             0x108000U           /* in BAR1 */
#define RP1_MSIX_CFG_SIZE               0x1000U
#define RP1_MSIX_CFG(irq)               (0x008U + ((irq) * 4U))
#define RP1_REG_SET                     0x800U
#define RP1_REG_CLR                     0xc00U
#define  MSIX_CFG_ENABLE                (1U << 0)
#define  MSIX_CFG_IACK                  (1U << 2)
#define  MSIX_CFG_IACK_EN               (1U << 3)
#define RP1_IRQ_USBHOST0                31U
#define RP1_IRQ_USBHOST1                36U

/* ---- PCI configuration space ---- */
#define PCI_COMMAND                     0x04U
#define  PCI_COMMAND_MEMORY             0x0002U
#define  PCI_COMMAND_MASTER             0x0004U
#define  PCI_COMMAND_INTX_DISABLE       0x0400U
#define PCI_BAR(n)                      (0x10U + ((n) * 4U))
#define PCI_CAP_PTR                     0x34U
#define PCI_CAP_ID_MSIX                 0x11U
#define  MSIX_CTRL_ENABLE               (1U << 31)
#define  MSIX_CTRL_MASKALL              (1U << 30)

static int       verbose;
static uintptr_t rc;            /* root complex registers */
static uint32_t  rp1_cfg_index; /* EXT_CFG_INDEX value selecting the RP1 function */

#define vprint(lvl, ...)  do { if (verbose >= (lvl)) { (void)fprintf(stderr, __VA_ARGS__); } } while (0)

static uintptr_t map_dev(const uint64_t paddr, const size_t size, const char * const what)
{
    void * const p = mmap_device_memory(NULL, size, PROT_NOCACHE | PROT_READ | PROT_WRITE, 0, paddr);
    if (p == MAP_FAILED) {
        (void)fprintf(stderr, "msix-rp1: cannot map %s at 0x%llx: %s\n", what, (unsigned long long)paddr, strerror(errno));
        exit(EXIT_FAILURE);
    }
    return (uintptr_t)p;
}

static uint32_t rp1_cfg_rd(const uint32_t reg)
{
    out32(rc + RC_EXT_CFG_INDEX, rp1_cfg_index);
    return in32(rc + RC_EXT_CFG_DATA + (reg & 0xffcU));
}

static void rp1_cfg_wr(const uint32_t reg, const uint32_t val)
{
    out32(rc + RC_EXT_CFG_INDEX, rp1_cfg_index);
    out32(rc + RC_EXT_CFG_DATA + (reg & 0xffcU), val);
}

/* Inbound window n (1..10) register offsets */
static uint32_t ibar_cfg_lo(const unsigned n)
{
    return (n <= 3U) ? (0x402cU + ((n - 1U) * 8U)) : (0x40d4U + ((n - 4U) * 8U));
}

static uint32_t ibar_remap_lo(const unsigned n)
{
    return (n <= 3U) ? (0x40acU + ((n - 1U) * 8U)) : (0x410cU + ((n - 4U) * 8U));
}

static uint64_t ibar_size(const uint32_t cfg_lo)
{
    const uint32_t enc = cfg_lo & 0x1fU;

    if (enc == 0U) {
        return 0;
    }
    if (enc >= 0x1cU) {
        return 1ULL << (enc - 0x1cU + 12U);     /* 4K .. 32K */
    }
    return 1ULL << (enc + 15U);                 /* 64K .. */
}

struct ibar {
    uint64_t pci;
    uint64_t cpu;
    uint64_t size;
    int      enabled;
};

static void ibar_read(const unsigned n, struct ibar * const w)
{
    const uint32_t lo = in32(rc + ibar_cfg_lo(n));
    const uint32_t hi = in32(rc + ibar_cfg_lo(n) + 4U);
    const uint32_t rlo = in32(rc + ibar_remap_lo(n));
    const uint32_t rhi = in32(rc + ibar_remap_lo(n) + 4U);

    w->size = ibar_size(lo);
    w->pci = ((uint64_t)hi << 32) | (lo & ~0x1fU);
    w->cpu = ((uint64_t)(rhi & 0xffU) << 32) | (rlo & ~0xfffU);
    w->enabled = ((rlo & RC_BAR_ENABLE_REMAP) != 0U) && (w->size != 0U);
}

/*
 * Find the PCI address at which RP1 sees host RAM (CPU physical address 0).
 * This is the value to pass to io-usb-otg as "bmstr=".
 */
static int find_ram_window(uint64_t * const pci_base)
{
    unsigned n;

    for (n = 1; n <= RC_NUM_INBOUND_WINS; n++) {
        struct ibar w;
        ibar_read(n, &w);
        if (w.enabled && (w.cpu == 0U) && (w.size >= 0x40000000ULL)) {
            *pci_base = w.pci;
            return 0;
        }
    }
    return -1;
}

/* Make sure a device write to the MSI doorbell address reaches the MIP */
static int setup_mip_window(void)
{
    unsigned n, free_win = 0;

    for (n = 1; n <= RC_NUM_INBOUND_WINS; n++) {
        struct ibar w;
        ibar_read(n, &w);
        vprint(2, "  inbound win %u: pci 0x%llx -> cpu 0x%llx size 0x%llx %s\n", n, (unsigned long long)w.pci,
               (unsigned long long)w.cpu, (unsigned long long)w.size, w.enabled ? "" : "(off)");
        if (w.enabled && (w.pci == MIP_MSI_PCI_ADDR) && (w.cpu == MIP_PADDR)) {
            vprint(1, "MIP doorbell window already present (inbound win %u)\n", n);
            return 0;
        }
        if ((free_win == 0U) && (w.size == 0U)) {
            free_win = n;
        }
    }

    if (free_win == 0U) {
        (void)fprintf(stderr, "msix-rp1: no free PCIe inbound window for the MIP doorbell\n");
        return -1;
    }

    /* 4K window: size encoding 0x1c */
    out32(rc + ibar_cfg_lo(free_win), (uint32_t)(MIP_MSI_PCI_ADDR & 0xfffff000U) | 0x1cU);
    out32(rc + ibar_cfg_lo(free_win) + 4U, (uint32_t)(MIP_MSI_PCI_ADDR >> 32));
    out32(rc + ibar_remap_lo(free_win), (uint32_t)(MIP_PADDR & 0xfffff000U) | RC_BAR_ENABLE_REMAP);
    out32(rc + ibar_remap_lo(free_win) + 4U, (uint32_t)(MIP_PADDR >> 32));
    vprint(1, "MIP doorbell mapped through inbound win %u\n", free_win);
    return 0;
}

/*
 * Fallback for firmware that left no inbound RAM window: let RP1 reach all of
 * host RAM at PCI address 0x10_0000_0000 (64 GB), the layout Linux uses.
 */
static int setup_ram_window(void)
{
    unsigned n;

    for (n = 1; n <= RC_NUM_INBOUND_WINS; n++) {
        struct ibar w;
        ibar_read(n, &w);
        if (w.size == 0U) {
            out32(rc + ibar_cfg_lo(n), 0x15U);                          /* 64 GB */
            out32(rc + ibar_cfg_lo(n) + 4U, (uint32_t)(RAM_PCI_ADDR >> 32));
            out32(rc + ibar_remap_lo(n), RC_BAR_ENABLE_REMAP);
            out32(rc + ibar_remap_lo(n) + 4U, 0U);
            vprint(1, "RAM mapped at PCI 0x%llx through inbound win %u\n", (unsigned long long)RAM_PCI_ADDR, n);
            return 0;
        }
    }
    return -1;
}

static void setup_mip(void)
{
    const uintptr_t mip = map_dev(MIP_PADDR, MIP_SIZE, "MIP");

    /* All vectors: unmasked for the host, masked for the VPU, edge triggered */
    out32(mip + MIP_INT_MASKL_HOST, 0U);
    out32(mip + MIP_INT_MASKH_HOST, 0U);
    out32(mip + MIP_INT_MASKL_VPU, 0xffffffffU);
    out32(mip + MIP_INT_MASKH_VPU, 0xffffffffU);
    out32(mip + MIP_INT_CFGL_HOST, 0xffffffffU);
    out32(mip + MIP_INT_CFGH_HOST, 0xffffffffU);
    (void)munmap_device_memory((void *)mip, MIP_SIZE);
}

static void gic_set_edge(const unsigned rp1_irq)
{
    const unsigned intid = MIP_GIC_IRQ_BASE + rp1_irq;
    const uintptr_t icfgr = map_dev(GICD_PADDR + GICD_ICFGR, 0x100U, "GICD_ICFGR");
    const uint32_t off = (intid / 16U) * 4U;

    out32(icfgr + off, in32(icfgr + off) | (0x2U << ((intid % 16U) * 2U)));
    (void)munmap_device_memory((void *)icfgr, 0x100U);
}

static int find_msix_cap(void)
{
    uint32_t ptr = rp1_cfg_rd(PCI_CAP_PTR) & 0xfcU;
    int guard = 48;

    while ((ptr != 0U) && (guard-- > 0)) {
        const uint32_t hdr = rp1_cfg_rd(ptr);
        if ((hdr & 0xffU) == PCI_CAP_ID_MSIX) {
            return (int)ptr;
        }
        ptr = (hdr >> 8) & 0xfcU;
    }
    return -1;
}


/* Diagnostic dump of the root complex state as left by the firmware */
static void dump_rc(void)
{
    unsigned n;

    (void)fprintf(stderr, "PCIe2 RC @0x%llx:\n", (unsigned long long)RC_PADDR);
    (void)fprintf(stderr, "  status 0x%08x  misc_ctrl 0x%08x  pcie_ctrl 0x%08x\n",
                  in32(rc + RC_PCIE_STATUS), in32(rc + 0x4008U), in32(rc + 0x4064U));
    (void)fprintf(stderr, "  bridge cfg: id 0x%08x cmd/sts 0x%08x class 0x%08x buses 0x%08x\n",
                  in32(rc + 0x00U), in32(rc + RC_CFG_COMMAND), in32(rc + 0x08U), in32(rc + RC_CFG_BUSES));
    (void)fprintf(stderr, "              io 0x%08x mem 0x%08x pref 0x%08x\n",
                  in32(rc + 0x1cU), in32(rc + RC_CFG_MEM_BASE_LIMIT), in32(rc + 0x24U));
    for (n = 0; n < 4U; n++) {
        (void)fprintf(stderr, "  outbound win%u: pci lo/hi 0x%08x/0x%08x base_limit 0x%08x base_hi 0x%08x limit_hi 0x%08x\n", n,
                      in32(rc + RC_CPU_2_PCIE_MEM_WIN0_LO + (n * 8U)), in32(rc + RC_CPU_2_PCIE_MEM_WIN0_HI + (n * 8U)),
                      in32(rc + RC_MEM_WIN0_BASE_LIMIT + (n * 4U)), in32(rc + RC_MEM_WIN0_BASE_HI + (n * 8U)),
                      in32(rc + RC_MEM_WIN0_BASE_HI + 4U + (n * 8U)));
    }
    for (n = 1; n <= RC_NUM_INBOUND_WINS; n++) {
        struct ibar w;
        ibar_read(n, &w);
        (void)fprintf(stderr, "  inbound win%u: cfg 0x%08x/0x%08x remap 0x%08x/0x%08x  pci 0x%llx -> cpu 0x%llx size 0x%llx%s\n", n,
                      in32(rc + ibar_cfg_lo(n)), in32(rc + ibar_cfg_lo(n) + 4U),
                      in32(rc + ibar_remap_lo(n)), in32(rc + ibar_remap_lo(n) + 4U),
                      (unsigned long long)w.pci, (unsigned long long)w.cpu, (unsigned long long)w.size,
                      w.enabled ? "" : " (off)");
    }
}

static void dump_rp1_cfg(void)
{
    unsigned r;

    (void)fprintf(stderr, "RP1 config space (index 0x%x):\n", rp1_cfg_index);
    for (r = 0; r < 0x40U; r += 16U) {
        (void)fprintf(stderr, "  %02x: %08x %08x %08x %08x\n", r, rp1_cfg_rd(r), rp1_cfg_rd(r + 4U),
                      rp1_cfg_rd(r + 8U), rp1_cfg_rd(r + 12U));
    }
}

/*
 * Outbound window 0: CPU 0x1f_0000_0000 -> PCI 0x0, 4 GB (the layout the
 * firmware's device tree describes for pcie2).
 */
#define RP1_WIN_CPU     0x1f00000000ULL
static void force_outbound_window(void)
{
    const uint32_t base_mb = (uint32_t)(RP1_WIN_CPU >> 20);
    const uint32_t limit_mb = (uint32_t)((RP1_WIN_CPU + 0xfffffffbULL) >> 20);

    out32(rc + RC_CPU_2_PCIE_MEM_WIN0_LO, 0U);
    out32(rc + RC_CPU_2_PCIE_MEM_WIN0_HI, 0U);
    out32(rc + RC_MEM_WIN0_BASE_LIMIT, ((limit_mb & 0xfffU) << 20) | ((base_mb & 0xfffU) << 4));
    out32(rc + RC_MEM_WIN0_BASE_HI, (base_mb >> 12) & 0xffU);
    out32(rc + RC_MEM_WIN0_BASE_HI + 4U, (limit_mb >> 12) & 0xffU);
}

/*
 * The firmware maps RP1 at CPU address 0x1c_0000_0000 (outbound window 0,
 * PCI 0x8000_0000). Linux, the SDP 8.0 BSP and every driver command line in
 * this BSP expect the RP1 peripherals at 0x1f_0000_0000, so move the CPU side
 * of the window there. PCI addresses, and with them the RP1 BARs, are untouched.
 */
static void retarget_outbound_window(void)
{
    const uint32_t bl = in32(rc + RC_MEM_WIN0_BASE_LIMIT);
    const uint32_t base_mb = ((in32(rc + RC_MEM_WIN0_BASE_HI) & 0xffU) << 12) | ((bl & 0xfff0U) >> 4);
    const uint32_t limit_mb = ((in32(rc + RC_MEM_WIN0_BASE_HI + 4U) & 0xffU) << 12) | (bl >> 20);
    const uint32_t new_base_mb = (uint32_t)(RP1_WIN_CPU >> 20);
    const uint32_t new_limit_mb = new_base_mb + (limit_mb - base_mb);

    if ((base_mb == new_base_mb) || (limit_mb < base_mb)) {
        return;
    }
    out32(rc + RC_MEM_WIN0_BASE_LIMIT, ((new_limit_mb & 0xfffU) << 20) | ((new_base_mb & 0xfffU) << 4));
    out32(rc + RC_MEM_WIN0_BASE_HI, (new_base_mb >> 12) & 0xffU);
    out32(rc + RC_MEM_WIN0_BASE_HI + 4U, (new_limit_mb >> 12) & 0xffU);
    vprint(1, "outbound window 0 moved from cpu 0x%llx to 0x%llx\n",
           (unsigned long long)base_mb << 20, (unsigned long long)RP1_WIN_CPU);
}

static void usage(void)
{
    (void)fprintf(stderr,
        "msix-rp1 [-v] [-i irq[,irq...]]   configure RP1 MSI-X interrupts\n"
        "msix-rp1 -B                       print the bus-master (DMA) offset for bmstr=\n"
        "msix-rp1 -d                       dump the PCIe root complex / RP1 state, change nothing\n"
        "msix-rp1 -f ...                   also assign bus numbers, BARs and windows if the\n"
        "                                  firmware left the RP1 link unconfigured\n"
        "msix-rp1 -c                       disable RP1 MSI-X\n");
}

int main(const int argc, char * const argv[])
{
    unsigned irq_list[RP1_NUM_IRQS] = { 6, RP1_IRQ_USBHOST0, RP1_IRQ_USBHOST1 };
    unsigned irq_num = 3;
    int opt, cleanup = 0, query_bmstr = 0, dump_only = 0, force = 0;

    while ((opt = getopt(argc, argv, "Bcdfi:v")) != -1) {
        switch (opt) {
            case 'B':
                query_bmstr = 1;
                break;
            case 'c':
                cleanup = 1;
                break;
            case 'd':
                dump_only = 1;
                break;
            case 'f':
                force = 1;
                break;
            case 'i': {
                char *str = optarg;
                irq_num = 0;
                while ((str != NULL) && (*str != '\0') && (irq_num < RP1_NUM_IRQS)) {
                    char *end;
                    const unsigned long v = strtoul(str, &end, 0);
                    if ((end == str) || (v >= RP1_NUM_IRQS)) {
                        (void)fprintf(stderr, "msix-rp1: invalid IRQ '%s', must be 0..%u\n", str, RP1_NUM_IRQS - 1U);
                        return EXIT_FAILURE;
                    }
                    irq_list[irq_num++] = (unsigned)v;
                    str = (*end == ',') ? (end + 1) : NULL;
                }
                break;
            }
            case 'v':
                ++verbose;
                break;
            default:
                usage();
                return EXIT_FAILURE;
        }
    }

    rc = map_dev(RC_PADDR, RC_SIZE, "PCIe2 root complex");

    if (query_bmstr) {
        uint64_t base;
        if (find_ram_window(&base) != 0) {
            (void)fprintf(stderr, "msix-rp1: no PCIe inbound RAM window found, assuming default\n");
            base = RAM_PCI_ADDR;
        }
        (void)printf("0x%llx\n", (unsigned long long)base);
        return EXIT_SUCCESS;
    }

    if (dump_only || (verbose > 1)) {
        dump_rc();
    }

    /* A config access to the secondary bus without a link aborts the CPU: check first */
    const uint32_t status = in32(rc + RC_PCIE_STATUS);
    if ((status & (RC_PCIE_STATUS_DL_ACTIVE | RC_PCIE_STATUS_PHYLINKUP)) !=
        (RC_PCIE_STATUS_DL_ACTIVE | RC_PCIE_STATUS_PHYLINKUP)) {
        (void)fprintf(stderr, "msix-rp1: PCIe link to RP1 is down (status 0x%x).\n"
                              "          Is 'pciex4_reset=0' set in config.txt?\n", status);
        return EXIT_FAILURE;
    }

    uint32_t buses = in32(rc + RC_CFG_BUSES);
    unsigned sec_bus = (buses >> 8) & 0xffU;
    if (sec_bus == 0U) {
        if (dump_only) {
            sec_bus = 1;        /* peek at where RP1 would be */
        }
        else {
            /*
             * The firmware talks to RP1 without ever programming the bridge's
             * bus numbers; until they are set, bus 1 config reads return ~0.
             * primary 0, secondary 1, subordinate 1
             */
            out32(rc + RC_CFG_BUSES, (buses & 0xff000000U) | 0x00010100U);
            buses = in32(rc + RC_CFG_BUSES);
            sec_bus = (buses >> 8) & 0xffU;
            vprint(1, "assigned bus numbers to the root bridge (0x%x)\n", buses);
        }
        if (sec_bus == 0U) {
            (void)fprintf(stderr, "msix-rp1: cannot assign bus numbers to the root bridge (0x%x)\n", buses);
            return EXIT_FAILURE;
        }
    }
    rp1_cfg_index = sec_bus << 20;      /* bus:0.0 */

    const uint32_t id = rp1_cfg_rd(0);
    if (dump_only || (verbose > 1)) {
        dump_rp1_cfg();
    }
    if (dump_only) {
        return EXIT_SUCCESS;
    }
    if (id != RP1_VID_DID) {
        (void)fprintf(stderr, "msix-rp1: unexpected device %04x:%04x on bus %u (expected RP1 1de4:0001)\n",
                      id & 0xffffU, id >> 16, sec_bus);
        return EXIT_FAILURE;
    }

    uint32_t bar0 = rp1_cfg_rd(PCI_BAR(0)) & ~0xfU;
    uint32_t bar1 = rp1_cfg_rd(PCI_BAR(1)) & ~0xfU;
    uint32_t cmd = rp1_cfg_rd(PCI_COMMAND);

    if (force) {
        /*
         * Give RP1 the address layout Linux uses when it is missing:
         * BAR1 (peripherals, 4M) at 0, BAR2 (SRAM, 64K) at 0x400000,
         * BAR0 (MSI-X table, 16K) at 0x410000; bridge window 0..0x5fffff.
         */
        if (bar0 == bar1) {
            uint32_t bar2 = rp1_cfg_rd(PCI_BAR(2)) & ~0xfU;
            rp1_cfg_wr(PCI_COMMAND, cmd & 0xfff8U);     /* decode off while moving BARs */
            if (bar2 == bar1) {
                rp1_cfg_wr(PCI_BAR(1), 0x00000000U);
                rp1_cfg_wr(PCI_BAR(2), 0x00400000U);
                bar2 = 0x00400000U;
            }
            rp1_cfg_wr(PCI_BAR(0), (bar2 == 0x00400000U) ? 0x00410000U : (bar2 + 0x00010000U));
            bar0 = rp1_cfg_rd(PCI_BAR(0)) & ~0xfU;
            bar1 = rp1_cfg_rd(PCI_BAR(1)) & ~0xfU;
            (void)fprintf(stderr, "msix-rp1: assigned RP1 BARs: BAR0 0x%x BAR1 0x%x BAR2 0x%x\n", bar0, bar1,
                          rp1_cfg_rd(PCI_BAR(2)) & ~0xfU);
        }
        if ((in32(rc + RC_CFG_MEM_BASE_LIMIT) >> 16) < 0x0050U) {
            out32(rc + RC_CFG_MEM_BASE_LIMIT, 0x00500000U);
        }
        if ((in32(rc + RC_MEM_WIN0_BASE_HI) & 0xffU) == 0U) {
            force_outbound_window();
            (void)fprintf(stderr, "msix-rp1: programmed outbound window 0\n");
        }
        cmd = (cmd & 0xffffU) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
        rp1_cfg_wr(PCI_COMMAND, cmd);
        out32(rc + RC_CFG_COMMAND, (in32(rc + RC_CFG_COMMAND) & 0xffffU) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    }

    if ((in32(rc + RC_MEM_WIN0_BASE_HI) & 0xffU) != 0U) {
        retarget_outbound_window();
    }

    /* CPU address of the PCI memory window (outbound window 0) */
    const uint32_t win_bl = in32(rc + RC_MEM_WIN0_BASE_LIMIT);
    const uint64_t win_cpu = ((uint64_t)(in32(rc + RC_MEM_WIN0_BASE_HI) & 0xffU) << 32) |
                             ((uint64_t)((win_bl & 0xfff0U) >> 4) << 20);
    const uint64_t win_pci = ((uint64_t)in32(rc + RC_CPU_2_PCIE_MEM_WIN0_HI) << 32) |
                             in32(rc + RC_CPU_2_PCIE_MEM_WIN0_LO);

    vprint(1, "RP1 on bus %u: cmd 0x%04x BAR0 0x%08x BAR1 0x%08x BAR2 0x%08x\n", sec_bus, cmd & 0xffffU,
           bar0, bar1, rp1_cfg_rd(PCI_BAR(2)) & ~0xfU);
    vprint(1, "outbound win0: cpu 0x%llx -> pci 0x%llx, bridge mem base/limit 0x%08x\n",
           (unsigned long long)win_cpu, (unsigned long long)win_pci, in32(rc + RC_CFG_MEM_BASE_LIMIT));

    if ((win_cpu == 0U) || (bar0 < win_pci) || (bar1 < win_pci)) {
        (void)fprintf(stderr, "msix-rp1: PCIe outbound window is not set up (cpu 0x%llx pci 0x%llx); try -f\n",
                      (unsigned long long)win_cpu, (unsigned long long)win_pci);
        return EXIT_FAILURE;
    }
    if (bar0 == bar1) {
        (void)fprintf(stderr, "msix-rp1: RP1 BAR0 (MSI-X table) is not assigned by the firmware (BAR0 = BAR1 = 0x%x); try -f\n", bar0);
        return EXIT_FAILURE;
    }

    const uint64_t bar0_cpu = win_cpu + (bar0 - win_pci);
    const uint64_t bar1_cpu = win_cpu + (bar1 - win_pci);

    const int cap = find_msix_cap();
    if (cap < 0) {
        (void)fprintf(stderr, "msix-rp1: RP1 has no MSI-X capability\n");
        return EXIT_FAILURE;
    }
    uint32_t ctrl = rp1_cfg_rd((uint32_t)cap);
    const unsigned nvec = ((ctrl >> 16) & 0x7ffU) + 1U;
    const uint32_t tbl = rp1_cfg_rd((uint32_t)cap + 4U);
    if ((tbl & 0x7U) != 0U) {
        (void)fprintf(stderr, "msix-rp1: MSI-X table is in BAR%u, expected BAR0\n", tbl & 0x7U);
        return EXIT_FAILURE;
    }
    const size_t tbl_size = (size_t)nvec * 16U;
    const uintptr_t table = map_dev(bar0_cpu + (tbl & ~0x7U), tbl_size, "RP1 MSI-X table");
    const uintptr_t msix_cfg = map_dev(bar1_cpu + RP1_MSIX_CFG_OFFSET, RP1_MSIX_CFG_SIZE, "RP1 MSIX_CFG");
    unsigned i;

    if (cleanup) {
        for (i = 0; i < nvec; i++) {
            out32(msix_cfg + RP1_REG_CLR + RP1_MSIX_CFG(i), MSIX_CFG_ENABLE);
            out32(table + (i * 16U) + 12U, 1U);
        }
        rp1_cfg_wr((uint32_t)cap, (ctrl & ~MSIX_CTRL_ENABLE) | MSIX_CTRL_MASKALL);
        (void)fprintf(stderr, "RP1 MSI-X disabled\n");
        return EXIT_SUCCESS;
    }

    (void)fprintf(stderr, "RP1 supports %u MSI-X IRQs, setting up %u\n", nvec, irq_num);

    /* MSI-X messages are memory writes from RP1: it must be a bus master */
    cmd = (cmd & 0xffffU) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INTX_DISABLE;
    rp1_cfg_wr(PCI_COMMAND, cmd);
    out32(rc + RC_CFG_COMMAND, (in32(rc + RC_CFG_COMMAND) & 0xffffU) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    if (setup_mip_window() != 0) {
        return EXIT_FAILURE;
    }
    setup_mip();

    /* Quiesce while the table is rewritten */
    rp1_cfg_wr((uint32_t)cap, ctrl | MSIX_CTRL_ENABLE | MSIX_CTRL_MASKALL);

    /* Vector n -> doorbell write of <n> -> GIC interrupt 160 + n. Everything masked by default */
    for (i = 0; i < nvec; i++) {
        out32(table + (i * 16U) + 0U, (uint32_t)MIP_MSI_PCI_ADDR);
        out32(table + (i * 16U) + 4U, (uint32_t)(MIP_MSI_PCI_ADDR >> 32));
        out32(table + (i * 16U) + 8U, i);
        out32(table + (i * 16U) + 12U, 1U);
    }

    for (i = 0; i < irq_num; i++) {
        const unsigned irq = irq_list[i];
        if (irq >= nvec) {
            (void)fprintf(stderr, "msix-rp1: IRQ %u is beyond the %u vectors of RP1, skipped\n", irq, nvec);
            continue;
        }
        gic_set_edge(irq);
        out32(table + (irq * 16U) + 12U, 0U);

        if ((irq == RP1_IRQ_USBHOST0) || (irq == RP1_IRQ_USBHOST1)) {
            /* edge source: one message per event, no acknowledge */
            out32(msix_cfg + RP1_REG_CLR + RP1_MSIX_CFG(irq), MSIX_CFG_IACK_EN);
            out32(msix_cfg + RP1_REG_SET + RP1_MSIX_CFG(irq), MSIX_CFG_ENABLE);
        }
        else {
            /*
             * level source: RP1 sends one message and then waits for the driver to
             * write MSIX_CFG.IACK once it has serviced the device (devc-serpl011
             * does this for the RP1 UARTs).
             */
            out32(msix_cfg + RP1_REG_SET + RP1_MSIX_CFG(irq), MSIX_CFG_ENABLE | MSIX_CFG_IACK_EN);
            out32(msix_cfg + RP1_REG_SET + RP1_MSIX_CFG(irq), MSIX_CFG_IACK);
        }
        vprint(1, "RP1 irq %u -> GIC irq %u, MSIX_CFG 0x%x\n", irq, MIP_GIC_IRQ_BASE + irq,
               in32(msix_cfg + RP1_MSIX_CFG(irq)));
    }

    ctrl = rp1_cfg_rd((uint32_t)cap);
    rp1_cfg_wr((uint32_t)cap, (ctrl | MSIX_CTRL_ENABLE) & ~MSIX_CTRL_MASKALL);

    uint64_t ram_pci;
    if (find_ram_window(&ram_pci) != 0) {
        if (setup_ram_window() != 0) {
            (void)fprintf(stderr, "msix-rp1: warning: no PCIe inbound RAM window, RP1 DMA (USB) will not work\n");
        }
        ram_pci = RAM_PCI_ADDR;
    }
    if ((ram_pci != RAM_PCI_ADDR) || (verbose > 0)) {
        (void)fprintf(stderr, "RP1 DMA offset (io-usb-otg bmstr=) is 0x%llx\n", (unsigned long long)ram_pci);
    }

    return EXIT_SUCCESS;
}
