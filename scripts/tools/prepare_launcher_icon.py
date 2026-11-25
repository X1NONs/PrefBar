#!/usr/bin/env python3
"""Create the square PS5 launcher icon from PrefBar's existing wordmark.

The source logo is a non-interlaced, 8-bit RGB PNG. This converter uses only
Python's standard library, preserves its aspect ratio, and centers it on a
512x512 canvas using the logo's corner background color.
"""

from __future__ import annotations

import math
from pathlib import Path
import struct
import sys
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
ICON_SIZE = 512


def paeth_predictor(left: int, above: int, upper_left: int) -> int:
    prediction = left + above - upper_left
    distance_left = abs(prediction - left)
    distance_above = abs(prediction - above)
    distance_upper_left = abs(prediction - upper_left)
    if distance_left <= distance_above and distance_left <= distance_upper_left:
        return left
    if distance_above <= distance_upper_left:
        return above
    return upper_left


def decode_rgb_png(path: Path) -> tuple[int, int, list[bytearray]]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError(f"{path} is not a PNG file")

    offset = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()

    while offset + 12 <= len(data):
        chunk_size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        chunk_type = data[offset:offset + 4]
        offset += 4
        chunk_end = offset + chunk_size
        if chunk_end + 4 > len(data):
            raise ValueError("PNG contains a truncated chunk")

        chunk_data = data[offset:chunk_end]
        expected_crc = struct.unpack_from(">I", data, chunk_end)[0]
        actual_crc = zlib.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError(f"PNG chunk {chunk_type!r} has an invalid CRC")

        if chunk_type == b"IHDR":
            if chunk_size != 13:
                raise ValueError("PNG has an invalid IHDR chunk")
            width, height, bit_depth, color_type, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", chunk_data)
            )
            if compression != 0 or filtering != 0:
                raise ValueError("Unsupported PNG compression or filter method")
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

        offset = chunk_end + 4

    if width is None or height is None:
        raise ValueError("PNG is missing its IHDR chunk")
    if width <= 0 or height <= 0:
        raise ValueError("PNG dimensions must be positive")
    if bit_depth != 8 or color_type != 2 or interlace != 0:
        raise ValueError(
            "PrefBar's source logo must remain an 8-bit, non-interlaced RGB PNG"
        )

    bytes_per_pixel = 3
    row_size = width * bytes_per_pixel
    decoded = zlib.decompress(bytes(compressed))
    expected_size = height * (row_size + 1)
    if len(decoded) != expected_size:
        raise ValueError("PNG pixel data length does not match its dimensions")

    rows: list[bytearray] = []
    previous = bytearray(row_size)
    position = 0

    for _ in range(height):
        filter_type = decoded[position]
        position += 1
        row = bytearray(decoded[position:position + row_size])
        position += row_size

        if filter_type == 1:  # Sub
            for index in range(row_size):
                left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                row[index] = (row[index] + left) & 0xFF
        elif filter_type == 2:  # Up
            for index in range(row_size):
                row[index] = (row[index] + previous[index]) & 0xFF
        elif filter_type == 3:  # Average
            for index in range(row_size):
                left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                above = previous[index]
                row[index] = (row[index] + ((left + above) // 2)) & 0xFF
        elif filter_type == 4:  # Paeth
            for index in range(row_size):
                left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                above = previous[index]
                upper_left = (
                    previous[index - bytes_per_pixel]
                    if index >= bytes_per_pixel
                    else 0
                )
                row[index] = (
                    row[index] + paeth_predictor(left, above, upper_left)
                ) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"Unsupported PNG row filter: {filter_type}")

        rows.append(row)
        previous = row

    return width, height, rows


def sample_pixel(rows: list[bytearray], width: int, x: int, y: int) -> tuple[int, int, int]:
    offset = x * 3
    row = rows[y]
    return row[offset], row[offset + 1], row[offset + 2]


def resize_bilinear(
    rows: list[bytearray],
    source_width: int,
    source_height: int,
    target_width: int,
    target_height: int,
) -> list[bytearray]:
    resized = [bytearray(target_width * 3) for _ in range(target_height)]

    for target_y in range(target_height):
        source_y = (target_y + 0.5) * source_height / target_height - 0.5
        y0 = max(0, min(source_height - 1, math.floor(source_y)))
        y1 = min(source_height - 1, y0 + 1)
        y_weight = max(0.0, min(1.0, source_y - y0))

        for target_x in range(target_width):
            source_x = (target_x + 0.5) * source_width / target_width - 0.5
            x0 = max(0, min(source_width - 1, math.floor(source_x)))
            x1 = min(source_width - 1, x0 + 1)
            x_weight = max(0.0, min(1.0, source_x - x0))

            p00 = sample_pixel(rows, source_width, x0, y0)
            p10 = sample_pixel(rows, source_width, x1, y0)
            p01 = sample_pixel(rows, source_width, x0, y1)
            p11 = sample_pixel(rows, source_width, x1, y1)

            output_offset = target_x * 3
            for channel in range(3):
                top = p00[channel] * (1.0 - x_weight) + p10[channel] * x_weight
                bottom = p01[channel] * (1.0 - x_weight) + p11[channel] * x_weight
                value = top * (1.0 - y_weight) + bottom * y_weight
                resized[target_y][output_offset + channel] = max(
                    0, min(255, int(round(value)))
                )

    return resized


def make_icon(rows: list[bytearray], width: int, height: int) -> bytes:
    scale = min(ICON_SIZE / width, ICON_SIZE / height)
    target_width = max(1, min(ICON_SIZE, int(round(width * scale))))
    target_height = max(1, min(ICON_SIZE, int(round(height * scale))))
    resized = resize_bilinear(rows, width, height, target_width, target_height)

    corners = (
        sample_pixel(rows, width, 0, 0),
        sample_pixel(rows, width, width - 1, 0),
        sample_pixel(rows, width, 0, height - 1),
        sample_pixel(rows, width, width - 1, height - 1),
    )
    background = tuple(
        sum(pixel[channel] for pixel in corners) // len(corners)
        for channel in range(3)
    )

    canvas = bytearray(background * (ICON_SIZE * ICON_SIZE))
    left = (ICON_SIZE - target_width) // 2
    top = (ICON_SIZE - target_height) // 2
    for y, row in enumerate(resized):
        start = ((top + y) * ICON_SIZE + left) * 3
        canvas[start:start + len(row)] = row

    raw = bytearray()
    row_size = ICON_SIZE * 3
    for y in range(ICON_SIZE):
        raw.append(0)  # PNG filter: None
        start = y * row_size
        raw.extend(canvas[start:start + row_size])

    def chunk(name: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + name
            + payload
            + struct.pack(">I", zlib.crc32(name + payload) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", ICON_SIZE, ICON_SIZE, 8, 2, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), level=9))
        + chunk(b"IEND", b"")
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(
            f"usage: {sys.argv[0]} INPUT_LOGO.png OUTPUT_ICON.png",
            file=sys.stderr,
        )
        return 2

    source = Path(sys.argv[1])
    destination = Path(sys.argv[2])

    try:
        width, height, rows = decode_rgb_png(source)
        icon = make_icon(rows, width, height)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(icon)
    except (OSError, ValueError, zlib.error, struct.error) as error:
        print(f"launcher icon generation failed: {error}", file=sys.stderr)
        return 1

    print(f"Generated {ICON_SIZE}x{ICON_SIZE} PrefBar launcher icon: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
