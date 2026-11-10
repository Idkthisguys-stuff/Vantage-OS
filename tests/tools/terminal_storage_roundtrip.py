"""Exercise terminal file commands against a regular disk image across reboots."""
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from storage_image import new_image

with tempfile.TemporaryDirectory(prefix='vantage-editor-') as directory:
    for version in (2, 3):
        path = pathlib.Path(directory) / f'storage-v{version}.img'
        original = new_image(version=version)
        path.write_bytes(original)
        for action in ('write', 'read', 'deleted'):
            subprocess.run([sys.argv[1], action, str(path)], check=True)
        if version == 3:
            subprocess.run([sys.argv[1], 'capacity', str(path)], check=True)
        assert path.read_bytes()[:2048 * 512] == original[:2048 * 512]
