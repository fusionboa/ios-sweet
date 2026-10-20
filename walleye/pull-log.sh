#!/bin/bash
# Pull the kernel console log (sweetlog ring, last 4 MiB of userdata) after an
# iOS boot, without photos: temp-boot TWRP (nothing is flashed), read the ring
# over adb, then go back to fastboot.
#
#   1. on the phone: hold Power + Volume Down until the fastboot screen
#   2. walleye/pull-log.sh [OUT.txt]      (default: walleye/logs/boot-TIME.txt)
set -euo pipefail
cd "$(dirname "$0")"
out=${1:-logs/boot-$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$out")"
twrp=../recovery/twrp-walleye.img	# twrp-3.3.0-0-walleye.img, sha256 e7edcfb5...

echo "waiting for fastboot..."
timeout 120 bash -c 'until fastboot devices | grep -q .; do sleep 1; done'
fastboot boot "$twrp"
echo "waiting for TWRP adb..."
timeout 180 adb wait-for-recovery
sleep 3
../tools/sweet-log-pull.sh "$out"
adb reboot bootloader
echo "log: $(realpath "$out") ($(wc -l < "$out") lines)"
tail -40 "$out"
