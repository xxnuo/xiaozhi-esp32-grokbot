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
        # Geometry-to-geometry uses its internal bridge, so there is no six-frame atlas fade.
        for offset in range(0, len(sleeping) - 6, 6):
            group = sleeping[offset:offset + 6]
            self.assertEqual([index for _, _, index in group], [2 + offset // 6] * 6)
            self.assertEqual(sleeping[offset + 6][0] - group[0][0], 200)
        waking = [frame for frame in frames if 10000 <= frame[0] < 10200]
        self.assertEqual([index for _, clip, index in waking], list(range(2, 8)))
        self.assertTrue(all(clip == "waking" for _, clip, _ in waking))
        first_wake = [frame for i, frame in enumerate(frames)
                      if 10000 <= frame[0] < 13400 and frames.presentations[i]]
        self.assertEqual([index for _, _, index in first_wake], list(range(2, 73)))
        self.assertTrue(all(clip == "waking" for _, clip, _ in first_wake))
        after_wake = next(clip for ms, clip, _ in frames if ms == 13400)
        self.assertNotIn(after_wake, ("waking", "thinking", "streaming"))
        self.assertTrue(all(clip == "waking" for ms, clip, _ in frames if 21000 <= ms < 24400))
        self.assertEqual(next(clip for ms, clip, _ in frames if ms == 24400), "thinking")
        # Business changes reset cadence even while the visual waking clip remains selected.
        after_status = [(ms, frames.presentations[i]) for i, (ms, _, _) in enumerate(frames)
                        if 10300 <= ms < 10450]
        self.assertEqual(after_status, [(10300, True), (10333, False), (10366, True),
                                        (10400, False), (10433, True)])

    def test_real_face_renderer_and_icon_bridge_use_firmware_time(self):
        if not shutil.which("g++"):
            self.skipTest("g++ unavailable")
        preview, root = self.preview, self.root
        binary = preview.compile_simulator(root)
        atlas_path = root / "main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas"
        frames = preview.run_simulation(binary, atlas_path, "conversation", 42)
        emotion = next(i for i, frame in enumerate(frames) if frame[0] == 12500)
        self.assertEqual(frames.face_modes[emotion], 1)
        self.assertEqual(frames.fade_steps[emotion], 6)
        bridge = [i for i, (ms, _, _) in enumerate(frames) if 5500 <= ms < 5850]
        self.assertEqual(len(bridge), 11)
        self.assertTrue(all(frames.face_modes[i] == 2 and frames[i][2] == 0 and
                            frames.fade_steps[i] == 6 for i in bridge))
        icon = bridge[-1] + 1
        self.assertEqual(frames[icon][:3], (5866, "thinking", 0))
        self.assertEqual(frames.face_modes[icon], 0)
        for start, end, count in ((3500, 5500, 40), (11500, 12500, 20), (30000, 31000, 30)):
            presented = [ms for i, (ms, _, _) in enumerate(frames)
                         if start <= ms < end and frames.presentations[i]]
            self.assertEqual(len(presented), count)
            if count < (end - start) * 30 // 1000:
                self.assertTrue(all(33 <= b - a <= 67 for a, b in zip(presented, presented[1:])))
        for i, (_, _, index) in enumerate(frames):
            if not frames.presentations[i]:
                self.assertEqual(index, frames[i - 1][2])

        # Compare the bridge's packed bytes with a separate, direct firmware Render() caller.
        atlas = preview.AtlasDecoder(atlas_path)
        source = '''#include "grokbot_face.h"
#include <cstdio>
#include <vector>
int main() {
    GrokBotFace face;
    std::vector<unsigned char> packed(360 * 360 / 2);
    face.Select("idle", 1, 0);
    for (int ms : {0, 33, 66, 100}) {
        if (!face.Render(packed.data(), 360, 360, ms)) return 1;
        if (std::fwrite(packed.data(), 1, packed.size(), stdout) != packed.size()) return 2;
    }
}'''
        with tempfile.TemporaryDirectory(dir=root / ".tmp") as directory:
            driver = Path(directory) / "direct-face"
            subprocess.run(
                ["g++", "-std=c++17", "-O2", "-I", str(root / "main/display"),
                 str(root / "main/display/grokbot_face.cc"), "-x", "c++", "-", "-o", str(driver)],
                input=source, text=True, check=True, capture_output=True,
            )
            expected = subprocess.run([str(driver)], capture_output=True, check=True).stdout
        renderer = preview.FaceRenderer(frames, atlas)
        try:
            actual = []
            for ms in (0, 33, 66, 100):
                gray = renderer.frame(ms, "idle", 1, 1, True).reshape(-1) // 17
                actual.append(((gray[0::2] << 4) | gray[1::2]).tobytes())
            self.assertEqual(b"".join(actual), expected)
        finally:
            renderer.close()

    def test_interactions_are_labelled_simulations_and_busy_input_is_ignored(self):
        if not shutil.which("g++"):
            self.skipTest("g++ unavailable")
        preview, root = self.preview, self.root
        binary = preview.compile_simulator(root)
        frames = preview.run_simulation(
            binary, root / "main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas",
            "interactions", 42,
        )
        self.assertEqual([(event["gesture"], event["emotion"], event["accepted"])
                          for event in frames.interactions], [
            ("tap", "happy", True), ("stroke", "loving", True),
            ("shake", "confused", True), ("tilt", "curious", True), ("tap", "happy", False),
        ])
        self.assertTrue(all(clip == "listening" for ms, clip, _ in frames if 28500 <= ms < 31000))
        with tempfile.TemporaryDirectory(dir=root / ".tmp") as directory:
            timeline = Path(directory) / "timeline.txt"
            preview.write_timeline(timeline, "interactions", preview.extract_transitions(frames),
                                   frames.interactions)
            self.assertIn("SIMULATED input at 4000ms: tap -> happy (accepted)", timeline.read_text())
            self.assertIn("ignored while busy", timeline.read_text())


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
