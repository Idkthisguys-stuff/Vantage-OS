"""Run writer and reader in separate processes against a regular disk image."""
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
from storage_image import BANK, SECTOR, MIN_SECTORS, inspect_bank, inspect_volume, migrate, new_image


with tempfile.TemporaryDirectory(prefix='vantage-storage-') as directory:
    path = pathlib.Path(directory) / 'storage.img'
    original = new_image(version=1)  # Old volumes upgrade only on deliberate mutation.
    path.write_bytes(original)
    for action in ('write', 'read'):
        subprocess.run([sys.argv[1], action, str(path)], check=True)
    assert path.read_bytes()[:2048 * 512] == original[:2048 * 512]
    stored = path.read_bytes()
    banks = [inspect_bank(stored[(2048 + bank * BANK) * SECTOR:(2048 + (bank + 1) * BANK) * SECTOR],
                          bank, 2048, MIN_SECTORS) for bank in range(2)]
    assert all(bank['valid'] for bank in banks)
    assert max(banks, key=lambda bank: bank['generation'])['files'] == [
        {'path': '/ram/empty', 'size': 0, 'type': 'directory'},
        {'path': '/ram/docs', 'size': 0, 'type': 'directory'},
        {'path': '/ram/docs/note', 'size': 6, 'type': 'file'},
        {'path': '/ram/reboot', 'size': 21, 'type': 'file'}]
    expected_files = max(banks, key=lambda bank: bank['generation'])['files']
    migrated = pathlib.Path(directory) / 'v3.img'
    migrate(path, migrated, 0, None)
    assert path.read_bytes() == stored
    subprocess.run([sys.argv[1], 'read', str(migrated)], check=True)
    with migrated.open('rb') as f:
        banks, selected = inspect_volume(f, 2048, 32768)
    assert all(bank['valid'] for bank in banks) and selected['version'] == 3
    assert selected['files'] == expected_files
    fresh = pathlib.Path(directory) / 'fresh-v3.img'
    fresh.write_bytes(new_image())
    for action in ('write', 'read'):
        subprocess.run([sys.argv[1], action, str(fresh)], check=True)
print('PASS: files survive writer process exit and fresh reader mount')
