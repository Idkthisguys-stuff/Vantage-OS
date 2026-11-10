"""VSTORE3 packed-block snapshot encoding and bounded-memory inspection."""
import struct
import zlib

SECTOR = 512
FILES = 256
CAPACITY = 1024 * 1024
META = FILES // 8
MIN_SECTORS = 2 * (1 + META + CAPACITY // SECTOR)
DEFAULT_SECTORS = 32768  # 16 MiB partition, shared capacity is one bank minus metadata.
MAGIC = b'VSTORE3\0'
ALLOWED = b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-'


def tree(nodes):
    names, files = set(), []
    for i, (name, kind, parent, size, _) in enumerate(nodes):
        if kind not in (0, 1, 2) or not 0 <= size <= CAPACITY:
            raise ValueError('Invalid entry type or size')
        if not kind:
            if name or parent or size:
                raise ValueError('Nonempty unused entry')
            continue
        if (not 0 < len(name) < 32 or name in (b'.', b'..') or
                any(c not in ALLOWED for c in name) or (parent, name) in names or (kind == 2 and size)):
            raise ValueError('Invalid name, duplicate sibling, or directory data')
        names.add((parent, name))
        chain, seen, node = [], set(), i + 1
        while node:
            if not 1 <= node <= FILES or node in seen:
                raise ValueError('Invalid parent or directory cycle')
            seen.add(node)
            component, ancestor_kind, node_parent, _, _ = nodes[node - 1]
            if node != i + 1 and ancestor_kind != 2:
                raise ValueError('Parent is not a directory')
            chain.append(component.decode('ascii'))
            node = node_parent
        path = '/ram/' + '/'.join(reversed(chain))
        if len(path) >= 256:
            raise ValueError('Path exceeds 255 bytes')
        files.append({'path': path, 'size': size, 'type': 'directory' if kind == 2 else 'file'})
    return files


def encode_bank(bank, generation, first, sectors, nodes=()):
    if bank not in (0, 1) or not 0 < generation <= 0xffffffffffffffff:
        raise ValueError('Invalid bank identity or generation')
    if sectors < MIN_SECTORS or len(nodes) > FILES:
        raise ValueError('V3 needs at least 4162 sectors and at most 256 entries')
    nodes = list(nodes) + [(b'', 0, 0, b'')] * (FILES - len(nodes))
    tree([(name, kind, parent, len(content), 0) for name, kind, parent, content in nodes])
    metadata, data, next_sector = bytearray(META * SECTOR), bytearray(), META
    for slot, (name, kind, parent, content) in enumerate(nodes):
        if not kind:
            if content:
                raise ValueError('Unused entry has data')
            continue
        struct.pack_into('<32sIIII', metadata, slot * 64, name, kind, len(content), parent,
                         next_sector if content else 0)
        blocks = (len(content) + SECTOR - 1) // SECTOR
        data.extend(content)
        data.extend(bytes(blocks * SECTOR - len(content)))
        next_sector += blocks
    if next_sector >= sectors // 2:
        raise ValueError('Files exceed shared volume capacity')
    payload = metadata + data
    header = bytearray(SECTOR)
    struct.pack_into('<8sIIQIIQQ', header, 0, MAGIC, 3, bank, generation,
                     zlib.crc32(payload), next_sector, first, sectors)
    struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
    return bytes(header) + payload


def inspect_bank(f, bank, first, sectors):
    invalid = {'bank': bank, 'valid': False, 'version': 3}
    f.seek((first + bank * (sectors // 2)) * SECTOR)
    header = f.read(SECTOR)
    if len(header) != SECTOR or sectors < MIN_SECTORS:
        return invalid
    magic, version, identity, generation, expected, count, start, length = struct.unpack_from('<8sIIQIIQQ', header)
    if (magic != MAGIC or version != 3 or identity != bank or not generation or
            not META <= count <= META + FILES * (CAPACITY // SECTOR) or count >= sectors // 2 or
            start != first or length != sectors or any(header[48:508]) or
            struct.unpack_from('<I', header, 508)[0] != zlib.crc32(header[:508])):
        return invalid
    metadata = f.read(META * SECTOR)
    if len(metadata) != META * SECTOR:
        return invalid
    crc, nodes, tails, next_sector = zlib.crc32(metadata), [], {}, META
    for slot in range(FILES):
        record = metadata[slot * 64:(slot + 1) * 64]
        name_field, kind, size, parent, sector = struct.unpack_from('<32sIIII', record)
        name = name_field.split(b'\0', 1)[0]
        if any(name_field[len(name):]) or any(record[48:]):
            return invalid
        if size:
            blocks = (size + SECTOR - 1) // SECTOR
            if sector != next_sector or size > CAPACITY or blocks > count - next_sector:
                return invalid
            next_sector += blocks
            if size % SECTOR:
                tails[next_sector - 1] = size % SECTOR
        elif sector:
            return invalid
        nodes.append((name, kind, parent, size, sector))
    if next_sector != count:
        return invalid
    for offset in range(META, count):
        data = f.read(SECTOR)
        if len(data) != SECTOR or (offset in tails and any(data[tails[offset]:])):
            return invalid
        crc = zlib.crc32(data, crc)
    if crc != expected:
        return invalid
    try:
        files = tree(nodes)
    except (ValueError, UnicodeDecodeError):
        return invalid
    return {'bank': bank, 'valid': True, 'version': 3, 'generation': generation, 'files': files}
