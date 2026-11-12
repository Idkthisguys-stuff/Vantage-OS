"""Download the Mozilla CA bundle published by curl and convert it to BearSSL
trust anchors for the TLS client in user/lib/tls.c.

The bundle is pinned by date and SHA-256 and cached in out/ca-bundle/ with the
generated C file, which is rebuilt only when the bundle or this script
changes. The certificates are Mozilla's root store, licensed MPL-2.0; neither
the bundle nor the generated file is committed. RSA keys and EC keys on P-256,
P-384 and P-521 are converted; other keys are skipped and counted.

Usage: python tools/fetch_ca.py [--pem FILE --output FILE]
"""
import argparse
import base64
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIR = ROOT / 'out' / 'ca-bundle'
BUNDLE = 'cacert-2026-11-05.pem'
SOURCE = 'https://curl.se/ca/'
SHA256 = '427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285'
OUTPUT = 'tls-trust.c'

RSA_OID = bytes.fromhex('2a864886f70d010101')
EC_OID = bytes.fromhex('2a8648ce3d0201')
# Curve OID -> (BearSSL curve name, uncompressed point length).
CURVES = {
    bytes.fromhex('2a8648ce3d030107'): ('BR_EC_secp256r1', 65),
    bytes.fromhex('2b81040022'): ('BR_EC_secp384r1', 97),
    bytes.fromhex('2b81040023'): ('BR_EC_secp521r1', 133),
}
NAME_OIDS = (bytes.fromhex('550403'), bytes.fromhex('55040a'))  # commonName, organizationName
RSA_MAX_BITS = 4096  # BR_MAX_RSA_SIZE


class BundleError(Exception):
    pass


class Unsupported(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def download(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'vantage-os-build'})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def obtain(directory, fetch=download):
    """Return the cached bundle, downloading it if it is missing or altered."""
    path = Path(directory) / BUNDLE
    if path.is_file() and sha256(path.read_bytes()) == SHA256:
        return path
    print(f'Downloading {BUNDLE}', flush=True)
    try:
        data = fetch(SOURCE + BUNDLE)
    except OSError as error:
        raise BundleError(f'Downloading {SOURCE + BUNDLE} failed: {error}') from error
    if sha256(data) != SHA256:
        raise BundleError(f'{BUNDLE} does not match its pinned SHA-256; refusing to use it')
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + '.part')
    partial.write_bytes(data)
    os.replace(partial, path)
    return path


def certificates(text):
    """DER certificates from PEM text, in order."""
    found = re.findall(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', text, re.S)
    try:
        return [base64.b64decode(''.join(block.split()), validate=True) for block in found]
    except binascii.Error as error:
        raise BundleError(f'Invalid base64 in a certificate: {error}') from error


def element(data, offset, end):
    """(tag, content start, content end) of the DER element at offset."""
    if offset + 2 > end:
        raise BundleError('Truncated DER element')
    tag, length = data[offset], data[offset + 1]
    offset += 2
    if tag & 0x1f == 0x1f:
        raise BundleError('Unsupported DER tag')
    if length & 0x80:
        count = length & 0x7f
        if not 1 <= count <= 3 or offset + count > end:
            raise BundleError('Unsupported DER length')
        length = int.from_bytes(data[offset:offset + count], 'big')
        offset += count
    if offset + length > end:
        raise BundleError('Truncated DER element')
    return tag, offset, offset + length


def children(data, start, end):
    """[(tag, element start, content start, content end)] of a constructed element."""
    items = []
    while start < end:
        tag, first, last = element(data, start, end)
        items.append((tag, start, first, last))
        start = last
    return items


def expect(item, tag):
    if item[0] != tag:
        raise BundleError(f'Expected DER tag {tag:#x}, found {item[0]:#x}')
    return item


def unsigned(data):
    data = data.lstrip(b'\x00')
    return data or b'\x00'


def label(der, start, end):
    """A readable name for comments: the first CN, else O, else '?'."""
    found = {}
    for rdn in children(der, start, end):
        for attribute in children(der, rdn[2], rdn[3]):
            parts = children(der, attribute[2], attribute[3])
            if len(parts) == 2 and parts[0][0] == 0x06:
                oid = der[parts[0][2]:parts[0][3]]
                if oid in NAME_OIDS and oid not in found:
                    found[oid] = der[parts[1][2]:parts[1][3]].decode('utf-8', 'replace')
    text = next((found[oid] for oid in NAME_OIDS if oid in found), '?')
    return ''.join(c if ' ' <= c <= '~' else '?' for c in text).replace('*/', '*?')


def anchor(der):
    """(name, subject DN bytes, key) for one certificate; key is ('rsa', n, e)
    or ('ec', curve, point). Raises Unsupported for keys BearSSL cannot use."""
    certificate = children(der, *element(der, 0, len(der))[1:])
    tbs = children(der, *expect(certificate[0], 0x30)[2:])
    if tbs and tbs[0][0] == 0xa0:
        tbs = tbs[1:]
    if len(tbs) < 6:
        raise BundleError('Truncated TBSCertificate')
    subject, spki = expect(tbs[4], 0x30), expect(tbs[5], 0x30)
    name = label(der, subject[2], subject[3])
    algorithm, key = children(der, spki[2], spki[3])[:2]
    parameters = children(der, *expect(algorithm, 0x30)[2:])
    oid = der[expect(parameters[0], 0x06)[2]:parameters[0][3]]
    bits = der[expect(key, 0x03)[2]:key[3]]
    if not bits or bits[0] != 0:
        raise BundleError('Unsupported public key bit string')
    bits = bits[1:]
    if oid == RSA_OID:
        numbers = children(bits, *element(bits, 0, len(bits))[1:])
        if len(numbers) != 2:
            raise BundleError('Malformed RSA public key')
        n = unsigned(bits[expect(numbers[0], 0x02)[2]:numbers[0][3]])
        e = unsigned(bits[expect(numbers[1], 0x02)[2]:numbers[1][3]])
        if len(n) * 8 > RSA_MAX_BITS:
            raise Unsupported(f'{name}: RSA key larger than {RSA_MAX_BITS} bits')
        return name, der[subject[1]:subject[3]], ('rsa', n, e)
    if oid == EC_OID:
        if len(parameters) < 2 or parameters[1][0] != 0x06:
            raise Unsupported(f'{name}: EC key without a named curve')
        curve = CURVES.get(der[parameters[1][2]:parameters[1][3]])
        if not curve:
            raise Unsupported(f'{name}: unsupported elliptic curve')
        if len(bits) != curve[1] or bits[0] != 4:
            raise Unsupported(f'{name}: EC point is not uncompressed')
        return name, der[subject[1]:subject[3]], ('ec', curve[0], bits)
    raise Unsupported(f'{name}: unsupported key type')


def array(name, data):
    lines = [f'static const unsigned char {name}[] = {{']
    for i in range(0, len(data), 12):
        lines.append('    ' + ' '.join(f'0x{b:02x},' for b in data[i:i + 12]))
    return lines + ['};']


def convert(text, source):
    """Return (C source, anchor count, skipped descriptions) for PEM text."""
    ders = certificates(text)
    if not ders:
        raise BundleError(f'{source} contains no certificates')
    arrays, entries, skipped = [], [], []
    for index, der in enumerate(ders):
        try:
            name, dn, key = anchor(der)
        except Unsupported as reason:
            skipped.append(str(reason))
            continue
        except (IndexError, ValueError) as error:
            raise BundleError(f'Certificate {index + 1} in {source} is malformed') from error
        i = len(entries)
        arrays += [f'/* {name} */', *array(f'ta{i}_dn', dn)]
        dn_ref = f'{{(unsigned char*)ta{i}_dn, sizeof(ta{i}_dn)}}'
        if key[0] == 'rsa':
            arrays += array(f'ta{i}_n', key[1]) + array(f'ta{i}_e', key[2])
            pkey = (f'{{BR_KEYTYPE_RSA, {{.rsa = {{(unsigned char*)ta{i}_n, sizeof(ta{i}_n), '
                    f'(unsigned char*)ta{i}_e, sizeof(ta{i}_e)}}}}}}')
        else:
            arrays += array(f'ta{i}_q', key[2])
            pkey = f'{{BR_KEYTYPE_EC, {{.ec = {{{key[1]}, (unsigned char*)ta{i}_q, sizeof(ta{i}_q)}}}}}}'
        entries.append(f'    {{{dn_ref}, BR_X509_TA_CA, {pkey}}},')
    if not entries:
        raise BundleError(f'{source} contains no usable certificates')
    header = [f'/* Generated by tools/fetch_ca.py from {source}: Mozilla CA certificates',
              ' * (MPL-2.0) as BearSSL trust anchors. Do not edit or commit.',
              f' * {len(ders)} certificates, {len(entries)} anchors, {len(skipped)} skipped. */',
              '#include <bearssl.h>', '']
    footer = ['', 'const br_x509_trust_anchor tls_trust_anchors[] = {', *entries, '};',
              'const size_t tls_trust_anchor_count = sizeof(tls_trust_anchors) / sizeof(tls_trust_anchors[0]);']
    return '\n'.join(header + arrays + footer) + '\n', len(entries), skipped


def generator():
    return sha256(Path(__file__).read_bytes())


def current(output):
    """True when output was generated from the pinned bundle by this script."""
    manifest = output.with_suffix('.json')
    if not output.is_file() or not manifest.is_file():
        return False
    try:
        data = json.loads(manifest.read_text(encoding='utf-8'))
    except (ValueError, UnicodeError):
        return False
    return (isinstance(data, dict) and data.get('bundle') == BUNDLE and
            data.get('sha256') == SHA256 and data.get('generator') == generator())


def write(output, text, record):
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + '.part')
    with open(partial, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(text)
    os.replace(partial, output)
    output.with_suffix('.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def prepare(directory=DEFAULT_DIR, fetch=download):
    """Download, verify and convert the pinned bundle; returns the C file."""
    directory = Path(directory)
    output = directory / OUTPUT
    if current(output):
        return output
    pem = obtain(directory, fetch)
    text, count, skipped = convert(pem.read_text(encoding='utf-8'), BUNDLE)
    write(output, text, {'bundle': BUNDLE, 'sha256': SHA256, 'generator': generator(),
                         'anchors': count, 'skipped': skipped})
    print(f'TLS trust anchors: {count} from {BUNDLE}' + (f', {len(skipped)} skipped' if skipped else ''),
          flush=True)
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dir', default=str(DEFAULT_DIR), help='download cache (default: out/ca-bundle)')
    parser.add_argument('--pem', help='convert this PEM bundle instead of the pinned download')
    parser.add_argument('--output', help='C file to write (required with --pem)')
    args = parser.parse_args(argv)
    try:
        if args.pem:
            if not args.output:
                parser.error('--output is required with --pem')
            pem = Path(args.pem)
            text, count, skipped = convert(pem.read_text(encoding='utf-8'), pem.name)
            output = Path(args.output)
            write(output, text, {'bundle': pem.name, 'sha256': sha256(pem.read_bytes()),
                                 'generator': generator(), 'anchors': count, 'skipped': skipped})
            for reason in skipped:
                print('Skipped:', reason)
        else:
            output = prepare(args.dir)
    except (BundleError, OSError) as error:
        parser.error(str(error))
    print(f'TLS trust anchors ready: {output}')
    print(f'Build with: python tools/build.py --tls-trust-c "{output}"')


if __name__ == '__main__':
    main()
