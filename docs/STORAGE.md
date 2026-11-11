# Persistent SD files

**Physical Raspberry Pi validation is pending. S1 is not hardware-accepted.**
The default build includes storage support but skips mounting until an explicit
`SD_BASE_CLOCK_HZ` and `STORAGE_PARTITION` are supplied. `STORAGE=0` disables it.
Without a successful mount file syscalls return `FILE_UNAVAILABLE`. Blank/corrupt
cards are never automatically formatted, and failures never fall back to volatile
files. The reboot probe remains opt-in through `STORAGE_VALIDATION=1`.

## Scope and interface

The portable SD engine in `drivers/storage/sd.c` is the retained implementation; the
nightly tutorial driver and FAT reader are not combined with it. The EMMC2
adapter, read/write/erase protocol and firmware handoff are in [SD.md](SD.md).

`kernel/fs/block_store.c` implements version 3, a bounded, checksummed,
block-backed snapshot filesystem over 512-byte sectors. `kernel/fs/storage.c`
retains the version 1/2 compatibility backend. `kernel/fs/files.c` uses them behind `SYS_FILE_*`
IDs, payload layouts, task-owned handles and file operations. The `/ram/<name>`
spelling is retained for compatibility; it now names persistent files. There is
no separate RAM-file fallback. The independent legacy block RAM disk is unused.

New version 3 volumes support **256 shared file/directory entries, 255-byte total
paths and 1 MiB per file**, subject to shared volume space. Components remain
31-byte ASCII names, with 16 open handles and 256 bytes per I/O syscall. File data
is allocated in whole sectors on disk, not reserved in RAM for each entry. The
default 16 MiB data partition provides 16,351 data sectors (slightly under 8 MiB)
per snapshot. A larger partition increases shared space, not the per-file limit.
This is not FAT; files share a namespace, not user-account permissions. Boot-time
Wi-Fi firmware embedding and executable loading are unchanged.

Existing version 1/2 volumes still mount without writes and retain their **16
entries, 47-byte paths and 4096-byte files**. Increasing limits requires explicit
migration to a new V3 data image; there is no in-place or automatic V3 conversion.

Directories are explicit persistent entries, including empty directories. The
implicit, protected mount root is `/ram` (`/` aliases that root); no default
subdirectories are provisioned. V3 path buffers are 256 bytes including the
terminator; each component uses letters, digits, `.`, `_` or `-`. `.` and `..` walk
existing directories, with `..` at root staying at root. Kernel relative paths
start at `/ram`; the terminal resolves them against its own current directory.
Missing parents and file parents are rejected, including before `..`. Duplicate
names under one parent are forbidden; different parents may share a child name.
Directories cannot be opened/truncated/deleted as files. Removing a nonempty
directory returns `FILE_BUSY`; removing root is invalid. No recursive removal
or rename is implemented.

Creation, truncation, each nonempty write and removal commit synchronously before
returning success. Close/reap only release handles; there is no new flush syscall.
Existing errors retain their values; `FILE_CORRUPT=-8` and `FILE_IO_ERROR=-9` are
additive. Invalid arguments/capacity failures do not change files. An I/O failure
quarantines the mount, so staged/uncertain data cannot subsequently be reported
as durable. Close or task exit can still release handles; unmount/remount is
required for recovery. A failed commit may nevertheless be present after reboot.

All kernel APIs are non-reentrant, non-yielding, core-0 operations with trusted
kernel buffers. Mount/unmount reject live handles; handle generations are not
reset by remount. V3 holds only the directory table in RAM and reads file sectors
from the active snapshot. V1/2 retain their small RAM cache. Media checksums are
verified at mount and commit readback, not continuously on every read.

## Version 3 block-backed snapshots

The primary MBR data partition is type `0xda`, with at least **4162 sectors**
(2,130,944 bytes). The partition is split into two banks of `floor(sectors / 2)`
sectors; an odd final sector is unused. Each bank starts with one descriptor,
followed by 32 metadata sectors containing 256 fixed 64-byte entry records, then
only the occupied file-data sectors, packed in entry-slot order. Files have no
fixed reserved data area. Removal/truncation releases space at the next commit.

The descriptor uses the offsets below, with magic `VSTORE3\0`, version 3 and a
payload count of `32 + sum(ceil(file_size / 512))`. CRC covers metadata and live
data, including zero padding after each file's EOF. Each metadata record stores:

| Offset | Field |
|---|---|
| 0–31 | Zero-terminated component name, zero-padded |
| 32 / 36 / 40 | 32-bit type / byte size / parent slot plus one (0 root) |
| 44 | 32-bit first data sector relative to payload; 0 for empty entries |
| 48–63 | Zero |

Types are 0 unused, 1 file and 2 directory. Empty files/directories consume no data
sectors. Unused records are entirely zero. Extents must be exactly packed,
nonoverlapping and inside the declared payload. Names, parents, directory types,
cycles, duplicates, total path lengths, sizes and padding are checked at mount.
Descriptor generation, partition identity and CRC follow the same rules as the
legacy format. All integer encodings are explicitly little-endian.

Every mutation invalidates the inactive descriptor, streams the complete live
snapshot into that bank, syncs and verifies it, then publishes its descriptor.
RAM usage does not grow with file sizes. Capacity checks occur before writes;
failed capacity checks leave data and handles unchanged. I/O failures quarantine
the mount. Mounting never formats a volume or migrates it.

This retains whole-snapshot **write amplification**: each small write copies all
live file sectors. Calls are synchronous, non-yielding core-0 work; large volumes
can cause long service latency. This is a capacity extension, not a throughput
optimization. Unused sectors may retain deleted data; deletion is not secure erase.

Long paths use additive syscalls **40** (`FileOpenLong`), **41** (`FilePathLong`
removal), and **42** (`FileDirectoryLong`). Syscalls 32–39 and their original
48-byte path payloads are unchanged. Read/write/seek/close handles work with both
interfaces. The user library and terminal use the long-path operations.

## Migrate an existing data image

Back up the original image, then create a **new** V3 data image:

```sh
python tools/storage_image.py inspect --image old-data.img --partition 0
python tools/storage_image.py migrate --image old-data.img --partition 0 --output larger-data.img
python tools/storage_image.py inspect --image larger-data.img
```

The source is opened read-only; the output must not already exist. Files, empty
directories and parent relationships are copied from the newest validated legacy
snapshot. Ambiguous generations, invalid/recovered sources and V3 sources are
rejected. Resolve recovery on a backup before migrating. `--sectors` sets the new
partition size (default 32768); insufficient capacity is rejected.

The result contains a type-0xda partition at sector 2048. It is a **data test
image**, not a complete bootable card image, even when the source was a full card
copy. It does not copy boot firmware or other partitions. Use a trusted host tool
and verified partition layout when provisioning a real card. Keep the old image
until contents and persistence of the new volume have been verified.

## Legacy version 1/2 format

Select an explicit primary MBR index 0–3, of type `0xda`, at least **290 sectors**
(148,480 bytes). Every nonempty primary entry is capacity-checked and overlaps
are rejected. Extended partitions and protective GPT MBRs are rejected. Sector 0,
the firmware FAT boot partition and other partitions are outside volume writes.
The data partition must be created separately in a trusted partitioning tool.

Each of two banks is 145 sectors: a descriptor and 144 payload sectors. Each file
occupies one metadata sector plus eight data sectors. All integers are explicitly
little-endian; C structs are not written to disk. Unused bytes are zeroed.

Descriptor offsets:

| Offset | Field |
|---|---|
| 0 | Eight bytes `VSTORE1\0` |
| 8 / 12 | 32-bit format version 1 or 2 / bank index |
| 16 | Nonzero 64-bit generation |
| 24 / 28 | CRC-32 of payload / payload sector count 144 |
| 32 / 40 | 64-bit partition start / sector count |
| 48–507 | Zero |
| 508 | CRC-32 of descriptor bytes 0–507 |

Version 1 metadata holds name bytes 0–31, 32-bit used flag at 32, byte size at 36,
and zero padding from 40. Version 2 uses the same bank/record geometry and magic:
offset 32 is type (0 unused, 1 file, 2 directory), offset 36 is byte size, offset
40 is a 32-bit parent (0 root, otherwise entry slot plus one); padding starts at
44. Directories have zero size and zero data. Unused entries have zero metadata
and data. Parent bounds/types, cycles, duplicate sibling names and complete path
lengths are checked. Names, sizes, type flags, padding and bytes
beyond EOF are checked before exposure. CRC uses the standard reflected CRC-32
polynomial `0xedb88320`, with initial/final inversion. It detects accidental
corruption, not malicious tampering; this is not an image-signature mechanism.

Existing version 1 snapshots mount without writes; their files become root
children. The first deliberate successful mutation writes a complete version 2
snapshot to the inactive bank using the unchanged commit protocol, preserving
all existing entries. The other bank remains recoverable until the next commit.
Mount never reformats or rewrites a schema. New image tooling creates version 2
and inspects both versions independently, reporting version and entry type.
Older kernels/tools do not understand newer formats: do not downgrade a mutated
volume, since an old kernel could select a stale version 1 bank.

Commit order:

1. Invalidate the inactive bank's descriptor, then synchronize.
2. Write its complete payload, synchronize, and verify payload readback CRC.
3. Publish the new checksummed descriptor, synchronize, and verify readback.
4. Update the in-memory active bank and generation. Generation never wraps.

Mount selects the highest valid generation. Equal valid generations are rejected.
One invalid bank yields `STORE_MOUNT_RECOVERED`, with a boot/simulator warning;
both invalid banks yield corruption, not an empty filesystem. Read failures are
I/O errors rather than silently selecting stale data. Recovery can discard a
newer snapshot if that snapshot is damaged, and the warning must be investigated.

The legacy implementation rewrites 74,752 bytes per mutation. It favors a small,
auditable recovery protocol over write amplification or throughput. No speed or
flash-endurance claim is made. Recovery assumes completed syncs are durable and
failed writes affect only their addressed sectors. SD-card FTL/controller behavior
can violate those assumptions under power loss; physical testing is mandatory.

## Provision a regular disk image

Python 3.11+ is required for image tooling. Commands only accept regular files,
never a physical disk. `create` refuses to overwrite an existing path:

```sh
python tools/storage_image.py create --image /path/to/test-data.img
python tools/storage_image.py inspect --image /path/to/test-data.img
```

`create` defaults to version 3 with a 32768-sector data partition; `--sectors`
changes its size. `--version 2` explicitly creates a legacy fixture. `format`
also defaults to V3 and rejects partitions smaller than 4162 sectors before
changing them; it does not resize or migrate an existing partition.

The new image has one type-0xda partition starting at sector 2048. It is a data
test image, **not a bootable Pi card image**. To provision a copy of a complete
bootable card image with a separately created type-0xda partition:

```sh
python tools/storage_image.py format --image /path/to/card-copy.img --partition 1 --confirm-format
python tools/storage_image.py inspect --image /path/to/card-copy.img --partition 1
```

Formatting destroys that volume. Keep a backup and verify the selected partition
before transferring an image to a spare card using a trusted host tool. The tool
prints an image SHA-256 for validation records. It does not repartition images.

The simulator mounts partition 0 of an existing image when
`VANTAGE_STORAGE_IMAGE` is set to its path. Its regular-file adapter uses
`fflush` and `_commit`/`fsync`; it never formats a missing or invalid volume.
The terminal provides file commands and a touch-first text editor; see
[TERMINAL.md](TERMINAL.md). Fresh-process round-trip tests exercise persistence.

An ordinary Pi build also attempts a mount when `STORAGE=1` and a confirmed
clock and explicit primary partition index are supplied, for example:

```sh
make BUILD_DIR=/path/to/configured STORAGE=1 SD_BASE_CLOCK_HZ=100000000 STORAGE_PARTITION=1
```

The clock is illustrative, not a detected or recommended value. A failed normal
mount warns and boot continues with files unavailable; it never runs the reboot
probe. Unconfigured defaults (`SD_BASE_CLOCK_HZ=0`, `STORAGE_PARTITION=-1`) skip
SD safely. Configuring a mount does not establish physical hardware acceptance.

## Hardware-validation image and acceptance

First establish the Pi 4 EMMC2 power, 3.3V signaling and stable source clock as
described in [SD.md](SD.md). The clock below is illustrative, not a detected or
recommended value:

```sh
make BUILD_DIR=/path/to/validation STORAGE_VALIDATION=1 SD_BASE_CLOCK_HZ=100000000 STORAGE_PARTITION=1
```

For kernel build and storage partition

```sh
make partition BUILD_DIR=out STORAGE_VALIDATION=1 SD_BASE_CLOCK_HZ=100000000 STORAGE_PARTITION=1
```

The build requires Python 3 as well as the GNU AArch64 toolchain. Build option
changes invalidate objects, including when reusing a directory. Normal builds
default to `STORAGE_VALIDATION=0`. The validation path mounts an already formatted
volume, halts on failure, and uses the reserved `/ram/.s1-reboot` test file:

- First run creates a deterministic 256-byte probe and logs that it was written.
- A later run verifies its exact contents/length and logs a match. A matching
  probe alone is not proof of reboot; retain distinct cold-boot logs.
- An empty probe is retried as interrupted initialization: creation and its first
  write are separate snapshot commits. This logs a write, not a persistence match.
- A nonempty damaged probe halts; the validation image does not erase or reformat it.

Before accepting storage on hardware, physically verify all of the following on
spare cards. Keep each result tied to an exact image; no model result substitutes
for these checks:

- [ ] Record Pi model/revision, boot firmware version/configuration, card model,
      source clock, commit, build flags, kernel image SHA-256 and complete UART log.
- [ ] Compare sector 0, adjacent sectors, last valid sector and aligned/unaligned
      reads against a trusted host image; confirm CSD capacity and failure bounds.
- [ ] In a disposable kernel harness, call `sd_write_blocks` on a known test-only
      range inside the data partition. Read it back, power-cycle, and verify from
      both the Pi and a trusted host. Check unchanged guard sectors outside it.
- [ ] In a separate disposable harness, use `sd_erase_blocks` only on a deliberately
      reserved raw range outside a filesystem (or after the fixed legacy banks),
      with CSD erase-unit-aligned start
      and count. Verify neighboring sectors are unchanged. Do not assume zero or
      `0xff` erased bytes. Never erase either snapshot bank or a boot partition.
- [ ] Preserve first-run probe output; cold-power-cycle the same image and record
      a successful probe match. Exercise file create, write, seek, list, truncate,
      remove, task-exit cleanup and subsequent cold-boot reads through syscalls.
- [ ] Interrupt power at different commit phases. Verify old/new complete files,
      explicit recovery warnings, and unchanged boot/neighbor partitions.
- [ ] On V3, cold-boot and verify more than 16 entries, nested paths longer than
      47 bytes and files larger than 4096 bytes against host-side contents/checksums.
      Repeat interrupted growth that moves subsequent files' packed allocations.
- [ ] Deliberately damage metadata and payload on copied images. One valid bank
      must warn/recover; two invalid banks must halt mounting without formatting.
- [ ] Exercise absent/unresponsive and write-protected media, repeated cold boots,
      multiple cards, CRC/timeout failures, and explicit reinitialization/remount.

Raw erase testing needs a separate reserved range of at least one aligned erase
unit. Unused V3 bank sectors are not reserved raw space: future commits may use
them. A legacy test volume can instead use extra space beyond its fixed banks.
Remove disposable
test harness changes before shipping an image. Record physical acceptance in a
separate reviewed change tied to the tested hardware and image.

## Software verification

```sh
cmake -S . -B /path/to/files-tests
cmake --build /path/to/files-tests --config Release
ctest --test-dir /path/to/files-tests -C Release -L storage --output-on-failure
cmake -S . -B /path/to/driver-tests
cmake --build /path/to/driver-tests --config Release
ctest --test-dir /path/to/driver-tests -C Release -L drivers --output-on-failure
python -B -m unittest discover -s tests/tools -v
python -B tests/arm/files.py /path/to/fresh/kernel8.elf -v
python -B tests/arm/capacity.py /path/to/fresh/kernel8.elf -v
python -B tests/arm/storage.py /path/to/fresh/kernel8.elf -v
```

Native tests cover persistent syscall semantics, process-exit/remount persistence,
partition bounds, semantic corruption and commit failures at 294 I/O boundaries,
with torn lengths 0/1/255/511/512 on immediate and sync-buffered devices. Native
capacity tests additionally cover allocation/growth, shared-capacity rejection,
semantic corruption and V3 cut-point recovery. Linked ARM tests cover 256 entries,
an exact 255-byte EL0 path, large-file mutation/remount and 1 MiB file bounds.
Tool/host tests migrate V1/2 images while
preserving their source and read the result through the kernel backend. Linked
tests execute actual ARM file/syscall/SD-adapter code against modeled sectors and
MMIO, including a mounted filesystem round trip. CI additionally runs host
address/undefined-behavior sanitizers. These checks do not validate electrical
behavior, actual cards, physical power loss or firmware handoff.
