from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from storage_image import BANK, MIN_SECTORS, SECTOR, inspect_bank, new_image, partition, empty_bank


def fixture_bank(nodes, version=2):
    data = bytearray(empty_bank(0, 1, 2048, MIN_SECTORS, version))
    for slot, (name, kind, parent, content) in enumerate(nodes):
        offset = SECTOR + slot * 9 * SECTOR
        struct.pack_into('<32sIII', data, offset, name, kind, len(content), parent)
        data[offset + SECTOR:offset + SECTOR + len(content)] = content
    struct.pack_into('<I', data, 24, zlib.crc32(data[SECTOR:]))
    struct.pack_into('<I', data, 508, zlib.crc32(data[:508]))
    return data


class ImageTests(unittest.TestCase):
    def test_old_files_and_explicit_directory_trees(self):
        for version in (1, 2):
            bank = fixture_bank([(b'legacy', 1, 0, b'old')], version)
            inspected = inspect_bank(bank, 0, 2048, MIN_SECTORS)
            self.assertTrue(inspected['valid'])
            self.assertEqual(inspected['files'], [{'path': '/ram/legacy', 'size': 3, 'type': 'file'}])
        nodes = [(b'empty', 2, 0, b''), (b'docs', 2, 0, b''), (b'note', 1, 2, b'hello')]
        self.assertTrue(inspect_bank(fixture_bank(nodes), 0, 2048, MIN_SECTORS)['valid'])
        for invalid in (
                [(b'dir', 2, 1, b'')],  # Cycle.
                [(b'dir', 2, 17, b'')],  # Bounds.
                [(b'file', 1, 0, b''), (b'child', 1, 1, b'')],
                [(b'dir', 2, 0, b'data')],
                [(b'dir', 2, 2, b''), (b'other', 2, 1, b'')],
                [(b'dir', 2, 0, b''), (b'dir', 1, 0, b'')],
                [(b'a' * 31, 2, 0, b''), (b'b' * 31, 1, 1, b'')]):
            self.assertFalse(inspect_bank(fixture_bank(invalid), 0, 2048, MIN_SECTORS)['valid'])
        self.assertFalse(inspect_bank(fixture_bank(nodes, 1), 0, 2048, MIN_SECTORS)['valid'])

    def test_format_and_inspect_regular_image_preserves_surrounding_sectors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'storage.img'
            command = [sys.executable, '-B', str(ROOT / 'tools' / 'storage_image.py')]
            subprocess.run(command + ['create', '--image', str(path), '--version', '2'], check=True, capture_output=True)
            original = path.read_bytes()
            self.assertEqual(partition(original[:512], len(original) // 512, 0), (2048, MIN_SECTORS))
            for bank in range(2):
                start = (2048 + bank * BANK) * SECTOR
                self.assertTrue(inspect_bank(original[start:start + BANK * SECTOR], bank, 2048, MIN_SECTORS)['valid'])
            self.assertNotEqual(subprocess.run(command + ['create', '--image', str(path)], capture_output=True).returncode, 0)
            self.assertNotEqual(subprocess.run(command + ['format', '--image', str(path)], capture_output=True).returncode, 0)
            self.assertEqual(path.read_bytes(), original)
            subprocess.run(command + ['format', '--image', str(path), '--confirm-format', '--version', '2'], check=True, capture_output=True)
            self.assertEqual(path.read_bytes(), original)
            subprocess.run(command + ['inspect', '--image', str(path)], check=True, capture_output=True)
            corrupt = bytearray(original)
            corrupt[2048 * SECTOR] ^= 1
            corrupt[(2048 + BANK) * SECTOR] ^= 1
            path.write_bytes(corrupt)
            self.assertNotEqual(subprocess.run(command + ['inspect', '--image', str(path)], capture_output=True).returncode, 0)
            self.assertEqual(path.read_bytes(), corrupt)

    def test_partition_rejects_overlap_gpt_bounds_and_wrong_type(self):
        for kind in ('overlap', 'gpt', 'bounds', 'wrong-type', 'zero-start'):
            image = new_image(version=2)
            if kind == 'overlap': image[462:478] = image[446:462]
            if kind == 'gpt': image[450] = 0xee
            if kind == 'bounds': struct.pack_into('<I', image, 458, 0xffffffff)
            if kind == 'wrong-type': image[450] = 0x0c
            if kind == 'zero-start': struct.pack_into('<I', image, 454, 0)
            with self.assertRaises(ValueError): partition(image[:512], len(image) // 512, 0)


if __name__ == '__main__':
    unittest.main()
