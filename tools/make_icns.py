#!/usr/bin/env python3
# Build a classic-friendly .icns (is32/il32/ih32/it32 + 8-bit masks, plus
# ic08/ic09 PNG for newer OSes) from the square source art. Run on a Mac with PIL.
# The OS 9 app itself Rez's ICN#/icl8/ics# from this same art at packaging time
# (Fio 3+); this .icns is the repo/preview artifact and matches the siblings.
import struct, io, sys
from PIL import Image, ImageDraw

SRC = sys.argv[1] if len(sys.argv) > 1 else "design/icon/casquinha-icon.png"
OUT = sys.argv[2] if len(sys.argv) > 2 else "Resources/Casquinha.icns"

src = Image.open(SRC).convert("RGBA")
w, h = src.size

# The source art has an opaque WHITE frame around the rounded-rect icon. Flood-fill
# the white background (from all four corners) to transparent and crop to the
# rounded rect so it fills the tile edge-to-edge.
rgb = src.convert("RGB")
SENT = (255, 0, 255)
for seed in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
    ImageDraw.floodfill(rgb, seed, SENT, thresh=48)
alpha = [0 if p == SENT else 255 for p in rgb.getdata()]
mask = Image.new("L", (w, h)); mask.putdata(alpha)
src.putalpha(mask)

bb = src.getbbox()                 # tight bounds of the rounded rect
if bb:
    src = src.crop(bb)
sw, sh = src.size
s = min(sw, sh)                    # square it (rounded rect is ~square)
src = src.crop(((sw - s)//2, (sh - s)//2, (sw - s)//2 + s, (sh - s)//2 + s))

def icns_rle(data):
    out = bytearray(); i = 0; n = len(data)
    while i < n:
        j = i
        while j < n and data[j] == data[i] and (j - i) < 130: j += 1
        if j - i >= 3:
            out.append(0x80 + (j - i) - 3); out.append(data[i]); i = j
        else:
            lit = bytearray(); j = i
            while j < n and len(lit) < 128:
                k = j
                while k < n and data[k] == data[j] and (k - j) < 3: k += 1
                if k - j >= 3: break
                lit.append(data[j]); j += 1
            out.append(len(lit) - 1); out.extend(lit); i = j
    return bytes(out)

def rgb_rle(img):
    r, g, b, a = img.split()
    return icns_rle(r.tobytes()) + icns_rle(g.tobytes()) + icns_rle(b.tobytes())

def alpha(img):
    return img.split()[3].tobytes()

def png(img):
    bio = io.BytesIO(); img.save(bio, "PNG"); return bio.getvalue()

def rz(px):
    return src.resize((px, px), Image.LANCZOS)

i16, i32, i48, i128 = rz(16), rz(32), rz(48), rz(128)
members = [
    (b'is32', rgb_rle(i16)),  (b's8mk', alpha(i16)),
    (b'il32', rgb_rle(i32)),  (b'l8mk', alpha(i32)),
    (b'ih32', rgb_rle(i48)),  (b'h8mk', alpha(i48)),
    (b'it32', b'\x00\x00\x00\x00' + rgb_rle(i128)), (b't8mk', alpha(i128)),
    (b'ic08', png(rz(256))),  (b'ic09', png(rz(512))),
]
body = b''.join(t + struct.pack('>I', len(d) + 8) + d for t, d in members)
data = b'icns' + struct.pack('>I', len(body) + 8) + body
open(OUT, "wb").write(data)
print("wrote %s: %d bytes; types: %s" % (OUT, len(data), ", ".join(t.decode() for t, _ in members)))
