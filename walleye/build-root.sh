#!/bin/bash
# Build the iOS 16.7.16 root image for walleye from the IPSW DMGs (needs sudo).
# Run in a real terminal:  bash ~/ios-sweet/walleye/build-root.sh
#
# Fedora's apfs-fuse is a 2020 snapshot without LZBITMAP decompression (iOS 16
# compresses most files with it: "Input/output error" from rsync), so this
# builds current upstream apfs-fuse into ~/.local/apfs-fuse first.
set -euo pipefail
cd ~/ios-sweet
D=~/ios-sweet-ipsw/dmg
IMG=~/ios-sweet-ipsw/ios16-root.img
APFS_SRC=~/.local/src/apfs-fuse
APFS_BIN=~/.local/apfs-fuse/bin
sudo -v

# Leftovers of an earlier failed run.
for m in /tmp/tmp.*/root /tmp/tmp.*/os /tmp/tmp.*/sysos /tmp/tmp.*/appos /mnt/iosroot; do
	if mountpoint -q "$m" 2>/dev/null; then
		echo "unmounting leftover $m"
		sudo umount "$m"
	fi
done

if [[ ! -x $APFS_BIN/apfs-fuse ]]; then
	rpm -q fuse3-devel bzip2-devel >/dev/null 2>&1 || sudo dnf install -y fuse3-devel bzip2-devel
	[[ -d $APFS_SRC ]] || git clone --recursive https://github.com/sgan81/apfs-fuse.git "$APFS_SRC"
	cmake -S "$APFS_SRC" -B "$APFS_SRC/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"
	cmake --build "$APFS_SRC/build" -j"$(nproc)"
	mkdir -p "$APFS_BIN"
	cp "$APFS_SRC/build/apfs-fuse" "$APFS_BIN/"
fi
export PATH=$APFS_BIN:$PATH
echo "using $(command -v apfs-fuse)"

rm -f "$IMG"
tools/mkiosroot.sh $D/087-86698-044.dmg $D/087-86593-047.dmg $D/087-86715-047.dmg "$IMG" 16G
sudo fsck.hfsplus -fy "$IMG"
sudo mkdir -p /mnt/iosroot
sudo mount -t hfsplus -o loop,force "$IMG" /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
echo ROOT-IMAGE-DONE
