#!/bin/bash
# Kernel -> walleye boot image: ADT (walleye/tools/mkadt.py), SWEETXNU payload
# (tools/mkpayload.py), shim with the payload linked in (walleye/shim).
#
#   walleye/mkboot.sh KERNEL [BOOT-ARGS]    -> walleye/shim/out/shim-boot.img
#   fastboot boot walleye/shim/out/shim-boot.img
set -euo pipefail
cd "$(dirname "$0")"
kernel=$(realpath "$1")
# Sweet's README boot-args, one core. Sweet's drivers poke SM7150 MMIO (SPMI,
# GENI touch, RPMh): off until ported. sweetusb=2: USB live log (walleye/usblog.sh). sweetlog keeps its 4 MiB ring at
# the end of userdata, past the 16G iOS root.
args=${2:-'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetkeys=0 sweettouch=0 sweetusb=2'}
mkdir -p out
python3 tools/mkadt.py out/walleye.adt
python3 ../tools/mkpayload.py "$kernel" out/walleye.adt out/payload.bin --cmdline "$args"
shim/build.sh "$(realpath out/payload.bin)"
