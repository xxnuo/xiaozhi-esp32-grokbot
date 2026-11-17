#!/usr/bin/env python3
"""Host-side GrokBot director preview generator.

Compiles scripts/grokbot_preview.cc against main/display/grokbot_director.cc,
decodes main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas, and renders
MP4 preview videos, transition timelines, and contact sheets under .tmp/preview/.
"""

from __future__ import annotations

import argparse
import math
import json
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

GLOBAL_HEADER = struct.Struct("<4sHHHBBI")
CLIP_HEADER = struct.Struct("<16sHHIII")
FRAME_HEADER = struct.Struct("<IB3x")
FRAME_INDEX = struct.Struct("<II")

SCENARIOS = ("boot_idle", "conversation", "wake_from_sleep")


class AtlasDecoder:
    def __init__(self, path: Path) -> None:
        raw = path.read_bytes()
        if len(raw) < GLOBAL_HEADER.size or raw[:4] != b"GBZ1":
            raise ValueError(f"{path} is not a GBZ1 atlas")
        _, version, width, height, fps, clip_count, _ = GLOBAL_HEADER.unpack_from(raw, 0)
        if version != 1:
            raise ValueError(f"Unsupported atlas version {version}")
        self.width = width
        self.height = height
        self.fps = fps
        self.clips: dict[str, dict] = {}
        for i in range(clip_count):
            off = GLOBAL_HEADER.size + i * CLIP_HEADER.size
            name_bytes, frames, start_frame, clip_off, clip_sz, packed_sz = CLIP_HEADER.unpack_from(
                raw, off
            )
            name = name_bytes.split(b"\0", 1)[0].decode("ascii")
            self.clips[name] = {
                "name": name,
                "frames": frames,
                "start_frame": start_frame,
                "payload": raw[clip_off : clip_off + clip_sz],
                "packed_size": packed_sz,
            }
        self._cache: dict[tuple[str, int], list[np.ndarray]] = {}

    def get_clip_frames(self, name: str, size: int) -> list[np.ndarray]:
        key = (name, size)
        if key in self._cache:
            return self._cache[key]
        clip = self.clips[name]
        frames_count = clip["frames"]
        payload = clip["payload"]
        expected_packed = self.width * self.height // 2
        prev_packed: np.ndarray | None = None
        decoded: list[np.ndarray] = []

        for idx in range(frames_count):
            rec_off, size_flags = FRAME_INDEX.unpack_from(payload, idx * FRAME_INDEX.size)
            comp_sz = size_flags & 0x7FFFFFFF
            is_delta = bool(size_flags >> 31)
            stored_sz, _ = FRAME_HEADER.unpack_from(payload, rec_off)
            if stored_sz != comp_sz:
                raise ValueError(f"{name} frame {idx}: size mismatch")
            comp_data = payload[rec_off + FRAME_HEADER.size : rec_off + FRAME_HEADER.size + comp_sz]
            raw_bytes = zlib.decompress(comp_data)
            if len(raw_bytes) != expected_packed:
                raise ValueError(f"{name} frame {idx}: unpacked size mismatch")
            delta_arr = np.frombuffer(raw_bytes, dtype=np.uint8)
            packed = (
                np.bitwise_xor(prev_packed, delta_arr)
                if (is_delta and prev_packed is not None)
                else delta_arr.copy()
            )
            prev_packed = packed

            pixels = np.empty(packed.size * 2, dtype=np.uint8)
            pixels[0::2] = (packed >> 4) * 17
            pixels[1::2] = (packed & 0x0F) * 17
            gray = pixels.reshape((self.height, self.width))
            if size != self.width:
                img = Image.fromarray(gray, mode="L").resize(
                    (size, size), Image.Resampling.BILINEAR
                )
                gray = np.asarray(img, dtype=np.uint8)
            decoded.append(gray)

        self._cache[key] = decoded
        return decoded


def compile_simulator(root: Path) -> Path:
    compiler = shutil.which("g++")
    if not compiler:
        raise RuntimeError("g++ not found in PATH")
    tmp_dir = root / ".tmp"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    binary = tmp_dir / "grokbot_preview"
    cmd = [
        compiler,
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(root / "main/display"),
        str(root / "main/display/grokbot_director.cc"),
        str(root / "scripts/grokbot_preview.cc"),
        "-o",
        str(binary),
    ]
    subprocess.run(cmd, check=True)
    return binary


def run_simulation(
    binary: Path, atlas_path: Path, scenario: str, seed: int
) -> list[tuple[int, str, int]]:
    res = subprocess.run(
        [str(binary), scenario, "--atlas", str(atlas_path), "--seed", str(seed)],
        check=True,
        capture_output=True,
        text=True,
    )
    frames: list[tuple[int, str, int]] = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            frames.append((int(parts[0]), parts[1], int(parts[2])))
    return frames


def extract_transitions(frames: list[tuple[int, str, int]]) -> list[dict]:
    transitions: list[dict] = []
    for idx, (ms, clip, frame_idx) in enumerate(frames):
        if not transitions or transitions[-1]["clip"] != clip:
            if transitions:
                transitions[-1]["end_ms"] = ms
                transitions[-1]["duration_ms"] = ms - transitions[-1]["start_ms"]
                transitions[-1]["end_step"] = idx - 1
            transitions.append(
                {
                    "index": len(transitions) + 1,
                    "start_ms": ms,
                    "end_ms": ms,
                    "duration_ms": 0,
                    "clip": clip,
                    "start_frame": frame_idx,
                    "start_step": idx,
                    "end_step": idx,
                }
            )
    if transitions and frames:
        last_ms = frames[-1][0]
        transitions[-1]["end_ms"] = last_ms
        transitions[-1]["duration_ms"] = last_ms - transitions[-1]["start_ms"]
        transitions[-1]["end_step"] = len(frames) - 1
    return transitions


def write_stats(path: Path, atlas: AtlasDecoder, frames: list[tuple[int, str, int]],
                transitions: list[dict]) -> dict:
    idle = np.asarray(atlas.get_clip_frames("idle", atlas.width), dtype=np.float32)
    reference_mae = np.abs(idle - idle[2]).mean(axis=(1, 2))
    base_frames = [index for _, clip, index in frames if clip == "idle"]
    adjacent_mae = [
        float(np.abs(idle[index] - idle[old_index]).mean())
        for (_, old_clip, old_index), (_, clip, index) in zip(frames, frames[1:])
        if old_clip == clip == "idle"
    ]
    waits = [
        {"start_s": tr["start_ms"] / 1000, "seconds": tr["duration_ms"] / 1000}
        for tr in transitions[:-1] if tr["clip"] == "idle"
    ]
    stats = {
        "idle_base_frames": len(base_frames),
        "idle_nonstatic_reference_percent": round(
            100 * sum(reference_mae[i] > 0.01 for i in base_frames) / max(1, len(base_frames)), 2
        ),
        "idle_moving_adjacent_percent": round(
            100 * sum(mae > 0.01 for mae in adjacent_mae) / max(1, len(adjacent_mae)), 2
        ),
        "idle_waits": waits,
        "mae_threshold": 0.01,
    }
    path.write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")
    return stats


def write_timeline(path: Path, scenario: str, transitions: list[dict]) -> None:
    col_idx = "idx"
    lines = [
        f"Scenario: {scenario} (transitions={len(transitions)})",
        f"{col_idx:>3}  {'start_ms':>8}  {'time_s':>7}  {'dur_s':>6}  {'clip':<16}  {'start_frame':>11}",
        "-" * 62,
    ]
    for t in transitions:
        lines.append(
            f"{t['index']:3d}  {t['start_ms']:8d}  {t['start_ms']/1000.0:6.2f}s  "
            f"{t['duration_ms']/1000.0:5.2f}s  {t['clip']:<16}  {t['start_frame']:11d}"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def annotate_frame(gray: np.ndarray, ms: int, clip: str, frame_idx: int) -> np.ndarray:
    img = Image.fromarray(gray, mode="L")
    draw = ImageDraw.Draw(img)
    font = ImageFont.load_default()
    text1 = f"{ms / 1000.0:6.2f}s"
    text2 = f"{clip} [{frame_idx}]"
    draw.rectangle((2, 2, 128, 28), fill=0)
    draw.text((5, 3), text1, fill=255, font=font)
    draw.text((5, 15), text2, fill=220, font=font)
    return np.asarray(img, dtype=np.uint8)


def render_contact_sheet(
    path: Path,
    atlas: AtlasDecoder,
    frames: list[tuple[int, str, int]],
    transitions: list[dict],
    thumb_size: int = 140,
) -> None:
    count = len(transitions)
    cols = min(5, max(1, count))
    rows = math.ceil(count / cols)
    pad = 8
    label_h = 30
    cell_w = thumb_size + pad * 2
    cell_h = thumb_size + label_h + pad * 2

    sheet = Image.new("L", (cols * cell_w, rows * cell_h), color=24)
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()

    for idx, tr in enumerate(transitions):
        r = idx // cols
        c = idx % cols
        x0 = c * cell_w + pad
        y0 = r * cell_h + pad

        span = tr["end_step"] - tr["start_step"]
        sample_step = tr["start_step"] + min(span // 2, 25)
        _, clip_name, sample_frame_idx = frames[sample_step]
        clip_frames = atlas.get_clip_frames(clip_name, thumb_size)
        frame_gray = clip_frames[sample_frame_idx]

        thumb_img = Image.fromarray(frame_gray, mode="L")
        sheet.paste(thumb_img, (x0, y0))
        draw.rectangle((x0, y0, x0 + thumb_size - 1, y0 + thumb_size - 1), outline=110)

        header = f"#{tr['index']} {tr['start_ms']/1000.0:.2f}s ({tr['duration_ms']/1000.0:.1f}s)"
        sub = f"{tr['clip']} (f{tr['start_frame']}->{sample_frame_idx})"
        draw.text((x0 + 2, y0 + thumb_size + 2), header, fill=240, font=font)
        draw.text((x0 + 2, y0 + thumb_size + 15), sub, fill=190, font=font)

    sheet.save(path)


def render_video(
    path: Path,
    atlas: AtlasDecoder,
    frames: list[tuple[int, str, int]],
    size: int,
    out_fps: int,
    speed: float,
) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found in PATH")

    step = (30.0 * speed) / out_fps
    indices: list[int] = []
    pos = 0.0
    while int(pos) < len(frames):
        indices.append(int(pos))
        pos += step

    cmd = [
        ffmpeg,
        "-y",
        "-loglevel",
        "error",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "gray",
        "-s",
        f"{size}x{size}",
        "-r",
        str(out_fps),
        "-i",
        "-",
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        "-crf",
        "20",
        "-preset",
        "fast",
        str(path),
    ]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    assert proc.stdin is not None
    try:
        for i in indices:
            ms, clip_name, frame_idx = frames[i]
            clip_frames = atlas.get_clip_frames(clip_name, size)
            gray = clip_frames[frame_idx]
            annotated = annotate_frame(gray, ms, clip_name, frame_idx)
            proc.stdin.write(annotated.tobytes())
    finally:
        proc.stdin.close()
        ret = proc.wait()
        if ret != 0:
            raise RuntimeError(f"ffmpeg exited with status {ret}")


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "scenario",
        nargs="?",
        default="all",
        choices=(*SCENARIOS, "all"),
        help="scenario to render (default: all)",
    )
    parser.add_argument(
        "--atlas",
        type=Path,
        default=root / "main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas",
        help="path to grokbot.atlas",
    )
    parser.add_argument("--size", type=int, default=180, help="video resolution (default: 180)")
    parser.add_argument("--fps", type=int, default=15, help="output video fps (default: 15)")
    parser.add_argument(
        "--speed",
        type=float,
        default=None,
        help="playback speed multiplier (default: 8.0 for boot_idle, 1.0 otherwise)",
    )
    parser.add_argument("--seed", type=int, default=42, help="director PRNG seed (default: 42)")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=root / ".tmp/preview",
        help="output directory (default: .tmp/preview)",
    )
    args = parser.parse_args()
    for name in ("speed", "fps", "size"):
        value = getattr(args, name)
        if value is not None and (value <= 0 or (isinstance(value, float) and not math.isfinite(value))):
            parser.error(f"--{name} must be a finite positive number")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    binary = compile_simulator(root)
    atlas = AtlasDecoder(args.atlas)

    targets = SCENARIOS if args.scenario == "all" else (args.scenario,)
    for sc in targets:
        speed = args.speed if args.speed is not None else (8.0 if sc == "boot_idle" else 1.0)
        frames = run_simulation(binary, args.atlas, sc, args.seed)
        transitions = extract_transitions(frames)

        timeline_path = args.out_dir / f"{sc}_timeline.txt"
        contact_path = args.out_dir / f"{sc}_contact.png"
        video_path = args.out_dir / f"{sc}.mp4"

        stats = write_stats(args.out_dir / f"{sc}_stats.json", atlas, frames, transitions)
        write_timeline(timeline_path, sc, transitions)
        render_contact_sheet(contact_path, atlas, frames, transitions)
        render_video(video_path, atlas, frames, args.size, args.fps, speed)
        print(
            f"[{sc}] speed={speed:g}x frames={len(frames)} transitions={len(transitions)} "
            f"idle motion={stats['idle_moving_adjacent_percent']}% -> "
            f"{video_path}, {timeline_path}, {contact_path}"
        )


if __name__ == "__main__":
    main()
