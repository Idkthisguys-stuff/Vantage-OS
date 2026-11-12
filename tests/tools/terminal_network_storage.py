"""Run the terminal network test with a fresh storage image mounted."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from storage_image import new_image

with tempfile.TemporaryDirectory(prefix='vantage-network-') as directory:
    path = Path(directory) / 'storage.img'
    path.write_bytes(new_image(version=3))
    subprocess.run([sys.argv[1]], check=True, env=dict(os.environ, VANTAGE_STORAGE_IMAGE=str(path)))
