#!/usr/bin/env python3
"""Download XYZ (slippy-map) tiles and save them directly as PNG files in the
exact layout the microReticulum T-Deck SAR app reads:

    /tiles/{zoom}/{x}/{y}.png

The firmware decodes PNG tiles directly on the ESP32-S3 (using the ROM's
tinfl/miniz inflate implementation - no PNG library dependency), so the
original server PNG is written verbatim with no transcoding.

Usage:
  python3 fetch_sar_tiles.py --lat 63.42 --lon 10.39 \
      --min-zoom 12 --max-zoom 16 [--out ./tiles]

  python3 fetch_sar_tiles.py --bbox 63.35,10.25,63.45,10.45 \
      --min-zoom 12 --max-zoom 16

After it finishes, copy the "tiles" directory to the root of the T-Deck's
microSD card.
"""

import argparse
import math
import os
import struct
import sys
import time
import urllib.error
import urllib.request

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
# PNG sanity check.
# ---------------------------------------------------------------------------

def png_is_256x256(data: bytes) -> bool:
    """Return True if the data is a PNG whose IHDR declares a 256x256 image."""
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    w, h = struct.unpack(">II", data[16:24])
    return w == 256 and h == 256


# ---------------------------------------------------------------------------
# Download helpers.
# ---------------------------------------------------------------------------

DEFAULT_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
DEFAULT_UA = "microReticulum-SAR-tile-fetcher/1.0 (contact: you@example.com)"


def download_tile(url: str, ua: str, retries: int = 3) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": ua})
    last_err = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.read()
        except (urllib.error.URLError, OSError) as e:
            last_err = e
            time.sleep(1.0 * (attempt + 1))
    raise last_err


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download XYZ tiles and save them as PNG "
                    "for the microReticulum T-Deck SAR app.")
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
    parser.add_argument("--url", default=DEFAULT_TILE_URL,
                        help="tile URL template with {z}/{x}/{y} placeholders")
    parser.add_argument("--user-agent", default=DEFAULT_UA)
    parser.add_argument("--overwrite", action="store_true",
                        help="re-fetch tiles that already exist")
    parser.add_argument("--delay", type=float, default=0.1,
                        help="delay between downloads (seconds)")
    args = parser.parse_args()

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

    for zoom in range(args.min_zoom, args.max_zoom + 1):
        x0, x1, y0, y1 = bbox_tile_range(min_lat, min_lon, max_lat, max_lon,
                                         zoom)
        for tx in range(x0, x1 + 1):
            for ty in range(y0, y1 + 1):
                out_dir = os.path.join(args.out, str(zoom), str(tx))
                os.makedirs(out_dir, exist_ok=True)
                out_path = os.path.join(out_dir, "{}.png".format(ty))

                if os.path.exists(out_path) and not args.overwrite:
                    total_skipped += 1
                    continue

                url = args.url.format(z=zoom, x=tx, y=ty)
                try:
                    data = download_tile(url, args.user_agent)
                except Exception as e:
                    print("  WARN: failed {}: {}".format(url, e))
                    continue

                if not png_is_256x256(data):
                    print("  WARN: {} is not a 256x256 PNG; will still "
                          "write (firmware assumes 256x256)".format(url))

                with open(out_path, "wb") as f:
                    f.write(data)
                total_fetched += 1
                print("  -> {}/{}".format(out_path, url))

                if args.delay > 0:
                    time.sleep(args.delay)

    print("\nDone: {} tiles written, {} already present (skipped).".format(
        total_fetched, total_skipped))
    print("Copy the '{}' directory to the root of the SD card.".format(
        os.path.abspath(args.out)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(1)