#!/bin/bash
# Kernel -> walleye boot image: ADT (walleye/tools/mkadt.py), SWEETXNU payload
# (tools/mkpayload.py), shim with the payload linked in (walleye/shim).
#
#   walleye/mkboot.sh KERNEL [BOOT-ARGS]    -> walleye/shim/out/shim-boot.img
#   fastboot boot walleye/shim/out/shim-boot.img
set -euo pipefail
cd "$(dirname "$0")"
kernel=$(realpath "$1")
# Sweet's drivers poke SM7150 MMIO (SPMI, GENI touch, RPMh, USB): off until ported.
args=${2:-'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 sweetkeys=0 sweettouch=0 sweetusb=0 sweetlog=0 wdt=-1 wdt_test=no_user_checkin'}
mkdir -p out
python3 tools/mkadt.py out/walleye.adt
python3 ../tools/mkpayload.py "$kernel" out/walleye.adt out/payload.bin --cmdline "$args"
shim/build.sh "$(realpath out/payload.bin)"
