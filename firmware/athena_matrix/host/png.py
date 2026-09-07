#!/usr/bin/env python3
"""png.py - compose binary P6 PPM frames into a contact-sheet PNG.

Used by sheet.py; also a CLI on its own.

Usage:
    python3 png.py --scale 4 --cols 8 --out sheet.png a.ppm b.ppm ...

Tiles are laid out left-to-right, top-to-bottom, nearest-neighbour upscaled
by --scale, with a 2-pixel black gap between tiles. Stdlib only: PPM parsing
is hand-rolled, and the PNG is written chunk-by-chunk with zlib for the
IDAT deflate stream and crc.
"""
import argparse
import struct
import sys
import zlib

GAP = 2  # pixels, in the final (already-scaled) image


def _skip_ws_and_comments(data, pos):
    while True:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b'#':
            while pos < len(data) and data[pos:pos + 1] != b'\n':
                pos += 1
            continue
        break
    return pos


def _read_token(data, pos):
    pos = _skip_ws_and_comments(data, pos)
    start = pos
    while pos < len(data) and not data[pos:pos + 1].isspace():
        pos += 1
    return data[start:pos], pos


def read_ppm(path):
    """Read a binary P6 PPM, tolerating whitespace and '#' comments in the
    header the way the format allows."""
    with open(path, 'rb') as f:
        data = f.read()

    pos = 0
    magic, pos = _read_token(data, pos)
    if magic != b'P6':
        raise ValueError(f"{path}: not a P6 PPM (magic={magic!r})")
    width_tok, pos = _read_token(data, pos)
    height_tok, pos = _read_token(data, pos)
    maxval_tok, pos = _read_token(data, pos)
    width = int(width_tok)
    height = int(height_tok)
    maxval = int(maxval_tok)
    if maxval != 255:
        raise ValueError(f"{path}: unsupported maxval {maxval} (only 255 is supported)")
    # Exactly one whitespace byte separates the header from the binary data.
    pos += 1
    pixels = data[pos:pos + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: truncated pixel data")
    return width, height, pixels


def compose(frames, scale, cols):
    w, h, _ = frames[0]
    for fw, fh, _ in frames:
        if (fw, fh) != (w, h):
            raise ValueError("all input PPMs must have matching dimensions")

    n = len(frames)
    rows = (n + cols - 1) // cols
    tile_w = w * scale
    tile_h = h * scale
    sheet_w = cols * tile_w + (cols - 1) * GAP
    sheet_h = rows * tile_h + (rows - 1) * GAP

    buf = bytearray(sheet_w * sheet_h * 3)  # zero-filled: black background/gaps

    for idx, (fw, fh, pixels) in enumerate(frames):
        col = idx % cols
        row = idx // cols
        ox = col * (tile_w + GAP)
        oy = row * (tile_h + GAP)
        for sy in range(fh):
            src_row_off = sy * fw * 3
            for sx in range(fw):
                po = src_row_off + sx * 3
                r, g, b = pixels[po], pixels[po + 1], pixels[po + 2]
                dst_x0 = ox + sx * scale
                for dy in range(scale):
                    yy = oy + sy * scale + dy
                    row_off = (yy * sheet_w + dst_x0) * 3
                    for dx in range(scale):
                        o = row_off + dx * 3
                        buf[o] = r
                        buf[o + 1] = g
                        buf[o + 2] = b

    return sheet_w, sheet_h, bytes(buf)


def write_png(path, width, height, rgb_bytes):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)  # 8-bit RGB

    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type: none
        raw.extend(rgb_bytes[y * stride:(y + 1) * stride])
    compressed = zlib.compress(bytes(raw), 9)

    with open(path, 'wb') as f:
        f.write(sig)
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', compressed))
        f.write(chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description="Compose P6 PPM frames into a contact-sheet PNG.")
    parser.add_argument('--scale', type=int, default=1, help="nearest-neighbour upscale factor")
    parser.add_argument('--cols', type=int, default=8, help="tiles per row")
    parser.add_argument('--out', required=True, help="output PNG path")
    parser.add_argument('ppms', nargs='+', help="input .ppm files, in sheet order")
    args = parser.parse_args()

    if args.scale < 1:
        parser.error("--scale must be >= 1")
    if args.cols < 1:
        parser.error("--cols must be >= 1")

    frames = [read_ppm(p) for p in args.ppms]
    width, height, rgb = compose(frames, args.scale, args.cols)
    write_png(args.out, width, height, rgb)
    print(f"png.py: wrote {args.out} ({width}x{height}, {len(frames)} tiles)", file=sys.stderr)


if __name__ == '__main__':
    main()
