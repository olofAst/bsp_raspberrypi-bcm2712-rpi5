#!/bin/sh
# The UART10 interrupt differs between chip steppings: 153 on Pi 5 rev 1.0,
# 152 on BCM2712 D0 boards (Pi 5 rev 1.1+, CM5, Pi 500). Startup works it out
# and publishes it as UART10_IRQ in the environment of procnto.
# (the boot script does not inherit procnto's environment, so read it from there)
IRQ=$(pidin -p 1 env | sed -n 's/.*UART10_IRQ=([0-9]*).*/1/p')
if [ -z "$IRQ" ]; then IRQ=153; fi
devc-serpl011 -b115200 -c44236800 -e -F -u10 0x107d001000^2,$IRQ
