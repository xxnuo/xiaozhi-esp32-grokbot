import pathlib
import re
import shutil
import subprocess
import unittest


class CaptionTests(unittest.TestCase):
    def test_all_director_clips_have_short_captions(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "main/display/grokbot_director.cc").read_text()
        clips = re.findall(r'"([a-z-]+)"', source.split("kClips[] = {", 1)[1].split("};", 1)[0])
        header = (root / "main/display/grokbot_caption.h").read_text()
        captions = dict(re.findall(r'\{"([a-z-]+)", "([^"\n]+)"\}', header))
        self.assertEqual(set(clips), set(captions))
        for clip, text in captions.items():
            self.assertTrue(0 < len(text) <= 20, (clip, text))
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("requires g++")
        subprocess.run([compiler, "-std=c++17", "-fsyntax-only", "-x", "c++",
                        "-I", str(root / "main/display"), "-"],
                       input='#include "grokbot_caption.h"\n', text=True, check=True)
