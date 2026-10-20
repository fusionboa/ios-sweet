#!/usr/bin/env python3
"""Flash a big raw image to a partition in 1 GiB windows of Android sparse images.

  flash-windows.py IMAGE PARTITION [--window-mib 1024] [--start N]

fastboot reads a raw image into memory to sparse it (OOM-killed on a 16 GiB
image with 7 GiB RAM), and a full img2simg copy needs as much disk again.
Here each window becomes one sparse file: DONT_CARE before and after it,
inside it FILL chunks for all-zero blocks (they still overwrite the old
partition contents) and RAW chunks for the rest. Only one window file exists
at a time; --start resumes at window N.
"""
import argparse, os, struct, subprocess, sys

MAGIC = 0xed26ff3a
RAW, FILL, DONT_CARE = 0xcac1, 0xcac2, 0xcac3
BS = 4096
ZERO = bytes(BS)


def write_window(img, out, total_blks, first, count):
    chunks = []  # (type, nblocks, payload offset in img or None)

    def add(kind, n, off=None):
        if chunks and chunks[-1][0] == kind and kind != RAW:
            chunks[-1] = (kind, chunks[-1][1] + n, None)
        elif chunks and kind == RAW and chunks[-1][0] == RAW and chunks[-1][2] + chunks[-1][1] * BS == off:
            chunks[-1] = (RAW, chunks[-1][1] + n, chunks[-1][2])
        else:
            chunks.append((kind, n, off))

    if first:
        add(DONT_CARE, first)
    with open(img, 'rb') as f:
        f.seek(first * BS)
        for i in range(count):
            b = f.read(BS)
            if b == ZERO:
                add(FILL, 1)
            else:
                add(RAW, 1, (first + i) * BS)
    rest = total_blks - first - count
    if rest:
        add(DONT_CARE, rest)

    with open(img, 'rb') as f, open(out, 'wb') as o:
        o.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, total_blks, len(chunks), 0))
        for kind, n, off in chunks:
            if kind == RAW:
                o.write(struct.pack('<HHII', RAW, 0, n, 12 + n * BS))
                f.seek(off)
                left = n * BS
                while left:
                    buf = f.read(min(left, 8 << 20))
                    o.write(buf)
                    left -= len(buf)
            elif kind == FILL:
                o.write(struct.pack('<HHII', FILL, 0, n, 16) + bytes(4))
            else:
                o.write(struct.pack('<HHII', DONT_CARE, 0, n, 12))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image'); ap.add_argument('partition')
    ap.add_argument('--window-mib', type=int, default=1024)
    ap.add_argument('--start', type=int, default=0)
    a = ap.parse_args()
    size = os.path.getsize(a.image)
    if size % BS:
        sys.exit('image size must be a multiple of 4096')
    total = size // BS
    win = a.window_mib * (1 << 20) // BS
    nwin = (total + win - 1) // win
    out = os.path.join(os.path.dirname(os.path.abspath(a.image)), 'window.simg')
    for w in range(a.start, nwin):
        first = w * win
        count = min(win, total - first)
        write_window(a.image, out, total, first, count)
        print(f'window {w + 1}/{nwin}: {os.path.getsize(out) >> 20} MiB sparse', flush=True)
        r = subprocess.run(['fastboot', '-S', '512M', 'flash', a.partition, out])
        if r.returncode:
            sys.exit(f'fastboot failed on window {w}; resume with --start {w}')
    os.remove(out)
    print('all windows flashed')


if __name__ == '__main__':
    main()
