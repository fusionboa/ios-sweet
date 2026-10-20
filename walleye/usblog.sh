#!/bin/bash
# Live kernel console from the phone over USB (kernel boot-arg sweetusb=2):
# tools/sweet-usbnet.sh, but the interface is set up through NetworkManager
# (no sudo for the desktop user).
#
#   walleye/usblog.sh [LOGFILE]     (default walleye/logs/live-TIME.txt)
set -euo pipefail
cd "$(dirname "$0")"
out=${1:-logs/live-$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$out")"
mac=02:00:5e:77:ee:01
echo "waiting for the phone's USB Ethernet ($mac)..." >&2
until ip -o link | grep -qi "$mac"; do sleep 1; done
ifname=$(ip -o link | grep -i "$mac" | awk -F': ' '{print $2}' | cut -d@ -f1)
nmcli connection delete walleye-usb >/dev/null 2>&1 || true
nmcli connection add type ethernet con-name walleye-usb ifname "$ifname" \
	ipv4.method manual ipv4.addresses 192.168.7.1/24 ipv6.method disabled >/dev/null
nmcli connection up walleye-usb >/dev/null
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140 -> $out" >&2
python3 -u -c '
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 5140))
while True:
    d, _ = s.recvfrom(65536)
    sys.stdout.write(d.decode("utf-8", "replace"))
    sys.stdout.flush()
' | tee -a "$out"
