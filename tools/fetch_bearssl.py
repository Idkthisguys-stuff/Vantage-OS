"""Provision the pinned upstream BearSSL sources for kernel and host builds."""

import argparse
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import sys
import tarfile
import tempfile
from urllib.request import urlopen

PIN = Path(__file__).with_name('bearssl-pin.mk')
REVISION, SHA256 = (line.split(':=', 1)[1].strip() for line in PIN.read_text(encoding='ascii').splitlines())
URL = f'https://www.bearssl.org/gitweb/?p=BearSSL;a=snapshot;h={REVISION};sf=tgz'
CACHE = PIN.resolve().parents[1] / 'out/deps'
STAMP = '.vantage-source'


class SourceError(Exception):
    pass


def download(url):
    with urlopen(url, timeout=30) as response:
        return response.read()


def check_source(source):
    for name in ('inc/bearssl.h', 'src/inner.h', 'src/ssl/ssl_engine.c', 'LICENSE.txt'):
        if not (source / name).is_file():
            raise SourceError(f'{source} is missing {name}; use the BearSSL source tree at {REVISION}')
    return source


def unpack(data, destination):
    prefix = f'BearSSL-{REVISION[:7]}'
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            if path.parts[:1] != (prefix,):
                raise SourceError(f'unexpected BearSSL archive path: {member.name}')
            parts = path.parts[1:]
            if not parts or parts[0] not in ('inc', 'src', 'LICENSE.txt') or member.isdir():
                continue
            if not member.isfile() or any(part == '..' or '\\' in part or ':' in part for part in parts):
                raise SourceError(f'unsafe BearSSL archive entry: {member.name}')
            output = destination.joinpath(*parts)
            output.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as source, output.open('xb') as target:
                target.write(source.read())


def check_cache(source):
    stamp = source / STAMP
    if not stamp.is_file() or stamp.read_text(encoding='ascii').strip() != SHA256:
        raise SourceError(f'incomplete BearSSL cache at {source}; remove that cache directory and retry')
    return check_source(source)


def prepare(source_dir=None, cache_dir=CACHE, fetch=download):
    if source_dir:
        return check_source(Path(source_dir).expanduser().resolve())
    cache_dir = Path(cache_dir).resolve()
    source = cache_dir / f'bearssl-{REVISION}'
    if source.exists():
        return check_cache(source)

    print(f'Fetching BearSSL {REVISION}', file=sys.stderr, flush=True)
    data = fetch(URL)
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SourceError('BearSSL archive does not match the pinned SHA-256')

    cache_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.bearssl-', dir=cache_dir) as directory:
        staging = Path(directory) / 'source'
        unpack(data, staging)
        check_source(staging)
        (staging / STAMP).write_text(SHA256 + '\n', encoding='ascii')
        try:
            staging.rename(source)
        except OSError:
            # Another build may have installed the complete tree first.
            if not source.is_dir():
                raise
            check_cache(source)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', help='path to an existing BearSSL source tree (or set BEARSSL_DIR)')
    parser.add_argument('--cache-dir', type=Path, default=CACHE, help='directory for the pinned source cache')
    args = parser.parse_args()
    try:
        source = prepare(args.source_dir or os.environ.get('BEARSSL_DIR'), args.cache_dir)
    except (OSError, SourceError, tarfile.TarError) as error:
        parser.exit(1, f'BearSSL: {error}\nSet BEARSSL_DIR to a local source tree at {REVISION} for offline builds.\n')
    print(source.as_posix())


if __name__ == '__main__':
    main()
