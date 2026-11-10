import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('vantage_build', ROOT / 'tools/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildHelperTests(unittest.TestCase):
    def test_portable_tool_directories(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'xpack-windows-build-tools-1' / 'bin').mkdir(parents=True)
            self.assertIn(root / 'bin', build.tool_dirs(root))
            self.assertIn(root / 'xpack-windows-build-tools-1' / 'bin', build.tool_dirs(root))

    def test_argument_validation(self):
        for args in (['--jobs', '0'], ['--storage-partition', '4'],
                     ['--sd-base-clock-hz', '1'], ['--v3d', 'yes'], ['test', '--sdl', 'x']):
            result = subprocess.run([sys.executable, str(ROOT / 'tools/build.py'), *args],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, args)
            self.assertIn('error:', result.stderr)

    def test_default_options(self):
        args = build.parser().parse_args([])
        self.assertEqual(args.target, 'kernel')
        self.assertIsNone(args.wifi)
        self.assertIsNone(args.storage_validation)


if __name__ == '__main__':
    unittest.main()
