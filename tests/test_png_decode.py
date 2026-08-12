#!/usr/bin/env python3
"""Validate the exact C++ PNG-decoding algorithm in TDeckUI.h by mirroring
it in Python and decoding real Pillow-generated 256x256 PNGs (which use
actual zlib + PNG scanline filters) pixel-for-pixel."""

import struct
import zlib
from io import BytesIO

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def make_png_pillow(color_type, pixel_fn):
    """Create a 256x256 PNG via Pillow with the given mode.
    color_type: "RGB", "RGBA", "L", "P"
    pixel_fn(x, y) -> tuple/list of channel values (0-255).
    """
    w = h = 256
    if color_type == "RGB":
        img = Image.new("RGB", (w, h))
        px = img.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = tuple(pixel_fn(x, y))
    elif color_type == "RGBA":
        img = Image.new("RGBA", (w, h))
        px = img.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = tuple(pixel_fn(x, y))
    elif color_type == "L":
        img = Image.new("L", (w, h))
        px = img.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = pixel_fn(x, y)[0]
    elif color_type == "P":
        # 8-bit palette image: first 256 entries = grayscale ramp
        pal = bytes([(i, i, i) for i in range(256)])
        img = Image.new("P", (w, h))
        img.putpalette(pal)
        px = img.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = pixel_fn(x, y)[0]
    else:
        raise ValueError(f"unsupported Pillow mode {color_type}")

    buf = BytesIO()
    # PNG filter 5 lets Pillow/zlib pick adaptive per-row filters, exercising
    # Sub/Up/Average/Paeth across rows - exactly what real OSM tiles use.
    img.save(buf, format="PNG", compress_level=6,
             optimize=False, filter=5 if hasattr(Image, "PNG") else 0)
    return buf.getvalue()


def mirror_cpp_decoder(png_bytes, expect_fn, channels):
    """Mirror tdeck_sar_decode_png (TDeckUI.h) and compare to expect_fn(x,y)."""

    # ---- signature + IHDR ----
    assert png_bytes[:8] == b"\x89PNG\r\n\x1a\n", "bad PNG magic"
    ihdr_len = struct.unpack(">I", png_bytes[8:12])[0]
    assert png_bytes[12:16] == b"IHDR" and ihdr_len >= 13, "bad IHDR"
    png_w, png_h = struct.unpack(">II", png_bytes[16:24])
    bit_depth = png_bytes[24]
    color_type = png_bytes[25]
    compression = png_bytes[26]
    filter_method = png_bytes[27]
    interlace = png_bytes[28]

    assert png_w == 256 and png_h == 256, f"{png_w}x{png_h}"
    assert bit_depth == 8, f"bit_depth={bit_depth}"
    assert color_type in (0, 2, 4, 6), f"color_type={color_type}"
    assert compression == 0 and filter_method == 0 and interlace == 0

    # ---- chunk parsing / IDAT concatenation (mirrors C++) ----
    idat = bytearray()
    pos = 8
    while pos + 8 <= len(png_bytes):
        clen = struct.unpack(">I", png_bytes[pos:pos+4])[0]
        ctype = png_bytes[pos+4:pos+8]
        assert pos + 12 + clen <= len(png_bytes), "truncated chunk"
        data = png_bytes[pos+8:pos+8+clen]
        if ctype == b"IDAT":
            idat.extend(data)
        pos += 12 + clen

    assert len(idat) >= 6, "tiny IDAT"
    # zlib header validation (mirrors C++)
    assert (idat[0] & 0x0F) == 8, "bad CMF"
    assert (idat[0] * 256 + idat[1]) % 31 == 0, "bad FLG"

    # Inflate the FULL IDAT (zlib header + deflate + adler) the way tinfl
    # would with TINFL_FLAG_PARSE_ZLIB_HEADER set.
    decomp = zlib.decompressobj()
    raw = decomp.decompress(bytes(idat))
    raw += decomp.flush()

    # ---- expected output size (mirrors C++ out_bytes) ----
    row_bpp = 256 * channels
    out_bytes = (row_bpp + 1) * 256
    assert len(raw) == out_bytes, f"inflate got {len(raw)}, want {out_bytes}"

    bpp = channels
    prev = bytearray(row_bpp)
    rows_rgb = []
    for y in range(256):
        line_off = y * (row_bpp + 1)
        filt = raw[line_off]
        cur = bytearray(raw[line_off+1: line_off+1+row_bpp])

        # Unfilter (mirrors C++ exactly)
        if filt == 1:  # Sub
            for i in range(bpp, row_bpp):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif filt == 2:  # Up
            for i in range(row_bpp):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif filt == 3:  # Average
            for i in range(row_bpp):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filt == 4:  # Paeth
            for i in range(row_bpp):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pred) & 0xFF

        row = bytearray()
        for x in range(256):
            if color_type == 0:
                r = g = b = cur[x]
            elif color_type == 2:
                r, g, b = cur[x*3], cur[x*3+1], cur[x*3+2]
            elif color_type == 4:
                r = g = b = cur[x*2]
            elif color_type == 6:
                r, g, b = cur[x*4], cur[x*4+1], cur[x*4+2]
            row.extend((r, g, b))
        rows_rgb.append(bytes(row))
        prev = bytes(cur)

    # ---- compare ----
    mismatches = 0
    for y in range(256):
        for x in range(256):
            exp = bytes(expect_fn(x, y))
            got = rows_rgb[y][x*3: x*3+3]
            if got != exp:
                mismatches += 1
                if mismatches <= 8:
                    print(f"  MISMATCH x={x} y={y} got={tuple(got)} exp={tuple(exp)}")
    return mismatches


def rgb_gradient(x, y):
    return (x & 0xFF, y & 0xFF, (x + y) & 0xFF)


def rgba_gradient(x, y):
    return (x & 0xFF, y & 0xFF, (x + y) & 0xFF, 0xFF)


def gray_gradient(x, y):
    return ((x + y) & 0xFF,)


def main():
    if not HAVE_PIL:
        print("Pillow not available - skipping real-PNG validation")
        return 1

    print("=== Real RGB PNG (adaptive filters) ===")
    png = make_png_pillow("RGB", rgb_gradient)
    m1 = mirror_cpp_decoder(png, lambda x, y: rgb_gradient(x, y), 3)
    print(f"  RGB mismatches: {m1}")
    assert m1 == 0

    print("=== Real RGBA PNG (adaptive filters) ===")
    png2 = make_png_pillow("RGBA", rgba_gradient)
    m2 = mirror_cpp_decoder(png2, lambda x, y: rgba_gradient(x, y)[:3], 4)
    print(f"  RGBA mismatches: {m2}")
    assert m2 == 0

    print("=== Real Grayscale PNG (adaptive filters) ===")
    png3 = make_png_pillow("L", gray_gradient)
    m3 = mirror_cpp_decoder(png3, lambda x, y: ((x+y) & 0xFF,) * 3, 1)
    print(f"  GRAY mismatches: {m3}")
    assert m3 == 0

    print("\nAll decoder-mirror tests passed: PNG decode algorithm is correct.")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())