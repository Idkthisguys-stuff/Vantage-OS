import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import storage_image as storage
import storage_v3 as v3
from test_storage_image import fixture_bank


class CapacityTests(unittest.TestCase):
    def test_256_entries_and_one_mib_file(self):
        for nodes in ([(f'd{i}'.encode(), 2, 0, b'') for i in range(256)],
                      [(b'big', 1, 0, b'z' * v3.CAPACITY)]):
            image = storage.new_image(3, v3.MIN_SECTORS, nodes)
            banks, selected = storage.inspect_volume(io.BytesIO(image), 2048, v3.MIN_SECTORS)
            self.assertTrue(all(bank['valid'] for bank in banks))
            self.assertEqual(len(selected['files']), len(nodes))
        with self.assertRaises(ValueError):
            v3.encode_bank(0, 1, 2048, v3.MIN_SECTORS, [(b'big', 1, 0, b'x' * (v3.CAPACITY + 1))])
        with self.assertRaises(ValueError):
            v3.encode_bank(0, 1, 2048, v3.MIN_SECTORS,
                           [(b'big', 1, 0, b'x' * v3.CAPACITY), (b'extra', 1, 0, b'x')])

    def test_paths_and_checksum_valid_semantic_corruption(self):
        nodes = [(bytes([97 + i]) * 31, 2, i, b'') for i in range(7)]
        nodes.append((b'z' * 26, 1, 7, b'content'))
        bank = v3.encode_bank(0, 1, 2048, v3.MIN_SECTORS, nodes)
        image = bytearray((2048 + v3.MIN_SECTORS) * 512)
        image[2048 * 512:2048 * 512 + len(bank)] = bank
        self.assertEqual(len(v3.inspect_bank(io.BytesIO(image), 0, 2048, v3.MIN_SECTORS)['files'][-1]['path']), 255)
        with self.assertRaises(ValueError):
            v3.encode_bank(0, 1, 2048, v3.MIN_SECTORS, nodes[:-1] + [(b'z' * 27, 1, 7, b'')])
        for kind in ('parent', 'cycle', 'extent', 'size', 'padding', 'tail', 'duplicate', 'unused'):
            corrupt = bytearray(bank)
            if kind == 'parent': struct.pack_into('<I', corrupt, 512 + 40, 257)
            if kind == 'cycle': struct.pack_into('<I', corrupt, 512 + 40, 1)
            if kind == 'extent': struct.pack_into('<I', corrupt, 512 + 7 * 64 + 44, v3.META + 1)
            if kind == 'size': struct.pack_into('<I', corrupt, 512 + 7 * 64 + 36, v3.CAPACITY + 1)
            if kind == 'padding': corrupt[512 + 48] = 1
            if kind == 'tail': corrupt[-1] = 1
            if kind == 'duplicate': corrupt[512 + 8 * 64:512 + 9 * 64] = corrupt[512:512 + 64]
            if kind == 'unused': struct.pack_into('<I', corrupt, 512 + 9 * 64 + 44, 1)
            struct.pack_into('<I', corrupt, 24, zlib.crc32(corrupt[512:]))
            struct.pack_into('<I', corrupt, 508, zlib.crc32(corrupt[:508]))
            damaged = bytearray(image)
            damaged[2048 * 512:2048 * 512 + len(corrupt)] = corrupt
            with self.subTest(kind=kind):
                self.assertFalse(v3.inspect_bank(io.BytesIO(damaged), 0, 2048, v3.MIN_SECTORS)['valid'])

    def test_explicit_migration_keeps_source_and_requires_new_output(self):
        for version in (1, 2):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                source, output = Path(directory) / 'old.img', Path(directory) / 'new.img'
                nodes = [(b'empty', 2, 0, b''), (b'note', 1, 1, b'hello')] if version == 2 else [(b'note', 1, 0, b'hello')]
                original = storage.new_image(version=version)
                index = 0
                if version == 2:
                    # Full-card-style source: boot partition 0, selected data 1.
                    original[462:478] = original[446:462]
                    original[446:462] = bytes(16)
                    original[450] = 0x0c
                    struct.pack_into('<II', original, 454, 8, 16)
                    original[8 * 512:24 * 512] = b'B' * (16 * 512)
                    index = 1
                bank = fixture_bank(nodes, version)
                struct.pack_into('<Q', bank, 16, 3)
                struct.pack_into('<I', bank, 508, zlib.crc32(bank[:508]))
                original[2048 * 512:(2048 + storage.BANK) * 512] = bank
                source.write_bytes(original)
                storage.migrate(source, output, index, v3.MIN_SECTORS)
                self.assertEqual(source.read_bytes(), original)
                banks, selected = storage.inspect_volume(io.BytesIO(output.read_bytes()), 2048, v3.MIN_SECTORS)
                self.assertTrue(all(bank['valid'] for bank in banks))
                self.assertEqual(selected['files'][-1]['size'], 5)
                self.assertEqual(selected['files'][-1]['path'], '/ram/empty/note' if version == 2 else '/ram/note')
                start = (2048 + selected['bank'] * (v3.MIN_SECTORS // 2) + 1 + v3.META) * 512
                self.assertEqual(output.read_bytes()[start:start + 5], b'hello')
                with self.assertRaises(FileExistsError): storage.migrate(source, source, index, None)
                with self.assertRaises(FileExistsError): storage.migrate(source, output, index, None)
                with self.assertRaises(ValueError): storage.migrate(source, Path(directory) / 'small.img', index, 290)
                self.assertFalse((Path(directory) / 'small.img').exists())
                self.assertEqual(source.read_bytes(), original)

    def test_cli_defaults_and_both_bank_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'data.img'
            command = [sys.executable, '-B', str(ROOT / 'tools/storage_image.py')]
            subprocess.run(command + ['create', '--image', str(image)], check=True, capture_output=True)
            original = image.read_bytes()
            banks, selected = storage.inspect_volume(io.BytesIO(original), 2048, v3.DEFAULT_SECTORS)
            self.assertEqual(selected['version'], 3)
            for bank in (0, 1):
                damaged = bytearray(original)
                damaged[(2048 + bank * (v3.DEFAULT_SECTORS // 2)) * 512] ^= 1
                valid, selected = storage.inspect_volume(io.BytesIO(damaged), 2048, v3.DEFAULT_SECTORS)
                self.assertEqual(sum(b['valid'] for b in valid), 1)
            subprocess.run(command + ['format', '--image', str(image), '--confirm-format'], check=True, capture_output=True)
            self.assertEqual(image.read_bytes(), original)
            subprocess.run(command + ['inspect', '--image', str(image)], check=True, capture_output=True)
            legacy = Path(directory) / 'legacy.img'
            original = storage.new_image(version=2); legacy.write_bytes(original)
            rejected = subprocess.run(command + ['format', '--image', str(legacy), '--confirm-format'], capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(legacy.read_bytes(), original)

    def test_mixed_formats_and_recovered_sources_are_rejected(self):
        image = storage.new_image()
        offset = (2048 + storage.BANK) * 512
        old = storage.empty_bank(1, 2, 2048, v3.DEFAULT_SECTORS)
        image[offset:offset + len(old)] = old
        with self.assertRaises(ValueError):
            storage.inspect_volume(io.BytesIO(image), 2048, v3.DEFAULT_SECTORS)
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / 'old.img', Path(directory) / 'new.img'
            image = storage.new_image(version=2); image[2048 * 512] ^= 1
            source.write_bytes(image)
            with self.assertRaises(ValueError): storage.migrate(source, output, 0, None)
            self.assertEqual(source.read_bytes(), image)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
