import struct
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


if __name__ == "__main__":
    unittest.main()
