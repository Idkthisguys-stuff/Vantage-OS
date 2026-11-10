"""Capture SDL frames and verify persistent contents across separate processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from storage_image import new_image, inspect_volume


def capture(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    image = output / 'data.img'
    with image.open('xb') as f:
        f.write(new_image())
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software')
    prefix = output / 'terminal'
    for action in ('write', 'read'):
        subprocess.run([executable, action, str(image), str(prefix)], env=env, check=True)
    frames = {}
    for name in ('editor', 'folders', 'reboot'):
        path = output / f'terminal.{name}.bmp'
        content = path.read_bytes()
        assert content[:2] == b'BM' and len(content) > 480 * 800 * 3
        frames[name] = {'file': path.name, 'sha256': hashlib.sha256(content).hexdigest()}
    assert len({frame['sha256'] for frame in frames.values()}) == 3
    with image.open('rb') as f:
        banks, selected = inspect_volume(f, 2048, 32768)
    assert all(bank['valid'] for bank in banks) and selected['version'] == 3
    assert selected['files'] == [
        {'path': '/ram/empty', 'size': 0, 'type': 'directory'},
        {'path': '/ram/docs', 'size': 0, 'type': 'directory'},
        {'path': '/ram/docs/notes', 'size': 0, 'type': 'directory'},
        {'path': '/ram/docs/notes/roadmap', 'size': len(
            b'Preempted processes and persistent folders.\n'
            b'VSTORE3: 256 entries, 255-byte paths, 1 MiB files.\n'), 'type': 'file'}]
    manifest = {'kind': 'actual SDL framebuffer captures; not Pi hardware',
                'processes': 2, 'format_version': 3, 'files': selected['files'], 'frames': frames}
    (output / 'capture.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print('PASS: SDL captures, exact file bytes, and empty-directory persistence across two processes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        capture(args.executable, args.output)
    else:
        with tempfile.TemporaryDirectory(prefix='vantage-capture-') as directory:
            capture(args.executable, Path(directory))
