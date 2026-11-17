import pathlib
import shutil
import subprocess
import sys
import unittest


class GrokBotFaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("g++ is unavailable")
        cls.root = pathlib.Path(__file__).resolve().parents[2]
        cls.driver = cls.root / ".tmp/grokbot-face-test"
        cls.driver.parent.mkdir(exist_ok=True)
        subprocess.run([
            compiler, "-std=c++17", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
            "-I", str(cls.root / "main/display"),
            str(cls.root / "main/display/grokbot_face.cc"),
            str(cls.root / "scripts/tests/grokbot_face_test.cc"), "-o", str(cls.driver),
        ], check=True, capture_output=True, text=True)

    def run_case(self, name):
        result = subprocess.run([str(self.driver), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_interrupts_continue_from_displayed_geometry(self):
        self.run_case("continuity")

    def test_idle_recipes_and_blink_exclusions(self):
        self.run_case("idle")

    def test_geometry_changes_are_not_pixel_fades(self):
        self.run_case("geometry")

    def test_intermediate_strokes_fill_self_intersections_without_white_holes(self):
        self.run_case("topology")

    def test_atlas_bridge_returns_to_shared_start_pose(self):
        self.run_case("atlas")

    def test_scratch_matches_host_path_and_bounds_are_checked(self):
        self.run_case("scratch")

    def test_fused_rgb_matches_expansion_and_failed_present_preserves_snapshot(self):
        self.run_case("fused")

    def test_extracted_constants_are_reproducible(self):
        if not (self.root / "research/grokbot.yin.wiki/js/vendor/grok-recipes.json").exists():
            self.skipTest("local yin.wiki research checkout is required for recipe extraction")
        subprocess.run([sys.executable, str(self.root / "scripts/export_grokbot_face.py"), "--check"], check=True)


if __name__ == "__main__":
    unittest.main()
