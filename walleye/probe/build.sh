#!/bin/bash
# Build the walleye probe as a boot.img v0 with the stock DTB appended to the
# Image, the way walleye's ABL expects (stock: Image.lz4-dtb; gzip is the format every msm8998 ABL decompresses).
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
DTBS=../../dtb/stock_dtbs.bin	# from the stock boot_b kernel, see ../README.md
mkdir -p out
for f in start.S probe.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
abs=$(aarch64-linux-gnu-readelf -r out/probe.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/probe.o -o out/probe.elf
$OBJCOPY -O binary out/probe.elf out/Image
size=$(aarch64-linux-gnu-nm out/probe.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
gzip -9nc out/Image > out/Image.gz
cat out/Image.gz "$DTBS" > out/Image-dtb
(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
mkbootimg --header_version 0 --kernel out/Image-dtb --ramdisk out/ramdisk.bin \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
	--pagesize 4096 --os_version 15.0.0 --os_patch_level 2026-07 \
	--cmdline "console=null" -o out/probe-boot.img
ls -l out/Image out/probe-boot.img
