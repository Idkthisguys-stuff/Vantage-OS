"""Download the Pi 4 Model B CYW43455 Wi-Fi firmware and embed it for a build.

The firmware, board NVRAM and CLM come from Raspberry Pi's firmware-nonfree
repository at a pinned commit and must match pinned SHA-256 hashes. They are
covered by the Cypress binary redistribution license, which must be accepted
once per cache directory. Without --country, the country is looked up once from
this network's public IP address and remembered. Files stay in the cache (out/
by default) and must not be committed.

Usage: python tools/fetch_wifi.py [--country TH] [--accept-license]
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIR = ROOT / 'out' / 'wifi-firmware'
COMMIT = '5d2295b633d94641253e16ba90fc847436fc9d4a'
SOURCE = f'https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/{COMMIT}/debian/'
# Raspberry Pi OS uses these for the Pi 4 Model B: the board-specific names in
# added-firmware/brcm are links to them.
FILES = {
    'copyright': ('copyright', '07082fd0bb65e32c73af39a2292dd6c83f169be7427d338ba1fe34f9b86c0e30'),
    'firmware.bin': ('added-firmware/cypress/cyfmac43455-sdio-standard.bin',
                     'd608f866582519c0a28d86db43040f4f1b98dd1d153e72e9752586546b4a36c3'),
    'nvram.txt': ('added-firmware/brcm/brcmfmac43455-sdio.txt',
                  'ca709be81a78bdb6932936374f39943acbd7af07fae6151011127599a3ce9e3d'),
    'clm.bin': ('added-firmware/cypress/cyfmac43455-sdio.clm_blob',
                '9823842cae9fb9a5dd1e5fb31f595516ec7deee341354bef30bb3026eee29cc1'),
}
LICENSE = 'binary-redist-Cypress'
LICENSE_FILE = 'LICENSE-cypress.txt'
ACCEPTED_FILE = 'license-accepted'
COUNTRY_FILE = 'country'
# Each service maps the caller's public IP address to an ISO 3166 country code.
LOOKUPS = (
    ('https://www.cloudflare.com/cdn-cgi/trace',
     lambda text: next((line[4:] for line in text.splitlines() if line.startswith('loc=')), '')),
    ('https://ipinfo.io/country', lambda text: text),
)


class FirmwareError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def download(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'vantage-os-build'})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def country_code(code):
    """The code in upper case, or None unless it is two letters naming a country."""
    code = code.strip().upper()
    # XX and ZZ mean unknown; Tor exit nodes report T1, which fails the letter test.
    ok = len(code) == 2 and all('A' <= c <= 'Z' for c in code) and code not in ('XX', 'ZZ')
    return code if ok else None


def detect_country(fetch=download):
    for url, parse in LOOKUPS:
        try:
            code = country_code(parse(fetch(url).decode('ascii', 'replace')))
        except (OSError, ValueError):
            continue
        if code:
            return code
    raise FirmwareError('Could not detect the country from the IP address; pass --wifi-country XX.')


def resolve_country(country=None, directory=DEFAULT_DIR, fetch=download):
    """Return (code, origin). None reuses the remembered country or detects it,
    'auto' detects it again, and anything else is used as given."""
    if country and country.lower() != 'auto':
        return country.upper(), 'given'
    record = Path(directory) / COUNTRY_FILE
    if country is None and record.is_file():
        code = country_code(record.read_text(encoding='utf-8'))
        if code:
            return code, 'detected earlier from the IP address'
    code = detect_country(fetch)
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(code + '\n', encoding='utf-8')
    return code, 'detected from the IP address'


def obtain(directory, name, fetch):
    """Return the cached file, downloading it if it is missing or altered."""
    remote, digest = FILES[name]
    path = directory / name
    if path.is_file() and sha256(path.read_bytes()) == digest:
        return path
    print(f'Downloading {remote.rsplit("/", 1)[-1]}', flush=True)
    try:
        data = fetch(SOURCE + remote)
    except OSError as error:
        raise FirmwareError(f'Downloading {remote} failed: {error}') from error
    if sha256(data) != digest:
        raise FirmwareError(f'{remote} does not match its pinned SHA-256; refusing to use it')
    partial = path.with_name(path.name + '.part')
    partial.write_bytes(data)
    os.replace(partial, path)
    return path


def license_text(copyright_data):
    """The license paragraph from a Debian machine-readable copyright file."""
    text = copyright_data.decode('utf-8')
    start = text.find(f'\nLicense: {LICENSE}\n ')
    if start < 0:
        raise FirmwareError(f'The {LICENSE} license text is missing from the copyright file')
    end = text.find('\n\n', start + 1)
    lines = text[start + 1:end if end >= 0 else len(text)].split('\n')[1:]
    return '\n'.join('' if line == ' .' else line[1:] for line in lines) + '\n'


def accept(directory, text, accepted, ask):
    record = directory / ACCEPTED_FILE
    digest = sha256(text.encode('utf-8'))
    if record.is_file() and record.read_text(encoding='utf-8').strip() == digest:
        return
    if not accepted:
        if ask is None:
            raise FirmwareError(f'Read {directory / LICENSE_FILE}, then accept the Cypress firmware '
                                'license with --accept-license.')
        print(text)
        answer = ask('Do you accept the Cypress license above for the Wi-Fi firmware? [y/N] ')
        if answer.strip().lower() not in ('y', 'yes'):
            raise FirmwareError('The Wi-Fi firmware license was not accepted; nothing was downloaded.')
    else:
        print(f'Accepting the Cypress license for the Wi-Fi firmware: {directory / LICENSE_FILE}', flush=True)
    record.write_text(digest + '\n', encoding='utf-8')


def current(output, country, regrev, inputs):
    """True when output was generated from exactly these inputs and settings."""
    manifest = output.with_suffix('.json')
    if not output.is_file() or not manifest.is_file():
        return False
    try:
        data = json.loads(manifest.read_text(encoding='utf-8'))
    except (ValueError, UnicodeError):
        return False
    return (isinstance(data, dict) and data.get('country') == country and
            data.get('regulatory_revision') == regrev and
            all(isinstance(data.get(key), dict) and data[key].get('sha256') == FILES[name][1]
                for key, name in inputs))


def prepare(country, regrev=-1, directory=DEFAULT_DIR, accepted=False, fetch=download, ask=None):
    """Download, verify and embed the firmware; returns the generated C file."""
    country = country.upper()
    if len(country) != 2 or not all('A' <= c <= 'Z' for c in country):
        raise FirmwareError('Use the two-letter country code where the Pi is used, e.g. TH or GB.')
    if not -1 <= regrev <= 2147483647:
        raise FirmwareError('The regulatory revision must be -1 (firmware default) or higher.')
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    text = license_text(obtain(directory, 'copyright', fetch).read_bytes())
    (directory / LICENSE_FILE).write_text(text, encoding='utf-8')
    accept(directory, text, accepted, ask)
    inputs = (('firmware', 'firmware.bin'), ('nvram', 'nvram.txt'), ('clm', 'clm.bin'))
    paths = {key: obtain(directory, name, fetch) for key, name in inputs}
    output = directory / f'wifi-assets-{country}.c'
    if current(output, country, regrev, inputs):
        return output
    command = [sys.executable, str(ROOT / 'tools/embed_wifi.py'),
               *[arg for key in ('firmware', 'nvram', 'clm') for arg in ('--' + key, str(paths[key]))],
               '--country', country, '--regrev', str(regrev), '--output', str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise FirmwareError('Embedding the firmware failed: ' + (result.stderr or result.stdout).strip())
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--country', help='two-letter country where the Pi is used, or auto to look '
                                          'it up again (default: remembered or looked up from the IP address)')
    parser.add_argument('--regrev', type=int, default=-1, help='regulatory revision (default: firmware default)')
    parser.add_argument('--dir', default=str(DEFAULT_DIR), help='download cache (default: out/wifi-firmware)')
    parser.add_argument('--accept-license', action='store_true',
                        help='accept the Cypress firmware license without a prompt')
    args = parser.parse_args(argv)
    try:
        country, origin = resolve_country(args.country, args.dir)
        print(f'Wi-Fi country: {country} ({origin})', flush=True)
        output = prepare(country, args.regrev, args.dir, args.accept_license,
                         ask=input if sys.stdin.isatty() else None)
    except FirmwareError as error:
        parser.error(str(error))
    print(f'Wi-Fi firmware ready: {output}')
    print(f'Build with: python tools/build.py --wifi-firmware-c "{output}"')


if __name__ == '__main__':
    main()
