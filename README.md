# Raspberry Pi 5 BSP

QNX Board Support Package (BSP) for the Raspberry Pi 5

## QNX SDP 7.1 port (branch `qnx_71`)

This branch ports the SDP 8.0 BSP to **QNX SDP 7.1**. It boots to a shell in an emulator (see below) but has
**not yet been run on real hardware**; USB in particular is untested.

### What works / what is ported

| Component | State on SDP 7.1 |
|---|---|
| Startup (`startup-bcm2712-rpi5`) | Ported to the SDP 7.1 startup library (taken from the 7.1 Raspberry Pi 4 BSP, plus a Cortex-A76 id) |
| Debug serial (`devc-serpl011`) | 7.1 PL011 driver, with the RP1 UART interrupt acknowledge added |
| SD card (`devb-sdmmc-bcm2712`) | 7.1 sdmmc driver with the Pi 5 board layer |
| RP1 interrupts (`msix-rp1`) | Rewritten: programs MSI-X, the MIP and the PCIe inbound window directly, no `pci-server` |
| USB host | Stock SDP 7.1 `devu-hcd-xhci.so` on the two RP1 xHCI controllers |
| GPIO, mailbox, watchdog, fan utilities | Build unchanged |
| Network | `io-pkt` with USB Ethernet adapters (`asix`, `smsc9500`, `usbnet`) |
| On-board Ethernet, Wi-Fi, I2C, SPI, PCIe expansion port | **Not ported** (no SDP 7.1 drivers / frameworks); the I2C and SPI sources are kept but excluded from the build with `Makefile.dnm` |

### Build

```
source <path to>/qnx710/qnxsdp-env.sh
make
```

The result is `images/ifs-rpi5.bin`.

### Boot

Prepare a FAT32 SD card with the Raspberry Pi firmware files (`bcm2712-rpi-5-b.dtb`,
`bcm2712d0-rpi-5-b.dtb`, `overlays/`) from https://github.com/raspberrypi/firmware/tree/stable/boot,
then copy `images/ifs-rpi5.bin` and `images/sdboot/config.txt` onto it.

`pciex4_reset=0` in `config.txt` is **mandatory**: SDP 7.1 has no BCM2712 PCIe driver, so the BSP
relies on the firmware leaving the PCIe link to the RP1 I/O controller up. Without it `msix-rp1`
reports that the link is down and USB does not start.

The console is the 3-pin debug UART header (115200 8N1).

### Emulator

`images/run-qemu.sh` boots the image on the minimal `raspi5` machine of the
[Ebiroll/qemu-stm32](https://github.com/Ebiroll/qemu-stm32) QEMU fork (`hw/arm/raspi5.c`):

```
QEMU=<qemu build dir>/qemu-system-aarch64 images/run-qemu.sh [sd.img] [bcm2712-rpi-5-b.dtb]
```

That machine has the four Cortex-A76 cores, GIC, debug UART, mailbox and SD controller. It has no
PCIe/RP1, so `msix-rp1` reports that the link is down and USB, the RP1 UARTs and RP1 GPIO are not
exercised. Verified in the emulator: startup, SMP bring-up through PSCI, the serial console,
the SD driver (mount, write, read back) and the shell.

### If USB does not come up

Run `msix-rp1 -vv -i 6,31,36` on the target. It prints the RP1 BARs, the PCIe windows and the
per-interrupt configuration it found and programmed. `msix-rp1 -B` prints the DMA offset that
`usb_start.sh` passes to `io-usb-otg` as `bmstr=`.

## Usage (SDP 8.0 documentation)

For more information on this BSP, including how to build it and load it onto your board, see the official BSP User Guide:
* [BSP User Guide](https://www.qnx.com/developers/docs/BSP8.0/com.qnx.doc.bsp_raspberrypi.bcm2712.rpi5_8.0/topic/about.html)
* [Build the BSP](https://www.qnx.com/developers/docs/BSP8.0/com.qnx.doc.bsp_raspberrypi.bcm2712.rpi5_8.0/topic/common/bsp_build.html)

Note that this Git version of the BSP differs from the QNX Software Center (QSC) version in a couple ways:
* The Git version contains source code only (no prebuilt binaries).
* The Git version is not fully supported by the QNX Momentics IDE.
* The QSC version is delivered as a ZIP file. (You may notice references to a ZIP file in the BSP User Guide.)

## Preparation

* You need to have a local QNX SDP to build this project. To start, get a QNX SDP 8.0 license and follow the official instructions to install QNX SDP 8.0 to your host machine.
* In addition to a baseline SDP 8.0 installation, you will also need to confirm that you have installed all of the packages listed in "source.xml".

## Notice on Software Maturity and Quality

The software included in this repository is classified under our Software Maturity Standard as Experimental Software – Software Quality and Maturity Level (SQML) 1.

As defined in the [QNX Development License Agreement](http://www.qnx.com/download/feature.html?programid=68651), Experimental Software represents early-stage deliverables intended for evaluation or proof-of-concept purposes.

SQML 1 indicates that this software is provided without one or more of the following:
* Formal requirements
* Formal design or architecture
* Formal testing
* Formal support
* Formal documentation
* Certifications of any type
* End-of-Life or End-of-Support policy

Additionally, this software is not monitored or scanned under our Cybersecurity Management Standard.

No warranties, guarantees, or claims are offered at this SQML level.
