#!/bin/sh
# Boot the SDP 7.1 Raspberry Pi 5 image in QEMU.
#
# Needs a QEMU with the minimal "raspi5" machine (hw/arm/raspi5.c in the
# Ebiroll/qemu-stm32 fork). That machine models the CPUs, GIC, debug UART,
# VideoCore mailbox and SD controller only: the PCIe link to RP1 is absent, so
# msix-rp1 reports "link is down" and USB does not start. That is expected.
#
#   QEMU=/path/to/qemu-system-aarch64 ./run-qemu.sh [sd.img] [file.dtb]
#
# The console is on stdio; leave with Ctrl-A X.
QEMU=${QEMU:-qemu-system-aarch64}
DIR=$(dirname "$0")
SD=${1:+-drive file=$1,if=sd,format=raw}
DTB=${2:+-dtb $2}

exec "$QEMU" -M raspi5 -m 2G -kernel "$DIR/ifs-rpi5.bin" $DTB $SD \
    -serial mon:stdio -display none
