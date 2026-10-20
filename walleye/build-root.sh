#!/bin/bash
# Build the iOS 16.7.16 root image for walleye from the IPSW DMGs (needs sudo).
# Run in a real terminal:  bash ~/ios-sweet/walleye/build-root.sh
set -euo pipefail
cd ~/ios-sweet
D=~/ios-sweet-ipsw/dmg
IMG=~/ios-sweet-ipsw/ios16-root.img
sudo -v
tools/mkiosroot.sh $D/087-86698-044.dmg $D/087-86593-047.dmg $D/087-86715-047.dmg "$IMG" 16G
sudo fsck.hfsplus -fy "$IMG"
sudo mkdir -p /mnt/iosroot
sudo mount -t hfsplus -o loop,force "$IMG" /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
echo ROOT-IMAGE-DONE
