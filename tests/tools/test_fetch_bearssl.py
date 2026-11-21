"""Offline tests for installing the pinned BearSSL tree."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import fetch_bearssl  # noqa: E402

SOURCES = ('inc/bearssl.h', 'src/inner.h', 'src/ssl/ssl_engine.c', 'LICENSE.txt')


def snapshot(*names):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w:gz') as archive:
        for name in names:
            entry = tarfile.TarInfo(f'BearSSL-{fetch_bearssl.REVISION[:7]}/{name}')
            data = name.encode('ascii')
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))
    return stream.getvalue()


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.cache = Path(self.directory.name) / 'cache'
        self.data = snapshot(*SOURCES, 'tools/upstream-tool.c')
        patch = mock.patch.object(fetch_bearssl, 'SHA256', hashlib.sha256(self.data).hexdigest())
        patch.start()
        self.addCleanup(patch.stop)

    def test_verified_download_is_reused_offline(self):
        fetch = mock.Mock(return_value=self.data)
        source = fetch_bearssl.prepare(cache_dir=self.cache, fetch=fetch)
        self.assertEqual(source.name, f'bearssl-{fetch_bearssl.REVISION}')
        self.assertEqual((source / 'inc/bearssl.h').read_bytes(), b'inc/bearssl.h')
        self.assertEqual((source / 'LICENSE.txt').read_bytes(), b'LICENSE.txt')
        self.assertFalse((source / 'tools').exists())
        self.assertEqual(fetch_bearssl.prepare(cache_dir=self.cache, fetch=fetch), source)
        fetch.assert_called_once_with(fetch_bearssl.URL)

    def test_unverified_archive_is_rejected(self):
        with self.assertRaisesRegex(fetch_bearssl.SourceError, 'SHA-256'):
            fetch_bearssl.prepare(cache_dir=self.cache, fetch=lambda _: self.data + b'tampered')
        self.assertFalse(self.cache.exists())

    def test_failed_extraction_leaves_no_cached_tree(self):
        data = snapshot(*SOURCES, 'src/../../escaped')
        with mock.patch.object(fetch_bearssl, 'SHA256', hashlib.sha256(data).hexdigest()):
            with self.assertRaisesRegex(fetch_bearssl.SourceError, 'unsafe'):
                fetch_bearssl.prepare(cache_dir=self.cache, fetch=lambda _: data)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_concurrent_installers_share_the_complete_tree(self):
        ready = threading.Barrier(2, timeout=10)

        def fetch(_):
            ready.wait()
            return self.data

        with ThreadPoolExecutor(max_workers=2) as pool:
            first, second = pool.map(lambda _: fetch_bearssl.prepare(cache_dir=self.cache, fetch=fetch), range(2))
        self.assertEqual(first, second)
        self.assertEqual(list(self.cache.iterdir()), [first])

    def test_explicit_source_needs_no_cache_or_download(self):
        source = Path(self.directory.name) / 'local source'
        fetch = mock.Mock(side_effect=AssertionError('unexpected download'))
        with self.assertRaisesRegex(fetch_bearssl.SourceError, 'inc/bearssl.h'):
            fetch_bearssl.prepare(source, self.cache, fetch)
        for name in SOURCES:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        self.assertEqual(fetch_bearssl.prepare(source, self.cache, fetch), source.resolve())
        self.assertFalse(self.cache.exists())
        fetch.assert_not_called()


if __name__ == '__main__':
    unittest.main()
