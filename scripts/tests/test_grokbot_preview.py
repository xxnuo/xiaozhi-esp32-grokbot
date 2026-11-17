import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest


class GrokBotPreviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location("grokbot_preview", root / "scripts/grokbot_preview.py")
        preview = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(preview)
        except ModuleNotFoundError as error:
            raise unittest.SkipTest(f"optional preview dependency unavailable: {error}")
        cls.root = root
        cls.preview = preview

    def test_motion_stats_exclude_other_clips_and_count_held_frames(self):
        root, preview = self.root, self.preview
        np = preview.np

        class Atlas:
            width = 2

            def get_clip_frames(self, name, size):
                self_frames = np.zeros((4, 2, 2), dtype=np.uint8)
                self_frames[3] = 17
                return self_frames

        frames = [(0, "idle", 2), (33, "idle", 3), (66, "idle", 3), (100, "happy", 2)]
        transitions = preview.extract_transitions(frames)
        (root / ".tmp").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root / ".tmp") as directory:
            path = Path(directory) / "stats.json"
            stats = preview.write_stats(path, Atlas(), frames, transitions)
            self.assertEqual(stats["idle_base_frames"], 3)
            self.assertEqual(stats["idle_nonstatic_reference_percent"], 66.67)
            self.assertEqual(stats["idle_moving_adjacent_percent"], 50.0)
            self.assertEqual(stats["idle_waits"], [{"start_s": 0, "seconds": 0.1}])
            self.assertEqual(json.loads(path.read_text()), stats)

    def test_power_save_sleep_advances_at_five_fps(self):
        if not shutil.which("g++"):
            self.skipTest("g++ unavailable")
        preview, root = self.preview, self.root
        binary = preview.compile_simulator(root)
        frames = preview.run_simulation(
            binary, root / "main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas",
            "wake_from_sleep", 42,
        )
        sleeping = [frame for frame in frames if 6133 <= frame[0] < 10000]
        self.assertEqual(len(sleeping), 116)
        self.assertTrue(all(clip == "sleeping" for _, clip, _ in sleeping))
        self.assertEqual([index for _, _, index in sleeping[:6]], list(range(2, 8)))
        sleeping = sleeping[6:]
        for offset in range(0, len(sleeping) - 6, 6):
            group = sleeping[offset:offset + 6]
            self.assertEqual([index for _, _, index in group], [8 + offset // 6] * 6)
            self.assertEqual(sleeping[offset + 6][0] - group[0][0], 200)
        waking = [frame for frame in frames if 10000 <= frame[0] < 10200]
        self.assertEqual([index for _, clip, index in waking], list(range(2, 8)))
        self.assertTrue(all(clip == "waking" for _, clip, _ in waking))
        first_wake = [frame for frame in frames if 10000 <= frame[0] < 12366]
        self.assertEqual([index for _, _, index in first_wake], list(range(2, 73)))
        self.assertTrue(all(clip == "waking" for _, clip, _ in first_wake))
        after_wake = next(clip for ms, clip, _ in frames if ms == 12366)
        self.assertNotIn(after_wake, ("waking", "thinking", "streaming"))
        self.assertTrue(all(clip == "waking" for ms, clip, _ in frames if 21000 <= ms < 23366))
        self.assertEqual(next(clip for ms, clip, _ in frames if ms == 23366), "thinking")


    def test_six_frame_blend_and_interrupted_fade(self):
        preview = self.preview
        np = preview.np

        class Atlas:
            width = 2
            def get_clip_frames(self, name, size):
                return [np.full((2, 2), 255 if name == "white" else 0, dtype=np.uint8)]

        frames = [(0, "black", 0)] + [(33 * i, "white", 0) for i in range(1, 7)]
        values = [int(gray[0, 0]) for gray in preview.blended_frames(Atlas(), frames, 2)]
        self.assertEqual(values, preview.DISPLAY_GRAY_LUT[[0, 3, 5, 8, 10, 13, 15]].tolist())
        interrupted = frames[:3] + [(100, "black", 0)]
        values = [int(gray[0, 0]) for gray in preview.blended_frames(Atlas(), interrupted, 2)]
        self.assertEqual(values[-1], int(preview.DISPLAY_GRAY_LUT[4]))
        # A new directive can restart the same clip: sequence, not name, drives fading.
        restart = preview.SimulationFrames([(0, "white", 0), (33, "black", 0), (66, "black", 0)])
        restart.sequences = [1, 2, 3]
        values = [int(gray[0, 0]) for gray in preview.blended_frames(Atlas(), restart, 2)]
        self.assertEqual(values, preview.DISPLAY_GRAY_LUT[[15, 13, 11]].tolist())

    def test_invalid_render_arguments_exit_before_creating_output(self):
        root = self.root
        (root / ".tmp").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root / ".tmp") as directory:
            output = Path(directory) / "output"
            for option in ("speed", "fps", "size"):
                for value in ("0", "-1", "nan", "inf", "-inf"):
                    with self.subTest(option=option, value=value):
                        result = subprocess.run(
                            [sys.executable, str(root / "scripts/grokbot_preview.py"),
                             "wake_from_sleep", f"--{option}={value}", "--out-dir", str(output)],
                            capture_output=True, text=True, timeout=10,
                        )
                        self.assertEqual(result.returncode, 2)
                        self.assertIn(f"--{option}", result.stderr)
                        self.assertFalse(output.exists())
