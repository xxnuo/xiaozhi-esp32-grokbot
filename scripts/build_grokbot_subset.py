#!/usr/bin/env python3
"""Build a Xiaozhi-only GrokBot atlas from a StopWatch atlas or baked frame directory.

The full atlas contains 53 showcase states. Xiaozhi only asks for a bounded set of
assistant status animations and emoji names. The selected clips can optionally be
resampled to a smaller square size while retaining the original frame timing, or
encoded directly from 4x supersampled baked frames.
"""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from pathlib import Path

GLOBAL_HEADER = struct.Struct("<4sHHHBBI")
CLIP_HEADER = struct.Struct("<16sHHIII")
FRAME_HEADER = struct.Struct("<IB3x")
FRAME_INDEX = struct.Struct("<II")
START_FRAME = 2

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


def pack_gray_frames(gray_frames):
    import numpy as np

    arr = np.asarray(gray_frames, dtype=np.uint16)
    indexes = ((arr + 8) // 17).clip(0, 15).astype(np.uint8)
    packed = (indexes[..., 0::2] << 4) | indexes[..., 1::2]
    return packed.astype(np.uint8)


def downsample_supersampled_gray(gray_hi, factor: int = 4):
    """Downsample a supersampled 2D grayscale array using exact factor x factor area averaging."""
    import numpy as np

    arr = np.asarray(gray_hi, dtype=np.float32)
    height, width = arr.shape
    if height != width or factor <= 0 or height % factor != 0:
        raise ValueError(f"invalid supersampled geometry {width}x{height} for factor {factor}")
    target = height // factor
    area = factor * factor
    summed = arr.reshape(target, factor, target, factor).sum(axis=(1, 3))
    return np.clip(np.floor((summed + area * 0.5) / area), 0, 255).astype(np.uint8)


def encode_packed_frames(packed_frames) -> bytes:
    import numpy as np

    packed = np.asarray(packed_frames, dtype=np.uint8)
    records = []
    for frame_index in range(packed.shape[0]):
        if frame_index == 0:
            encoded = packed[0].tobytes()
            is_delta = False
        else:
            encoded = np.bitwise_xor(packed[frame_index], packed[frame_index - 1]).tobytes()
            is_delta = True
        records.append((is_delta, zlib.compress(encoded, 9)))

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


def load_baked_directory(frames_dir: Path, target_size: int = 0) -> tuple[int, int, int, list[dict]]:
    import numpy as np
    from PIL import Image

    summary_path = frames_dir / "summary.json"
    if not summary_path.is_file():
        raise SystemExit(f"missing {summary_path}")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    fps = int(summary.get("fps", 30))
    baked_size = int(summary.get("size", 360))
    output_size = target_size or baked_size
    if output_size <= 0 or output_size % 2 != 0:
        raise SystemExit("--size must be a positive even number")

    state_items = {item["state"]: item for item in summary["states"]}
    missing = [name for name in XIAOZHI_STATES if name not in state_items]
    if missing:
        raise SystemExit(f"baked directory is missing states: {', '.join(missing)}")

    selected = []
    packed_size = output_size * output_size // 2
    for name in XIAOZHI_STATES:
        item = state_items[name]
        count = int(item["count"])
        start_frame = int(item.get("start_frame", START_FRAME))
        if count <= start_frame:
            raise SystemExit(f"{name}: not enough frames ({count} <= {start_frame})")
        raw_path = frames_dir / f"{name}.raw"
        if raw_path.is_file():
            raw = np.frombuffer(raw_path.read_bytes(), dtype=np.uint8)
            side = int(round((raw.size / count) ** 0.5))
            frames = raw.reshape(count, side, side)
            if side != output_size:
                if side % output_size == 0:
                    factor = side // output_size
                    frames = np.stack(
                        [downsample_supersampled_gray(frame, factor) for frame in frames]
                    )
                else:
                    frames = np.stack(
                        [
                            np.asarray(
                                Image.fromarray(frame, mode="L").resize(
                                    (output_size, output_size), Image.Resampling.BOX
                                ),
                                dtype=np.uint8,
                            )
                            for frame in frames
                        ]
                    )
        else:
            png_files = sorted((frames_dir / name).glob("*.png"))
            if len(png_files) != count:
                raise SystemExit(f"{name}: expected {count} frames, found {len(png_files)}")
            frames_list = []
            for png_path in png_files:
                rgba = np.asarray(Image.open(png_path).convert("RGBA"), dtype=np.uint16)
                gray = ((rgba[:, :, :3].max(axis=2) * rgba[:, :, 3] + 127) // 255).astype(
                    np.uint8
                )
                if gray.shape[0] != output_size:
                    if gray.shape[0] % output_size == 0:
                        gray = downsample_supersampled_gray(gray, gray.shape[0] // output_size)
                    else:
                        gray = np.asarray(
                            Image.fromarray(gray, mode="L").resize(
                                (output_size, output_size), Image.Resampling.BOX
                            ),
                            dtype=np.uint8,
                        )
                frames_list.append(gray)
            frames = np.stack(frames_list)

        payload = encode_packed_frames(pack_gray_frames(frames))
        selected.append(
            {
                "name": name,
                "name_bytes": name.encode("ascii")[:15].ljust(16, b"\0"),
                "frames": count,
                "start_frame": start_frame,
                "size": len(payload),
                "packed_size": packed_size,
                "payload": payload,
            }
        )
    return output_size, output_size, fps, selected


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
    parser.add_argument("input", type=Path, help="full GrokBot GBZ1 atlas or baked frames dir")
    parser.add_argument("output", type=Path, help="output subset atlas")
    parser.add_argument("--size", type=int, default=0, help="resample to this square size")
    parser.add_argument("--max-size", type=parse_size, help="fail when output exceeds this size")
    args = parser.parse_args()

    if args.input.is_dir():
        output_width, output_height, fps, selected = load_baked_directory(args.input, args.size)
    else:
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
            if args.size > width:
                raise SystemExit("upscaling the atlas is not supported")
            for clip in selected:
                clip["payload"] = rebuild_clip(clip, width, height, output_width)
                clip["size"] = len(clip["payload"])
                clip["packed_size"] = output_width * output_height // 2

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
