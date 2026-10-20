#!/bin/bash
# Kernel -> walleye boot image: ADT (walleye/tools/mkadt.py), SWEETXNU payload
# (tools/mkpayload.py), shim with the payload linked in (walleye/shim).
#
#   walleye/mkboot.sh KERNEL [BOOT-ARGS]    -> walleye/shim/out/shim-boot.img
#   fastboot boot walleye/shim/out/shim-boot.img
set -euo pipefail
cd "$(dirname "$0")"
kernel=$(realpath "$1")
# Sweet's README boot-args with walleye drivers (patch 0105): the four big cores
# (cpus=4; a core that fails to start is left off), Synaptics touch, GPIO keys,
# PMI8998 battery. sweetusb=2: USB live log (walleye/usblog.sh). sweetfb_hz:
# panel refreshes per second. sweetslot=0 only reports the A/B boot slot
# attributes; the flashed image uses sweetslot=1 (mark the slot successful).
# sweetlog keeps its 4 MiB ring at the end of userdata, past the 16G iOS root.
args=${2:-'debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetusb=2 sweetfb_hz=20 sweetslot=0'}
mkdir -p out
python3 tools/mkadt.py out/walleye.adt
python3 ../tools/mkpayload.py "$kernel" out/walleye.adt out/payload.bin --cmdline "$args"
shim/build.sh "$(realpath out/payload.bin)"
