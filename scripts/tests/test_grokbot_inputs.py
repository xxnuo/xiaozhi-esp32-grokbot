import pathlib
import shutil
import subprocess
import unittest


class GrokBotInputsTests(unittest.TestCase):
    def test_touch_motion_and_shared_cooldown(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("g++ is unavailable")
        root = pathlib.Path(__file__).resolve().parents[2]
        output = root / ".tmp" / "grokbot-inputs-test"
        output.parent.mkdir(exist_ok=True)
        subprocess.run(
            [compiler, "-std=c++17", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
             "-I", str(root / "main/boards/m5stack/stopwatch-grokbot"),
             str(root / "scripts/tests/grokbot_inputs_test.cc"), "-o", str(output)],
            check=True, capture_output=True, text=True,
        )
        result = subprocess.run([str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
