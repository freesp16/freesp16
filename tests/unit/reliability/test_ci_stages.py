"""The CI driver must stop at a failed configure or build."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class CiStageFailure(unittest.TestCase):
    def test_build_failure_stops_stage(self):
        source = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "ci").mkdir()
            (root / "tools").mkdir()
            (root / "bin").mkdir()
            shutil.copyfile(source / "ci/run-local.sh", root / "ci/run-local.sh")
            (root / "tools/identity.py").touch()

            cmake = root / "bin/cmake"
            cmake.write_text(
                '#!/bin/sh\nprintf "%s\\n" "$1"\n'
                'if [ "$1" = "$FAIL_AT" ]; then exit 17; fi\n'
            )
            cmake.chmod(0o755)
            ctest = root / "bin/ctest"
            ctest.write_text('#!/bin/sh\nprintf "ctest ran\\n"\n')
            ctest.chmod(0o755)

            for stage in ("host", "bench", "golden", "long"):
                for failure in ("--preset", "--build"):
                    with self.subTest(stage=stage, failure=failure):
                        env = dict(os.environ, PATH=str(root / "bin") + os.pathsep + os.environ["PATH"],
                                   FAIL_AT=failure)
                        result = subprocess.run(
                            ["sh", str(root / "ci/run-local.sh"), stage],
                            env=env, capture_output=True, text=True, timeout=10,
                        )
                        self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
                        self.assertNotIn("ctest ran", result.stdout)
                        self.assertNotIn("passed", result.stdout)
                        if failure == "--preset":
                            self.assertNotIn("--build", result.stdout)


if __name__ == "__main__":
    unittest.main()
