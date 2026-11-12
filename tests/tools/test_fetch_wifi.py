import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('vantage_fetch_wifi', ROOT / 'tools/fetch_wifi.py')
fetch_wifi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch_wifi)

COPYRIGHT = (b'Format: test\n\nFiles: debian/added-firmware/*/*43455*\nLicense: binary-redist-Cypress\n\n'
             b'License: binary-redist-Cypress\n SYNTHETIC LICENSE\n .\n Second paragraph.\n\n'
             b'License: other\n Not this one.\n')
DATA = {'copyright': COPYRIGHT, 'firmware.bin': b'\x01\x02\x03\x04' * 8,
        'nvram.txt': b'boardtype=0x1\n', 'clm.bin': b'synthetic-clm'}


class FakeServer:
    def __init__(self):
        self.files = {fetch_wifi.SOURCE + remote: DATA[name] for name, (remote, _) in self.pinned().items()}
        self.requests = []

    @staticmethod
    def pinned():
        return {name: (remote, fetch_wifi.sha256(DATA[name])) for name, (remote, _) in fetch_wifi.FILES.items()}

    def __call__(self, url):
        self.requests.append(url)
        return self.files[url]


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name) / 'cache'
        self.server = FakeServer()
        original = fetch_wifi.FILES
        fetch_wifi.FILES = self.server.pinned()
        self.addCleanup(setattr, fetch_wifi, 'FILES', original)

    def prepare(self, country='TH', **options):
        with contextlib.redirect_stdout(io.StringIO()):   # The prompt prints the license.
            return fetch_wifi.prepare(country, directory=self.dir, fetch=self.server, **options)

    def test_license_must_be_accepted_before_firmware_is_downloaded(self):
        with self.assertRaises(fetch_wifi.FirmwareError):
            self.prepare()
        self.assertEqual(len(self.server.requests), 1)   # Only the license document.
        self.assertFalse((self.dir / 'firmware.bin').exists())
        self.assertEqual((self.dir / 'LICENSE-cypress.txt').read_text(), 'SYNTHETIC LICENSE\n\nSecond paragraph.\n')
        with self.assertRaises(fetch_wifi.FirmwareError):
            self.prepare(ask=lambda prompt: 'n')
        self.assertFalse((self.dir / 'license-accepted').exists())

    def test_accepted_download_is_verified_embedded_and_cached(self):
        output = self.prepare(country='th', ask=lambda prompt: 'y')
        self.assertEqual(output, self.dir / 'wifi-assets-TH.c')
        manifest = json.loads(output.with_suffix('.json').read_text())
        self.assertEqual((manifest['country'], manifest['regulatory_revision']), ('TH', -1))
        self.assertIn('"TH", -1', output.read_text())
        before, requests = output.stat().st_mtime_ns, len(self.server.requests)
        self.assertEqual(self.prepare(), output)          # Acceptance is remembered.
        self.assertEqual(len(self.server.requests), requests)
        self.assertEqual(output.stat().st_mtime_ns, before)
        self.prepare(country='GB', regrev=3)
        self.assertEqual(json.loads((self.dir / 'wifi-assets-GB.json').read_text())['regulatory_revision'], 3)

    def test_altered_files_are_replaced_and_mismatches_rejected(self):
        self.prepare(accepted=True)
        (self.dir / 'clm.bin').write_bytes(b'tampered')
        self.prepare()
        self.assertEqual((self.dir / 'clm.bin').read_bytes(), DATA['clm.bin'])
        (self.dir / 'clm.bin').unlink()
        self.server.files[fetch_wifi.SOURCE + fetch_wifi.FILES['clm.bin'][0]] = b'wrong'
        with self.assertRaises(fetch_wifi.FirmwareError):
            self.prepare()
        self.assertFalse((self.dir / 'clm.bin').exists())

    def test_changed_license_needs_new_acceptance(self):
        self.prepare(accepted=True)
        changed = COPYRIGHT.replace(b'SYNTHETIC', b'REVISED')
        fetch_wifi.FILES['copyright'] = (fetch_wifi.FILES['copyright'][0], fetch_wifi.sha256(changed))
        self.server.files[fetch_wifi.SOURCE + fetch_wifi.FILES['copyright'][0]] = changed
        with self.assertRaises(fetch_wifi.FirmwareError):
            self.prepare()

    def test_invalid_regulatory_settings(self):
        for country, regrev in (('T', -1), ('THA', -1), ('1A', -1), ('TH', -2)):
            with self.assertRaises(fetch_wifi.FirmwareError):
                self.prepare(country=country, regrev=regrev, accepted=True)
        self.assertEqual(self.server.requests, [])

    def test_download_failure_is_reported(self):
        def offline(url):
            raise OSError('network unreachable')
        with self.assertRaisesRegex(fetch_wifi.FirmwareError, 'network unreachable'):
            fetch_wifi.prepare('TH', directory=self.dir, fetch=offline, accepted=True)


class CountryTests(unittest.TestCase):
    TRACE, IPINFO = (url for url, _ in fetch_wifi.LOOKUPS)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.answers = {}
        self.requests = []

    def fetch(self, url):
        self.requests.append(url)
        answer = self.answers.get(url)
        if answer is None:
            raise OSError('offline')
        return answer

    def test_detection_falls_back_and_rejects_unknown_locations(self):
        self.answers[self.TRACE] = b'fl=1\nip=192.0.2.1\nloc=KH\ntls=TLSv1.3\n'
        self.assertEqual(fetch_wifi.detect_country(self.fetch), 'KH')
        self.answers[self.TRACE] = b'ip=192.0.2.1\n'
        self.answers[self.IPINFO] = b'th\n'
        self.assertEqual(fetch_wifi.detect_country(self.fetch), 'TH')
        for unknown in (b'loc=XX\n', b'loc=T1\n'):
            self.answers = {self.TRACE: unknown, self.IPINFO: b'ZZ'}
            with self.assertRaises(fetch_wifi.FirmwareError):
                fetch_wifi.detect_country(self.fetch)
        self.answers = {}
        with self.assertRaises(fetch_wifi.FirmwareError):
            fetch_wifi.detect_country(self.fetch)

    def test_detected_country_is_remembered(self):
        self.answers[self.TRACE] = b'loc=KH\n'
        self.assertEqual(fetch_wifi.resolve_country(None, self.dir, self.fetch)[0], 'KH')
        self.assertEqual((self.dir / 'country').read_text(), 'KH\n')
        self.answers[self.TRACE] = b'loc=GB\n'
        requests = len(self.requests)
        self.assertEqual(fetch_wifi.resolve_country(None, self.dir, self.fetch)[0], 'KH')
        self.assertEqual(len(self.requests), requests)
        self.assertEqual(fetch_wifi.resolve_country('auto', self.dir, self.fetch)[0], 'GB')
        self.assertEqual((self.dir / 'country').read_text(), 'GB\n')

    def test_given_country_is_not_looked_up(self):
        self.assertEqual(fetch_wifi.resolve_country('th', self.dir, self.fetch), ('TH', 'given'))
        self.assertEqual(self.requests, [])
        self.assertFalse((self.dir / 'country').exists())


class BuildOptionTests(unittest.TestCase):
    def wifi_assets(self, fake, fail):
        spec = importlib.util.spec_from_file_location('vantage_build', ROOT / 'tools/build.py')
        build = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(build)
        with mock.patch.dict(sys.modules, {'fetch_wifi': fake}), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return build.wifi_assets(None, -1, fail)

    def test_automatic_firmware_accepts_the_license_and_tolerates_failure(self):
        calls = []
        fake = types.SimpleNamespace(
            FirmwareError=fetch_wifi.FirmwareError,
            resolve_country=lambda country: ('KH', 'detected from the IP address'),
            prepare=lambda *args, **options: calls.append((args, options)) or Path('wifi-assets-KH.c'))
        self.assertEqual(self.wifi_assets(fake, None), Path('wifi-assets-KH.c'))
        self.assertEqual(calls, [(('KH', -1), {'accepted': True})])

        def offline(country):
            raise fetch_wifi.FirmwareError('offline')
        fake.resolve_country = offline
        self.assertIsNone(self.wifi_assets(fake, None))

        def fail(message):
            raise SystemExit(message)
        with self.assertRaisesRegex(SystemExit, 'offline'):
            self.wifi_assets(fake, fail)

    def test_country_conflicts_are_rejected(self):
        for args in (['--wifi-country', 'TH', '--wifi-firmware-c', 'x.c'], ['--wifi', '0', '--wifi-country', 'TH'],
                     ['sim', '--wifi-country', 'TH'], ['--wifi', '0', '--wifi-regrev', '0'],
                     ['test', '--wifi-regrev', '0']):
            result = subprocess.run([sys.executable, str(ROOT / 'tools/build.py'), *args],
                                    capture_output=True, text=True,
                                    env={k: v for k, v in os.environ.items() if k != 'VANTAGE_WIFI_COUNTRY'})
            self.assertNotEqual(result.returncode, 0, args)
            self.assertIn('error:', result.stderr)


if __name__ == '__main__':
    unittest.main()
