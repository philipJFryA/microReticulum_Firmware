#!/usr/bin/env python3
"""Download XYZ (slippy-map) tiles from the MapTiler API and save them as
24-bit BMP files in the exact layout the microReticulum T-Deck SAR app reads:

    /tiles/{zoom}/{x}/{y}.bmp

The firmware decodes 24-bit BMP tiles directly (bottom-up BGR rows padded to 4
bytes), so each source tile is converted from the MapTiler PNG response to an
uncompressed 256x256 24-bit BMP.

Requires Pillow for the PNG -> BMP conversion:

    pip install Pillow

Usage:
  python3 fetch_sar_tiles.py --api-key your_key_here \
      --lat 63.42 --lon 10.39 --min-zoom 12 --max-zoom 16 [--out ./tiles]

  python3 fetch_sar_tiles.py --api-key your_key_here \
      --bbox 63.35,10.25,63.45,10.45 --min-zoom 12 --max-zoom 16

After it finishes, copy the "tiles" directory to the root of the T-Deck's
microSD card.
"""

import argparse
import io
import math
import os
import struct
import sys
import time
import urllib.error
import urllib.request

try:
    from PIL import Image
except ImportError:
    print("Pillow is required for PNG->BMP conversion. Install with: "
          "pip install Pillow")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Web Mercator (EPSG:3857) tile math — same formulas the firmware uses.
# ---------------------------------------------------------------------------

def deg2num(lat_deg: float, lon_deg: float, zoom: int) -> tuple[int, int]:
    """Return (x, y) tile indices for a lat/lon at the given zoom."""
    lat_rad = math.radians(lat_deg)
    n = 2.0 ** zoom
    x = int((lon_deg + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return x, y


def tile_bounds(zoom: int) -> int:
    """Number of tiles along each axis at the given zoom."""
    return 1 << zoom


def bbox_tile_range(min_lat: float, min_lon: float,
                    max_lat: float, max_lon: float, zoom: int,
                    ) -> tuple[int, int, int, int]:
    """Return (min_x, max_x, min_y, max_y) tile indices covering a bbox."""
    n = tile_bounds(zoom)
    x0, y1 = deg2num(max_lat, min_lon, zoom)   # top-left
    x1, y0 = deg2num(min_lat, max_lon, zoom)   # bottom-right
    x0 = max(0, min(x0, n - 1))
    x1 = max(0, min(x1, n - 1))
    y0 = max(0, min(y0, n - 1))
    y1 = max(0, min(y1, n - 1))
    if x1 < x0:
        x0, x1 = x1, x0
    if y1 < y0:
        y0, y1 = y1, y0
    return x0, x1, y0, y1


# ---------------------------------------------------------------------------
# BMP writing (24-bit RGB, bottom-up, row padded to 4 bytes — matches the
# firmware's sar_decode_bmp exactly).
# ---------------------------------------------------------------------------

def encode_24bit_bmp(png_bytes: bytes) -> bytes:
    """Convert PNG bytes to an uncompressed 256x256 24-bit BMP byte string.

    MapTiler renders tiles at 512x512 by default; the firmware's fixed-size
    decoder requires 256x256, so the image is downscaled with a high-quality
    resample before encoding.
    """
    img = Image.open(io.BytesIO(png_bytes))
    if img.mode != "RGB":
        img = img.convert("RGB")
    if img.size != (256, 256):
        img = img.resize((256, 256), Image.LANCZOS)

    w = h = 256
    row_size = w * 3
    pad = (4 - row_size % 4) % 4
    stride = row_size + pad
    pixel_array_size = stride * h

    bmp = bytearray()
    # BITMAPFILEHEADER (14 bytes)
    bmp += b"BM"
    bmp += struct.pack("<IHHI", 14 + 40 + pixel_array_size, 0, 0, 14 + 40)
    # BITMAPINFOHEADER (40 bytes)
    bmp += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0,
                       pixel_array_size, 2835, 2835, 0, 0)

    # Pixel rows, bottom-up (BGR order), each row padded to a 4-byte boundary.
    pixels = img.load()
    for yy in range(h - 1, -1, -1):   # bottom row first
        row = bytearray()
        for xx in range(w):
            r, g, b = pixels[xx, yy]
            row += bytes((b, g, r))
        row += b"\x00" * pad
        bmp += row
    return bytes(bmp)


# ---------------------------------------------------------------------------
# Download helpers.
# ---------------------------------------------------------------------------

DEFAULT_STYLE = "streets-v2"
DEFAULT_UA = "microReticulum-SAR-tile-fetcher/1.0 (contact: you@example.com)"


def maptiler_url(style: str, z: int, x: int, y: int, api_key: str) -> str:
    return "https://api.maptiler.com/maps/{}/{}/{}/{}.png?key={}".format(
        style, z, x, y, api_key)


def download_tile(url: str, ua: str, retries: int = 3) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": ua})
    last_err = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.read()
        except urllib.error.HTTPError as e:
            # Don't retry auth failures or missing tiles.
            if e.code in (401, 403):
                raise
            last_err = e
            time.sleep(1.0 * (attempt + 1))
        except (urllib.error.URLError, OSError) as e:
            last_err = e
            time.sleep(1.0 * (attempt + 1))
    raise last_err


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download XYZ tiles from MapTiler and save them as 24-bit "
                    "BMPs for the microReticulum T-Deck SAR app.")
    loc = parser.add_argument_group("location (one required)")
    loc.add_argument("--lat", type=float, help="centre latitude (with --lon)")
    loc.add_argument("--lon", type=float, help="centre longitude (with --lat)")
    loc.add_argument("--bbox", type=str,
                     help="min_lat,min_lon,max_lat,max_lon")
    parser.add_argument("--radius-km", type=float, default=2.0,
                        help="radius around centre (km) when using --lat/--lon")
    parser.add_argument("--min-zoom", type=int, default=12)
    parser.add_argument("--max-zoom", type=int, default=16)
    parser.add_argument("--out", default="./tiles",
                        help="output directory (copy to SD card root)")
    parser.add_argument("--style", default=DEFAULT_STYLE,
                        help="MapTiler style id (default: %(default)s)")
    parser.add_argument("--api-key", required=True,
                        help="MapTiler API key (required)")
    parser.add_argument("--user-agent", default=DEFAULT_UA)
    parser.add_argument("--overwrite", action="store_true",
                        help="re-fetch tiles that already exist")
    parser.add_argument("--delay", type=float, default=0.1,
                        help="delay between downloads (seconds)")
    args = parser.parse_args()

    if not args.api_key:
        parser.error("MapTiler API key required: pass --api-key")

    if args.lat is not None or args.lon is not None:
        if args.lat is None or args.lon is None:
            parser.error("--lat and --lon must be used together")
        # Radius (km) to degree box (approx).
        d_lat = args.radius_km / 111.0
        d_lon = args.radius_km / (111.0 * max(0.1,
                                              math.cos(math.radians(args.lat))))
        min_lat = args.lat - d_lat
        max_lat = args.lat + d_lat
        min_lon = args.lon - d_lon
        max_lon = args.lon + d_lon
    else:
        parts = [float(v.strip()) for v in args.bbox.split(",")]
        if len(parts) != 4:
            parser.error("--bbox must be min_lat,min_lon,max_lat,max_lon")
        min_lat, min_lon, max_lat, max_lon = parts

    if args.min_zoom < 0 or args.max_zoom > 22 or args.min_zoom > args.max_zoom:
        parser.error("invalid zoom range (use 0-22, min <= max)")

    total_fetched = 0
    total_skipped = 0
    total_failed = 0

    for zoom in range(args.min_zoom, args.max_zoom + 1):
        x0, x1, y0, y1 = bbox_tile_range(min_lat, min_lon, max_lat, max_lon,
                                         zoom)
        for tx in range(x0, x1 + 1):
            for ty in range(y0, y1 + 1):
                out_dir = os.path.join(args.out, str(zoom), str(tx))
                os.makedirs(out_dir, exist_ok=True)
                out_path = os.path.join(out_dir, "{}.bmp".format(ty))

                if os.path.exists(out_path) and not args.overwrite:
                    total_skipped += 1
                    continue

                url = maptiler_url(args.style, zoom, tx, ty, args.api_key)
                try:
                    png_data = download_tile(url, args.user_agent)
                    bmp_data = encode_24bit_bmp(png_data)
                except urllib.error.HTTPError as e:
                    print("  WARN: HTTP {} failed {}: {}".format(
                        e.code, url, e.reason))
                    total_failed += 1
                    continue
                except Exception as e:
                    print("  WARN: failed {}: {}".format(url, e))
                    total_failed += 1
                    continue

                with open(out_path, "wb") as f:
                    f.write(bmp_data)
                total_fetched += 1
                print("  -> {} ({} bytes BMP)".format(out_path, len(bmp_data)))

                if args.delay > 0:
                    time.sleep(args.delay)

    print("\nDone: {} tiles written, {} already present (skipped), "
          "{} failed.".format(total_fetched, total_skipped, total_failed))
    print("Copy the '{}' directory to the root of the SD card.".format(
        os.path.abspath(args.out)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(1)