import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'tools' / 'build_config.py'
ROOT = SCRIPT.parents[1]


class BuildConfigTests(unittest.TestCase):
    def test_make_defaults_and_validation_requirements(self):
        make = shutil.which('make') or shutil.which('gmake')
        if not make:
            self.skipTest('GNU make is not on PATH')
        def invoke(*args):
            return subprocess.run([make, '-n', *args], cwd=ROOT, capture_output=True, text=True)
        default = invoke()
        self.assertEqual(default.returncode, 0, default.stderr)
        for flag in ('-DVANTAGE_WIFI=1', '-DVANTAGE_WIFI_ASSETS=0',
                     '-DVANTAGE_STORAGE=1', '-DVANTAGE_STORAGE_VALIDATION=0',
                     '-DSTORAGE_PARTITION=-1'):
            self.assertIn(flag, default.stdout)
        for args in (('STORAGE_VALIDATION=1',),
                     ('STORAGE_VALIDATION=1', 'SD_BASE_CLOCK_HZ=100000000'),
                     ('STORAGE_PARTITION=foo',), ('STORAGE=0', 'STORAGE_VALIDATION=1')):
            self.assertNotEqual(invoke(*args).returncode, 0, args)

    def test_unchanged_configuration_keeps_timestamp_and_changed_flags_replace_it(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'nested' / 'build-config'
            command = [sys.executable, '-B', str(SCRIPT), '--output', str(output),
                       '--cc=gcc', '--ld=ld', '--objcopy=objcopy', '--firmware=']
            subprocess.run(command + ['--cflags=-DVANTAGE_STORAGE_VALIDATION=0'], check=True)
            os.utime(output, ns=(1_000_000_000, 1_000_000_000))
            stamp = output.stat().st_mtime_ns
            subprocess.run(command + ['--cflags=-DVANTAGE_STORAGE_VALIDATION=0'], check=True)
            self.assertEqual(output.stat().st_mtime_ns, stamp)
            subprocess.run(command + ['--cflags=-DVANTAGE_STORAGE_VALIDATION=1'], check=True)
            self.assertNotEqual(output.stat().st_mtime_ns, stamp)
            self.assertEqual(json.loads(output.read_text())['cflags'], '-DVANTAGE_STORAGE_VALIDATION=1')


if __name__ == '__main__':
    unittest.main()
