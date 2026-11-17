import pathlib
import shutil
import subprocess
import unittest


class GrokBotFrameCursorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("g++ is unavailable")
        root = pathlib.Path(__file__).resolve().parents[2]
        output = root / ".tmp" / "grokbot-frame-cursor-test"
        output.parent.mkdir(exist_ok=True)
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
             "-I", str(root / "main/display"),
             str(root / "scripts/tests/grokbot_frame_cursor_test.cc"), "-o", str(output)],
            check=True, capture_output=True, text=True,
        )
        cls.driver = output

    def run_case(self, name):
        result = subprocess.run([str(self.driver), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_single_playable_delta_frame(self):
        self.run_case("single")

    def test_two_frame_pingpong_and_boundaries(self):
        self.run_case("pingpong")

    def test_middle_keyframe(self):
        self.run_case("keyframe")

    def test_max_frames_limit(self):
        self.run_case("limit")

    def test_presentation_failure_retry(self):
        self.run_case("retry")


if __name__ == "__main__":
    unittest.main()
