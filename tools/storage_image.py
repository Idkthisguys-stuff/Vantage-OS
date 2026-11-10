"""Create/provision regular disk-image files for the experimental storage format.

Only accepts regular files. Formatting is explicit and changes only the selected
type-0xda primary partition, including invalidating descriptors of both layouts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import zlib
import storage_v3 as v3

SECTOR = 512
FILES = 16
CAPACITY = 4096
PAYLOAD = FILES * 9
BANK = PAYLOAD + 1
MIN_SECTORS = BANK * 2
MAGIC = b'VSTORE1\0'


def empty_bank(bank, generation, first, sectors, version=2):
    payload = bytes(PAYLOAD * SECTOR)
    header = bytearray(SECTOR)
    struct.pack_into('<8sIIQIIQQ', header, 0, MAGIC, version, bank, generation,
                     zlib.crc32(payload), PAYLOAD, first, sectors)
    struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
    return bytes(header) + payload


def new_image(version=3, sectors=None, nodes=()):
    first = 2048
    sectors = (v3.DEFAULT_SECTORS if version == 3 else MIN_SECTORS) if sectors is None else sectors
    if version not in (1, 2, 3) or sectors < (v3.MIN_SECTORS if version == 3 else MIN_SECTORS):
        raise ValueError('Invalid version or insufficient partition sectors')
    image = bytearray((first + sectors) * SECTOR)
    image[510:512] = b'\x55\xaa'
    image[450] = 0xda
    struct.pack_into('<II', image, 454, first, sectors)
    for bank in range(2):
        stride = sectors // 2 if version == 3 else BANK
        start = (first + bank * stride) * SECTOR
        data = v3.encode_bank(bank, bank + 1, first, sectors, nodes) if version == 3 else empty_bank(bank, bank + 1, first, sectors, version)
        image[start:start + len(data)] = data
    return image


def partition(mbr, total_sectors, index):
    if index not in range(4) or len(mbr) != SECTOR or mbr[510:] != b'\x55\xaa':
        raise ValueError('Invalid primary MBR partition selection')
    entries = []
    for i in range(4):
        boot, kind = mbr[446 + 16 * i], mbr[450 + 16 * i]
        first, count = struct.unpack_from('<II', mbr, 454 + 16 * i)
        if not (boot or kind or first or count):
            entries.append((0, 0, 0))
            continue
        if (boot not in (0, 0x80) or kind in (0, 5, 0x0f, 0x85, 0xee) or
                not first or not count or first >= total_sectors or count > total_sectors - first):
            raise ValueError('Invalid, extended, or GPT partition entry')
        if any(old_count and first < old_first + old_count and old_first < first + count
               for _, old_first, old_count in entries):
            raise ValueError('Overlapping primary partitions')
        entries.append((kind, first, count))
    kind, first, count = entries[index]
    if kind != 0xda or count < MIN_SECTORS:
        raise ValueError('Select a type-0xda data partition of at least 290 sectors')
    return first, count


def inspect_bank(data, bank, first, sectors):
    header, payload = data[:SECTOR], data[SECTOR:]
    if len(data) != BANK * SECTOR:
        return {'bank': bank, 'valid': False}
    fields = struct.unpack_from('<8sIIQIIQQ', header)
    magic, version, identity, generation, crc, count, start, length = fields
    valid = (magic == MAGIC and version in (1, 2) and identity == bank and generation > 0 and
             count == PAYLOAD and start == first and length == sectors and
             struct.unpack_from('<I', header, 508)[0] == zlib.crc32(header[:508]) and
             crc == zlib.crc32(payload) and not any(header[48:508]))
    files = []
    names, nodes = set(), []
    for i in range(FILES):
        record = payload[i * 9 * SECTOR:(i + 1) * 9 * SECTOR]
        name = bytes(record[:32]).split(b'\0', 1)[0]
        used, size = struct.unpack_from('<II', record, 32)
        parent = struct.unpack_from('<I', record, 40)[0] if version == 2 else 0
        allowed = b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-'
        valid &= (used in ((0, 1, 2) if version == 2 else (0, 1)) and size <= CAPACITY and
                  not any(record[44 if version == 2 else 40:SECTOR]) and
                  not any(record[len(name):32]) and not any(record[SECTOR + size:]))
        if used:
            valid &= (0 < len(name) < 32 and name not in (b'.', b'..') and
                       all(c in allowed for c in name) and (parent, name) not in names and
                       (used != 2 or size == 0))
            names.add((parent, name))
        else:
            valid &= not name and not size and not parent
        nodes.append((used, parent, name, size))
    for i, (used, parent, name, size) in enumerate(nodes):
        if not used:
            continue
        chain, seen, node = [], set(), i + 1
        while node:
            if node not in range(1, FILES + 1) or node in seen:
                valid = False
                break
            seen.add(node)
            kind, ancestor, component, _ = nodes[node - 1]
            if node != i + 1 and kind != 2:
                valid = False
                break
            chain.append(component.decode('ascii', errors='replace'))
            node = ancestor
        path = '/ram/' + '/'.join(reversed(chain))
        valid &= len(path) < 48
        files.append({'path': path, 'size': size, 'type': 'directory' if used == 2 else 'file'})
    return {'bank': bank, 'valid': bool(valid), 'version': version,
            'generation': generation, 'files': files if valid else []}


def regular_file(path, mode):
    f = open(path, mode)
    if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
        f.close()
        raise ValueError('Only regular image files are supported')
    return f


def inspect_volume(f, first, count):
    # Try both geometries independently: a torn first descriptor must not hide
    # the second bank. Never silently select between two different valid formats.
    old, current = [], []
    for bank in range(2):
        f.seek((first + bank * BANK) * SECTOR)
        old.append(inspect_bank(f.read(BANK * SECTOR), bank, first, count))
        current.append(v3.inspect_bank(f, bank, first, count))
    if any(b['valid'] for b in old) and any(b['valid'] for b in current):
        raise ValueError('Ambiguous mixed volume formats')
    banks = current if any(b['valid'] for b in current) else old
    valid = [b for b in banks if b['valid']]
    if not valid:
        raise ValueError('No valid storage snapshot')
    if len(valid) == 2 and valid[0]['generation'] == valid[1]['generation']:
        raise ValueError('Ambiguous equal snapshot generations')
    return banks, max(valid, key=lambda bank: bank['generation'])


def migrate(source, destination, index, sectors):
    """Copy a validated legacy snapshot into a new V3 data image; source is read-only."""
    with regular_file(source, 'rb') as f:
        size = os.fstat(f.fileno()).st_size
        if size % SECTOR:
            raise ValueError('Image size is not a multiple of 512 bytes')
        first, count = partition(f.read(SECTOR), size // SECTOR, index)
        banks, selected = inspect_volume(f, first, count)
        if selected['version'] == 3:
            raise ValueError('Migration expects a version 1 or 2 source')
        if not all(b['valid'] for b in banks):
            raise ValueError('Source needs recovery; inspect and repair a copy before migration')
        f.seek((first + selected['bank'] * BANK + 1) * SECTOR)
        payload = f.read(PAYLOAD * SECTOR)
        nodes = []
        for slot in range(FILES):
            record = payload[slot * 9 * SECTOR:(slot + 1) * 9 * SECTOR]
            name = record[:32].split(b'\0', 1)[0]
            kind, length = struct.unpack_from('<II', record, 32)
            parent = struct.unpack_from('<I', record, 40)[0] if selected['version'] == 2 else 0
            nodes.append((name, kind, parent, record[SECTOR:SECTOR + length]))
        # Validate/encode before opening the output, using only live legacy data.
        count = v3.DEFAULT_SECTORS if sectors is None else sectors
        snapshots = [v3.encode_bank(bank, bank + 1, 2048, count, nodes) for bank in range(2)]
    # Exclusive create also prevents source==destination, symlink aliases, and
    # accidental overwrite. The complete original remains usable after failure.
    with regular_file(destination, 'xb') as f:
        write_image(f, 3, count, snapshots)


def write_image(f, version, count, snapshots=None):
    """Provision a regular file without allocating RAM proportional to its size."""
    minimum = v3.MIN_SECTORS if version == 3 else MIN_SECTORS
    if not minimum <= count <= 0xffffffff - 2048:
        raise ValueError('Partition sector count is out of range')
    if snapshots is None:
        snapshots = [(v3.encode_bank(bank, bank + 1, 2048, count) if version == 3 else
                      empty_bank(bank, bank + 1, 2048, count, version)) for bank in range(2)]
    mbr = bytearray(SECTOR); mbr[510:512] = b'\x55\xaa'; mbr[450] = 0xda
    struct.pack_into('<II', mbr, 454, 2048, count)
    f.truncate((2048 + count) * SECTOR); f.seek(0); f.write(mbr)
    stride = count // 2 if version == 3 else BANK
    for bank, snapshot in enumerate(snapshots):
        f.seek((2048 + bank * stride) * SECTOR); f.write(snapshot)
    f.flush(); os.fsync(f.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('create', 'format', 'inspect', 'migrate'))
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--partition', type=int, default=0)
    parser.add_argument('--confirm-format', action='store_true')
    parser.add_argument('--version', type=int, choices=(1, 2, 3), default=3)
    parser.add_argument('--sectors', type=int, help='new data partition size (default: 32768 for V3)')
    parser.add_argument('--output', type=Path, help='new, nonexisting V3 image for migrate')
    args = parser.parse_args()
    if args.command == 'migrate':
        if args.output is None:
            parser.error('migrate requires --output; the source image is never overwritten')
        migrate(args.image, args.output, args.partition, args.sectors)
        args.image = args.output
    elif args.command == 'create':
        if args.partition != 0:
            parser.error('create makes partition 0')
        with regular_file(args.image, 'xb') as f:
            count = args.sectors if args.sectors is not None else (v3.DEFAULT_SECTORS if args.version == 3 else MIN_SECTORS)
            write_image(f, args.version, count)
    else:
        if args.command == 'format' and not args.confirm_format:
            parser.error('format requires --confirm-format and destroys the selected volume')
        with regular_file(args.image, 'r+b' if args.command == 'format' else 'rb') as f:
            size = os.fstat(f.fileno()).st_size
            if size % SECTOR:
                raise ValueError('Image size is not a multiple of 512 bytes')
            first, count = partition(f.read(SECTOR), size // SECTOR, args.partition)
            if args.command == 'format':
                if args.version == 3 and count < v3.MIN_SECTORS:
                    raise ValueError('V3 needs at least 4162 partition sectors; migrate to a new image instead')
                # Invalidate both old descriptors durably before provisioning.
                offsets = {first + bank * stride for bank in range(2) for stride in (BANK, count // 2)}
                for offset in offsets:
                    f.seek(offset * SECTOR); f.write(bytes(SECTOR))
                f.flush(); os.fsync(f.fileno())
                for bank in range(2):
                    stride = count // 2 if args.version == 3 else BANK
                    f.seek((first + bank * stride) * SECTOR)
                    f.write(v3.encode_bank(bank, bank + 1, first, count) if args.version == 3 else
                            empty_bank(bank, bank + 1, first, count, args.version))
                f.flush(); os.fsync(f.fileno())
            else:
                banks, _ = inspect_volume(f, first, count)
                valid = [b for b in banks if b['valid']]
                if len(valid) == 2 and valid[0]['generation'] == valid[1]['generation']:
                    raise ValueError('Ambiguous equal snapshot generations')
                print(json.dumps({'partition': args.partition, 'first_sector': first, 'sectors': count,
                                  'recovered': len(valid) == 1, 'banks': banks}, indent=2))
                if not valid:
                    raise ValueError('No valid storage snapshot')
    # Bounded-memory image hash for validation records, never device identifiers.
    with regular_file(args.image, 'rb') as f:
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    print('Image SHA-256:', digest)


if __name__ == '__main__':
    main()
