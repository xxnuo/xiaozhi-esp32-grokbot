#!/usr/bin/env python3
"""Build a Xiaozhi-only GrokBot atlas from the full StopWatch atlas.

The full atlas contains 53 showcase states. Xiaozhi only asks for a bounded set of
assistant status animations and emoji names. The selected clips can optionally be
resampled to a smaller square size while retaining the original frame timing.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

GLOBAL_HEADER = struct.Struct("<4sHHHBBI")
CLIP_HEADER = struct.Struct("<16sHHIII")
FRAME_HEADER = struct.Struct("<IB3x")
FRAME_INDEX = struct.Struct("<II")

XIAOZHI_STATES = (
    "idle",
    "listening",
    "thinking",
    "streaming",
    "searching",
    "radar",
    "waiting",
    "loading",
    "uploading",
    "notifying",
    "alerting",
    "approving",
    "failed",
    "sleeping",
    "waking",
    "happy",
    "excited",
    "laughing",
    "proud",
    "curious",
    "playful",
    "shy",
    "surprised",
    "angry",
    "confused",
    "sad",
    "drowsy",
    "scared",
    "humming",
    "suspicious",
    "finished",
    "celebrate",
    "working",
    "turntable",
    "logo",
    "spawning",
    "writing",
    "receiving",
    "powering-down",
)


def parse_size(value: str) -> int:
    return int(value, 0)


def unpack_frame(packed: bytes, width: int, height: int):
    import numpy as np

    nibbles = np.frombuffer(packed, dtype=np.uint8)
    pixels = np.empty(nibbles.size * 2, dtype=np.uint8)
    pixels[0::2] = nibbles >> 4
    pixels[1::2] = nibbles & 0x0F
    return pixels.reshape(height, width)


def resize_packed(packed: bytes, width: int, height: int, size: int) -> bytes:
    import numpy as np
    from PIL import Image

    levels = np.arange(16, dtype=np.uint8) * 17
    gray = levels[unpack_frame(packed, width, height)]
    image = Image.fromarray(gray, mode="L").resize((size, size), Image.Resampling.BOX)
    gray = np.asarray(image, dtype=np.uint8)
    indexes = ((gray.astype(np.uint16) + 8) // 17).astype(np.uint8)
    return ((indexes[:, 0::2] << 4) | indexes[:, 1::2]).astype(np.uint8).tobytes()


def rebuild_clip(clip: dict, width: int, height: int, size: int) -> bytes:
    frames = clip["frames"]
    payload = clip["payload"]
    previous_packed = None
    previous_small = None
    records = []

    for frame_index in range(frames):
        record_offset, size_flags = FRAME_INDEX.unpack_from(payload, frame_index * FRAME_INDEX.size)
        compressed_size = size_flags & 0x7FFFFFFF
        is_delta = bool(size_flags >> 31)
        record = payload[record_offset : record_offset + FRAME_HEADER.size + compressed_size]
        stored_size, _ = FRAME_HEADER.unpack_from(record)
        if stored_size != compressed_size or len(record) != FRAME_HEADER.size + compressed_size:
            raise ValueError(f"{clip['name']}: invalid frame record")
        delta = zlib.decompress(record[FRAME_HEADER.size :])
        expected_size = width * height // 2
        if len(delta) != expected_size:
            raise ValueError(f"{clip['name']}: invalid packed frame size")
        packed = bytes(a ^ b for a, b in zip(previous_packed, delta)) if is_delta else delta
        previous_packed = packed
        small = resize_packed(packed, width, height, size)
        if frame_index == 0:
            encoded = small
            encoded_delta = False
        else:
            encoded = bytes(a ^ b for a, b in zip(small, previous_small))
            encoded_delta = True
        previous_small = small
        records.append((encoded_delta, zlib.compress(encoded, 9)))

    index_size = len(records) * FRAME_INDEX.size
    frame_offset = index_size
    index = bytearray()
    for is_delta, compressed in records:
        index.extend(FRAME_INDEX.pack(frame_offset, len(compressed) | (int(is_delta) << 31)))
        frame_offset += FRAME_HEADER.size + len(compressed)

    output = bytearray(index)
    for is_delta, compressed in records:
        output.extend(FRAME_HEADER.pack(len(compressed), int(is_delta)))
        output.extend(compressed)
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="full GrokBot GBZ1 atlas")
    parser.add_argument("output", type=Path, help="output subset atlas")
    parser.add_argument("--size", type=int, default=0, help="resample to this square size")
    parser.add_argument("--max-size", type=parse_size, help="fail when output exceeds this size")
    args = parser.parse_args()

    source = args.input.read_bytes()
    if len(source) < GLOBAL_HEADER.size or source[:4] != b"GBZ1":
        raise SystemExit("input is not a GBZ1 atlas")
    magic, version, width, height, fps, clip_count, _ = GLOBAL_HEADER.unpack_from(source)
    if version != 1 or width != height or width <= 0:
        raise SystemExit("unsupported atlas geometry or version")

    clips = {}
    for index in range(clip_count):
        header_offset = GLOBAL_HEADER.size + index * CLIP_HEADER.size
        header = source[header_offset : header_offset + CLIP_HEADER.size]
        name_bytes, frames, start_frame, offset, size, packed_size = CLIP_HEADER.unpack(header)
        name = name_bytes.split(b"\0", 1)[0].decode("ascii")
        if offset + size > len(source):
            raise SystemExit(f"{name}: clip exceeds input file")
        clips[name] = {
            "name": name,
            "name_bytes": name_bytes,
            "frames": frames,
            "start_frame": start_frame,
            "size": size,
            "packed_size": packed_size,
            "payload": source[offset : offset + size],
        }

    missing = [name for name in XIAOZHI_STATES if name not in clips]
    if missing:
        raise SystemExit(f"input atlas is missing states: {', '.join(missing)}")

    output_width = args.size or width
    output_height = args.size or height
    if output_width <= 0 or output_width % 2 != 0:
        raise SystemExit("--size must be a positive even number")

    selected = [clips[name].copy() for name in XIAOZHI_STATES]
    if output_width != width:
        for clip in selected:
            clip["payload"] = rebuild_clip(clip, width, height, output_width)
            clip["size"] = len(clip["payload"])
            clip["packed_size"] = output_width * output_height // 2
        if args.size > width:
            raise SystemExit("upscaling the atlas is not supported")

    output = bytearray(
        GLOBAL_HEADER.pack(b"GBZ1", 1, output_width, output_height, fps, len(selected), 0)
    )
    offset = GLOBAL_HEADER.size + len(selected) * CLIP_HEADER.size
    headers = []
    for clip in selected:
        headers.append(
            CLIP_HEADER.pack(
                clip["name_bytes"],
                clip["frames"],
                clip["start_frame"],
                offset,
                clip["size"],
                clip["packed_size"],
            )
        )
        offset += clip["size"]
    output.extend(b"".join(headers))
    for clip in selected:
        output.extend(clip["payload"])

    if args.max_size is not None and len(output) > args.max_size:
        raise SystemExit(f"output is {len(output)} bytes, exceeds limit {args.max_size}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    print(
        f"wrote {args.output} ({len(output)} bytes, {len(output) / 1048576:.3f} MiB, "
        f"{len(selected)} states, {output_width}x{output_height}@{fps})"
    )


if __name__ == "__main__":
    main()
