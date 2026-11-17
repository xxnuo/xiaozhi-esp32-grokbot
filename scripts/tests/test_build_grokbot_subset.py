import struct
import json
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from scripts.build_grokbot_subset import (
    CLIP_HEADER,
    FRAME_HEADER,
    FRAME_INDEX,
    GLOBAL_HEADER,
    XIAOZHI_STATES,
    downsample_supersampled_gray,
    rebuild_clip,
    unpack_frame,
)


ROOT = Path(__file__).resolve().parents[2]
EXCLUDED_CLIPS = (
    "bored",
    "blocked",
    "orbit",
    "progress",
    "dragging",
    "showcase",
    "reading",
    "coding",
    "running",
    "computing",
    "controlling",
    "bouncing",
    "dictating",
    "sending",
)


def make_atlas(path: Path, names: tuple[str, ...]) -> None:
    clip_payloads = [b"\x5a" * 64 for _ in names]
    offset = GLOBAL_HEADER.size + len(names) * 32
    headers = []
    for name, payload in zip(names, clip_payloads, strict=True):
        headers.append(
            struct.pack(
                "<16sHHIII",
                name.encode("ascii"),
                3,
                2,
                offset,
                len(payload),
                400 * 400 // 2,
            )
        )
        offset += len(payload)
    path.write_bytes(
        GLOBAL_HEADER.pack(b"GBZ1", 1, 400, 400, 30, len(names), 0)
        + b"".join(headers)
        + b"".join(clip_payloads)
    )


class BuildGrokbotSubsetTest(unittest.TestCase):
    def test_xiaozhi_states_inventory(self):
        self.assertEqual(len(XIAOZHI_STATES), 39)
        self.assertEqual(len(set(XIAOZHI_STATES)), 39)
        for clip in EXCLUDED_CLIPS:
            self.assertNotIn(clip, XIAOZHI_STATES)

    def test_output_contains_only_xiaozhi_states(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "full.atlas"
            output = root / "subset.atlas"
            make_atlas(source, XIAOZHI_STATES + ("unused-showcase",))

            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "build_grokbot_subset.py"),
                    str(source),
                    str(output),
                    "--max-size",
                    "0x10000",
                ],
                check=True,
            )

            data = output.read_bytes()
            magic, version, width, height, fps, count, _ = GLOBAL_HEADER.unpack_from(data)
            self.assertEqual((magic, version, width, height, fps), (b"GBZ1", 1, 400, 400, 30))
            self.assertEqual(count, len(XIAOZHI_STATES))
            names = []
            for index in range(count):
                header = data[GLOBAL_HEADER.size + index * 32 : GLOBAL_HEADER.size + (index + 1) * 32]
                names.append(header[:16].split(b"\0", 1)[0].decode("ascii"))
            self.assertEqual(tuple(names), XIAOZHI_STATES)

    def test_missing_state_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "full.atlas"
            make_atlas(source, XIAOZHI_STATES[:-1])
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "build_grokbot_subset.py"),
                    str(source),
                    str(root / "subset.atlas"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing states", result.stderr)

    def test_rebuild_clip_resizes_and_reencodes_zlib_delta_frames(self):
        try:
            import numpy  # noqa: F401
            from PIL import Image  # noqa: F401
        except ImportError:
            self.skipTest("numpy and Pillow are required for resize fixture test")

        width = height = 8
        target_size = 4
        packed_len = width * height // 2
        frame0 = bytes([0x33] * (packed_len // 2) + [0xCC] * (packed_len // 2))
        frame1 = bytes([0xFF] * (packed_len // 2) + [0x00] * (packed_len // 2))
        delta1 = bytes(a ^ b for a, b in zip(frame0, frame1, strict=True))

        z0 = zlib.compress(frame0, 9)
        z1 = zlib.compress(delta1, 9)
        rec0 = FRAME_HEADER.pack(len(z0), 0) + z0
        rec1 = FRAME_HEADER.pack(len(z1), 1) + z1
        index = FRAME_INDEX.pack(16, len(z0)) + FRAME_INDEX.pack(
            16 + len(rec0), len(z1) | (1 << 31)
        )
        clip = {
            "name": "idle",
            "frames": 2,
            "payload": index + rec0 + rec1,
        }

        rebuilt = rebuild_clip(clip, width, height, target_size)
        small_packed_len = target_size * target_size // 2
        off0, sz_flags0 = FRAME_INDEX.unpack_from(rebuilt, 0)
        off1, sz_flags1 = FRAME_INDEX.unpack_from(rebuilt, FRAME_INDEX.size)
        self.assertEqual(sz_flags0 >> 31, 0)
        self.assertEqual(sz_flags1 >> 31, 1)

        sz0, flag0 = FRAME_HEADER.unpack_from(rebuilt, off0)
        self.assertEqual((sz0, flag0), (sz_flags0 & 0x7FFFFFFF, 0))
        dec0 = zlib.decompress(rebuilt[off0 + FRAME_HEADER.size : off0 + FRAME_HEADER.size + sz0])
        self.assertEqual(len(dec0), small_packed_len)

        sz1, flag1 = FRAME_HEADER.unpack_from(rebuilt, off1)
        self.assertEqual((sz1, flag1), (sz_flags1 & 0x7FFFFFFF, 1))
        dec1_delta = zlib.decompress(
            rebuilt[off1 + FRAME_HEADER.size : off1 + FRAME_HEADER.size + sz1]
        )
        dec1 = bytes(a ^ b for a, b in zip(dec0, dec1_delta, strict=True))
        self.assertEqual(len(dec1), small_packed_len)

        pixels0 = unpack_frame(dec0, target_size, target_size)
        pixels1 = unpack_frame(dec1, target_size, target_size)
        self.assertEqual(int(pixels0[0, 0]), 3)
        self.assertEqual(int(pixels0[-1, -1]), 12)
        self.assertEqual(int(pixels1[0, 0]), 15)
        self.assertEqual(int(pixels1[-1, -1]), 0)

    def test_baked_directory_encodes_supersampled_frames(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy is required for baked directory fixture test")

        hi_frame = np.zeros((16, 16), dtype=np.uint8)
        hi_frame[:, 6:] = 255
        aa = downsample_supersampled_gray(hi_frame, 4)
        self.assertEqual(aa.shape, (4, 4))
        self.assertEqual(int(aa[0, 0]), 0)
        self.assertEqual(int(aa[0, 1]), 128)
        self.assertEqual(int(aa[0, 2]), 255)
        self.assertEqual(int(aa[0, 3]), 255)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baked_dir = root / "baked"
            baked_dir.mkdir()
            output = root / "baked.atlas"
            states_meta = []
            for name in XIAOZHI_STATES:
                raw = np.zeros((3, 4, 4), dtype=np.uint8)
                raw[1, :, :2] = 136
                raw[2, :, :] = 255
                (baked_dir / f"{name}.raw").write_bytes(raw.tobytes())
                states_meta.append({"state": name, "count": 3, "start_frame": 2})
            (baked_dir / "summary.json").write_text(
                json.dumps({"fps": 30, "size": 4, "supersample": 4, "states": states_meta}),
                encoding="utf-8",
            )

            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "build_grokbot_subset.py"),
                    str(baked_dir),
                    str(output),
                    "--size",
                    "4",
                    "--max-size",
                    "0x10000",
                ],
                check=True,
            )

            data = output.read_bytes()
            magic, version, width, height, fps, count, _ = GLOBAL_HEADER.unpack_from(data)
            self.assertEqual((magic, version, width, height, fps), (b"GBZ1", 1, 4, 4, 30))
            self.assertEqual(count, len(XIAOZHI_STATES))
            _, frames, start_frame, _, _, packed_size = CLIP_HEADER.unpack_from(
                data, GLOBAL_HEADER.size
            )
            self.assertEqual((frames, start_frame, packed_size), (3, 2, 8))

    def test_supersampled_box_filter_preserves_moving_thin_line_brightness(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy is required for thin-line shift invariance test")

        vertical_sums = set()
        horizontal_sums = set()
        for pos in range(4, 28):
            v_hi = np.zeros((32, 32), dtype=np.uint8)
            v_hi[:, pos] = 240
            v_lo = downsample_supersampled_gray(v_hi, 4)
            vertical_sums.add(int(v_lo.sum()))

            h_hi = np.zeros((32, 32), dtype=np.uint8)
            h_hi[pos, :] = 240
            h_lo = downsample_supersampled_gray(h_hi, 4)
            horizontal_sums.add(int(h_lo.sum()))

        self.assertEqual(len(vertical_sums), 1)
        self.assertEqual(len(horizontal_sums), 1)
        self.assertEqual(vertical_sums, {8 * 60})
        self.assertEqual(horizontal_sums, {8 * 60})

    def test_bake_script_vector_precision_patches(self):
        script_url = (ROOT / "scripts" / "bake_grokbot_atlas.mjs").as_uri()
        upstream_player = (
            ROOT / "research" / "Stopwatch-GrokBot" / "third_party" / "grokbot" / "grok-player.js"
        )
        node_code = f"""
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {{ upgradeVectorPrecision }} from {json.dumps(script_url)};

const fixture = [
  'const JM = .02, eT = 16, tT = .015, nT = 400, rT = 512;',
  'const yT = 192, kT = Math.PI / 48;',
  'const kE = hM(192);',
  '    $h = Array.from({{ length: 96 }}, (e, t) => {{ // $h: 96-gon ring',
  '      const n = t / 96 * TAU;',
  '      return [x + Math.cos(n) * x, x + Math.sin(n) * x];',
  '    }});',
].join('\\n');

const patched = upgradeVectorPrecision(fixture);
assert.ok(patched.includes('const JM = .0005, eT = 20, tT = .003, nT = 1200'));
assert.ok(patched.includes('const yT = 768, kT = Math.PI / 192;'));
assert.ok(patched.includes('const kE = hM(768);'));
assert.ok(patched.includes('$h = Array.from({{ length: 384 }}, (e, t) => {{'));
assert.ok(patched.includes('const n = t / 384 * TAU;'));
assert.ok(!patched.includes('length: 96'));

assert.throws(() => upgradeVectorPrecision(patched), /expected exactly 1 match/);
assert.throws(() => upgradeVectorPrecision(fixture + '\\nconst kE = hM(192);'), /expected exactly 1 match/);

const upstreamPath = {json.dumps(str(upstream_player))};
if (fs.existsSync(upstreamPath)) {{
  const upstream = fs.readFileSync(upstreamPath, 'utf8');
  const upPatched = upgradeVectorPrecision(upstream);
  assert.ok(upPatched.includes('$h = Array.from({{ length: 384 }}'));
  assert.ok(!upPatched.includes('$h = Array.from({{ length: 96 }}'));
}}
"""
        subprocess.run(["node", "--input-type=module", "-e", node_code], check=True)


if __name__ == "__main__":
    unittest.main()
