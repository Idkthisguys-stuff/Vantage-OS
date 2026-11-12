"""CA bundle download, verification, conversion and build integration, offline.

The certificates are synthetic: DER structures with arbitrary public key bytes
and dummy signatures, built at test time. No key pairs exist for them."""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import fetch_ca  # noqa: E402

spec = importlib.util.spec_from_file_location('vantage_build', ROOT / 'tools/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

CN = bytes.fromhex('550403')
P256 = bytes.fromhex('2a8648ce3d030107')
P384 = bytes.fromhex('2b81040022')
P521 = bytes.fromhex('2b81040023')
BRAINPOOL = bytes.fromhex('2b2403030208010107')
ED25519 = bytes.fromhex('2b6570')


def der(tag, content):
    n = len(content)
    if n < 128:
        head = bytes([n])
    elif n < 256:
        head = bytes([0x81, n])
    else:
        head = bytes([0x82, n >> 8, n & 255])
    return bytes([tag]) + head + content


def name(common_name):
    return der(0x30, der(0x31, der(0x30, der(0x06, CN) + der(0x0c, common_name.encode()))))


def rsa_key(modulus, exponent=b'\x01\x00\x01'):
    numbers = der(0x30, der(0x02, b'\x00' + modulus) + der(0x02, exponent))
    return der(0x30, der(0x30, der(0x06, fetch_ca.RSA_OID) + b'\x05\x00') + der(0x03, b'\x00' + numbers))


def ec_key(curve, point):
    return der(0x30, der(0x30, der(0x06, fetch_ca.EC_OID) + der(0x06, curve)) + der(0x03, b'\x00' + point))


def other_key(oid, key):
    return der(0x30, der(0x30, der(0x06, oid)) + der(0x03, b'\x00' + key))


def certificate(subject, key):
    algorithm = der(0x30, der(0x06, bytes.fromhex('2a864886f70d01010b')) + b'\x05\x00')
    validity = der(0x30, der(0x17, b'250101000000Z') + der(0x17, b'350101000000Z'))
    tbs = der(0x30, der(0xa0, der(0x02, b'\x02')) + der(0x02, b'\x01') + algorithm + subject + validity +
              subject + key)
    return der(0x30, tbs + algorithm + der(0x03, b'\x00' + bytes(64)))


def pem(*ders):
    blocks = []
    for data in ders:
        text = base64.b64encode(data).decode()
        lines = [text[i:i + 64] for i in range(0, len(text), 64)]
        blocks.append('Synthetic\n=========\n-----BEGIN CERTIFICATE-----\n' + '\n'.join(lines) +
                      '\n-----END CERTIFICATE-----\n')
    return '\n'.join(blocks)


def arrays(source):
    return {m.group(1): bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(2)))
            for m in re.finditer(r'static const unsigned char (\w+)\[\] = \{(.*?)\};', source, re.S)}


MODULUS = bytes([0xc5]) + bytes(range(255))
P256_POINT = b'\x04' + bytes(range(1, 65))
P384_POINT = b'\x04' + bytes(range(2, 98))
P521_POINT = b'\x04' + bytes(i & 255 for i in range(3, 135))
SUPPORTED = [(name('RSA Root'), rsa_key(MODULUS)), (name('P-256 Root'), ec_key(P256, P256_POINT)),
             (name('P-384 Root'), ec_key(P384, P384_POINT)), (name('P-521 Root'), ec_key(P521, P521_POINT))]
UNSUPPORTED = [(name('Ed25519 Root'), other_key(ED25519, bytes(32))),
               (name('Brainpool Root'), ec_key(BRAINPOOL, P256_POINT)),
               (name('Huge RSA Root'), rsa_key(b'\xff' * 520)),
               (name('Compressed Root'), ec_key(P256, b'\x02' + bytes(32)))]
BUNDLE = pem(*[certificate(s, k) for s, k in SUPPORTED + UNSUPPORTED]).encode()


class ConversionTests(unittest.TestCase):
    def test_keys_and_names_are_converted_exactly(self):
        source, count, skipped = fetch_ca.convert(BUNDLE.decode(), 'synthetic.pem')
        self.assertEqual(count, 4)
        self.assertEqual(len(skipped), 4)
        for reason, label in zip(skipped, ('Ed25519', 'Brainpool', 'Huge RSA', 'Compressed')):
            self.assertIn(label + ' Root', reason)
        data = arrays(source)
        for i, (subject, _) in enumerate(SUPPORTED):
            self.assertEqual(data[f'ta{i}_dn'], subject)
        self.assertEqual(data['ta0_n'], MODULUS)
        self.assertEqual(data['ta0_e'], b'\x01\x00\x01')
        self.assertEqual(data['ta1_q'], P256_POINT)
        self.assertEqual(data['ta2_q'], P384_POINT)
        self.assertEqual(data['ta3_q'], P521_POINT)
        for curve in ('BR_EC_secp256r1', 'BR_EC_secp384r1', 'BR_EC_secp521r1'):
            self.assertIn(curve, source)
        self.assertEqual(source.count('BR_X509_TA_CA'), 4)
        self.assertIn('/* P-384 Root */', source)
        self.assertIn('8 certificates, 4 anchors, 4 skipped', source)
        self.assertTrue(source.endswith('sizeof(tls_trust_anchors[0]);\n'))

    def test_comment_names_cannot_end_the_comment(self):
        source, _, _ = fetch_ca.convert(pem(certificate(name('Evil */ Root\x01'), rsa_key(MODULUS))), 'x.pem')
        self.assertIn('/* Evil *? Root? */', source)

    def test_invalid_bundles_are_rejected(self):
        cases = ['no certificates here', pem(certificate(*UNSUPPORTED[0])),
                 '-----BEGIN CERTIFICATE-----\n!!!\n-----END CERTIFICATE-----\n',
                 pem(certificate(*SUPPORTED[0])[:40]), pem(der(0x30, der(0x30, b'')))]
        for text in cases:
            with self.subTest(text=text[:30]), self.assertRaises(fetch_ca.BundleError):
                fetch_ca.convert(text, 'bad.pem')

    def test_generated_source_compiles_against_the_trust_header(self):
        compiler = next((shutil.which(c) for c in ('aarch64-linux-gnu-gcc', 'aarch64-none-elf-gcc')
                         if shutil.which(c)), None)
        if not compiler:
            self.skipTest('no AArch64 GNU compiler on PATH')
        source, _, _ = fetch_ca.convert(BUNDLE.decode(), 'synthetic.pem')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trust.c'
            path.write_text('#include "tls_trust.h"\n' + source, encoding='utf-8')
            result = subprocess.run([compiler, '-fsyntax-only', '-ffreestanding', '-nostdinc', '-Wall', '-Wextra',
                                     '-Werror', '-I', str(ROOT / 'include'), '-I', str(ROOT / 'user/lib'),
                                     '-I', str(ROOT / 'third_party/bearssl/inc'),
                                     '-isystem', str(ROOT / 'user/lib/libc'), str(path)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name)
        self.calls = []
        patch = mock.patch.object(fetch_ca, 'SHA256', hashlib.sha256(BUNDLE).hexdigest())
        patch.start()
        self.addCleanup(patch.stop)

    def tearDown(self):
        self.directory.cleanup()

    def fetch(self, data):
        def run(url):
            self.calls.append(url)
            if isinstance(data, Exception):
                raise data
            return data
        return run

    def test_download_is_verified_and_cached(self):
        with mock.patch('builtins.print'):
            path = fetch_ca.obtain(self.path, self.fetch(BUNDLE))
            self.assertEqual(path.read_bytes(), BUNDLE)
            self.assertEqual(self.calls, [fetch_ca.SOURCE + fetch_ca.BUNDLE])
            fetch_ca.obtain(self.path, self.fetch(BUNDLE))
            self.assertEqual(len(self.calls), 1)
            path.write_bytes(BUNDLE + b'tampered')
            fetch_ca.obtain(self.path, self.fetch(BUNDLE))
            self.assertEqual(len(self.calls), 2)
            self.assertEqual(path.read_bytes(), BUNDLE)

    def test_mismatched_or_failed_downloads_are_refused(self):
        with mock.patch('builtins.print'):
            with self.assertRaisesRegex(fetch_ca.BundleError, 'pinned SHA-256'):
                fetch_ca.obtain(self.path, self.fetch(BUNDLE + b'x'))
            with self.assertRaisesRegex(fetch_ca.BundleError, 'failed'):
                fetch_ca.obtain(self.path, self.fetch(OSError('offline')))
        self.assertFalse((self.path / fetch_ca.BUNDLE).exists())

    def test_prepare_regenerates_only_when_inputs_change(self):
        with mock.patch('builtins.print'):
            output = fetch_ca.prepare(self.path, self.fetch(BUNDLE))
            manifest = json.loads(output.with_suffix('.json').read_text(encoding='utf-8'))
            self.assertEqual((manifest['anchors'], len(manifest['skipped'])), (4, 4))
            self.assertNotIn(b'\r\n', output.read_bytes())
            os.utime(output, ns=(1_000_000_000, 1_000_000_000))
            fetch_ca.prepare(self.path, self.fetch(OSError('offline')))
            self.assertEqual(output.stat().st_mtime_ns, 1_000_000_000)
            with mock.patch.object(fetch_ca, 'generator', return_value='changed'):
                fetch_ca.prepare(self.path, self.fetch(OSError('offline')))
            self.assertNotEqual(output.stat().st_mtime_ns, 1_000_000_000)
            self.assertEqual(len(self.calls), 1)

    def test_local_bundle_conversion(self):
        source = self.path / 'mine.pem'
        source.write_bytes(BUNDLE)
        output = self.path / 'mine.c'
        with mock.patch('builtins.print'):
            fetch_ca.main(['--pem', str(source), '--output', str(output)])
        self.assertIn('4 anchors', output.read_text(encoding='utf-8'))


class BuildIntegrationTests(unittest.TestCase):
    def test_offline_bundle_failure_builds_without_anchors(self):
        with mock.patch.object(fetch_ca, 'prepare', side_effect=fetch_ca.BundleError('offline')), \
                mock.patch('sys.stderr') as stderr:
            self.assertIsNone(build.tls_trust())
        self.assertIn('TLS trust anchors unavailable: offline',
                      ''.join(call.args[0] for call in stderr.write.call_args_list))

    def test_option_is_kernel_only(self):
        self.assertIsNone(build.parser().parse_args([]).tls_trust_c)
        result = subprocess.run([sys.executable, str(ROOT / 'tools/build.py'), 'test', '--tls-trust-c', 'x.c'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('only valid for kernel builds', result.stderr)


if __name__ == '__main__':
    unittest.main()
