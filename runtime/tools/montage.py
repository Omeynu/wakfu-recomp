#!/usr/bin/env python3
"""Planche de captures PNG (RGBA 8 bits, sans filtre ou filtres standard) réduites d'un facteur donné.
Usage : python3 -I montage.py <sortie.png> <facteur> <colonnes> <image1.png> [image2.png ...]"""
import struct, sys, zlib

def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path
    i, idat, w = 8, b'', 0
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]; t = d[i + 4:i + 8]; c = d[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', c[:10])
            assert depth == 8 and ctype == 6, 'RGBA 8 bits attendu'
        elif t == b'IDAT':
            idat += c
        i += 12 + n
    raw, stride, out, prev = zlib.decompress(idat), w * 4, bytearray(), bytearray(w * 4)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0; b = prev[x]; c = prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line; prev = line
    return w, h, out

def write_png(path, w, h, rgba):
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

out, k, cols, files = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
imgs = [read_png(f) for f in files]
tw, th = max(i[0] for i in imgs) // k, max(i[1] for i in imgs) // k
rows = (len(imgs) + cols - 1) // cols
W, H = tw * cols, th * rows
canvas = bytearray(W * H * 4)
for n, (w, h, px) in enumerate(imgs):
    ox, oy = (n % cols) * tw, (n // cols) * th
    for y in range(h // k):
        for x in range(w // k):
            s = ((y * k) * w + x * k) * 4; d = ((oy + y) * W + ox + x) * 4
            canvas[d:d + 4] = px[s:s + 4]
write_png(out, W, H, canvas)
print('%s : %dx%d (%d images)' % (out, W, H, len(imgs)))
