import pathlib
import shutil
import subprocess
import unittest


class GrokBotDirectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("g++ is unavailable")
        root = pathlib.Path(__file__).resolve().parents[2]
        output = root / ".tmp" / "grokbot-director-test"
        output.parent.mkdir(exist_ok=True)
        subprocess.run(
            [compiler, "-std=c++17", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
             "-I", str(root / "main/display"),
             str(root / "main/display/grokbot_director.cc"),
             str(root / "scripts/tests/grokbot_director_test.cc"), "-o", str(output)],
            check=True, capture_output=True, text=True,
        )
        cls.driver = output

    def run_case(self, name):
        result = subprocess.run([str(self.driver), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stages_and_repeated_idle_status(self):
        self.run_case("stages")

    def test_fidget_cooldown_dedup_and_seed(self):
        self.run_case("fidgets")

    def test_rare_clip_cooldowns(self):
        self.run_case("rare")

    def test_thinking_chain(self):
        self.run_case("thinking")

    def test_emotions_and_timeout(self):
        self.run_case("emotions")

    def test_finished_and_failed(self):
        self.run_case("transitions")

    def test_waking_and_power_save(self):
        self.run_case("wake")

    def test_legacy_atlas(self):
        self.run_case("legacy")

    def test_notifications_expiry_and_priority(self):
        self.run_case("notifications")


if __name__ == "__main__":
    unittest.main()
