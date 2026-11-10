"""Linked VSTORE3 capacity and long-path EL0 ABI regressions (modeled sectors)."""
import struct
import sys
import unittest

from a64 import Program
from cpu import PROGRAM_BASE, USER_BASE, FAULT_ADDRESS, OK
from files import FileProcessMachine, DATA, EXIT, call, payload
from file_storage import FileMachine
from memory_management import Image
from storage_image import new_image
from tasks import signed

BUFFER = 0x7400000


class CapacityMachine(FileMachine):
    # Full-size snapshots checksum >3 MiB during mount, unlike the 64 KiB
    # legacy fixtures. Keep a finite budget scaled to that additional work.
    call_timeout_us = 180_000_000
    call_instruction_limit = 400000000


class CapacityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def mount(self, nodes=()):
        m = CapacityMachine(self.image, new_image(3, nodes=nodes))
        m.start()
        self.assertEqual(m.mount_files(), 0)
        return m

    def test_256_real_entries_and_capacity_failure(self):
        m = self.mount([(f'd{i}'.encode(), 2, 0, b'') for i in range(256)])
        m.uc.mem_write(BUFFER, struct.pack('<4I256s256s', 1, 0, 0, 0, b'/ram', b''))
        count = 0
        while m.call('files_directory_long', 1, BUFFER) == 1:
            count += 1
        self.assertEqual(count, 256)
        before = bytes(m.disk)
        m.uc.mem_write(BUFFER, b'/ram/full\0')
        self.assertEqual(signed(m.call('files_open', 1, BUFFER, 6)), -3)
        self.assertEqual(bytes(m.disk), before)

    def test_large_file_partial_growth_and_remount(self):
        content = bytes((i * 17) & 255 for i in range(8193))
        m = self.mount([(b'other', 1, 0, b'untouched')])
        m.uc.mem_write(BUFFER, b'/ram/data\0')
        h = m.call('files_open', 1, BUFFER, 7)
        m.uc.mem_write(BUFFER + 4096, content)
        self.assertEqual(m.call('files_write', 1, h, BUFFER + 4096, len(content)), len(content))
        self.assertEqual(m.call('files_seek', 1, h, 511), 0)
        m.uc.mem_write(BUFFER + 4096, b'changed')
        self.assertEqual(m.call('files_write', 1, h, BUFFER + 4096, 7), 7)
        expected = content[:511] + b'changed' + content[518:]
        reboot = FileMachine(self.image, m.disk); reboot.start()
        self.assertEqual(reboot.mount_files(), 0)
        reboot.uc.mem_write(BUFFER, b'/ram/data\0')
        h = reboot.call('files_open', 1, BUFFER, 1)
        self.assertEqual(reboot.call('files_read', 1, h, BUFFER + 4096, len(expected)), len(expected))
        self.assertEqual(bytes(reboot.uc.mem_read(BUFFER + 4096, len(expected))), expected)
        reboot.uc.mem_write(BUFFER, b'/ram/other\0')
        h = reboot.call('files_open', 1, BUFFER, 1)
        self.assertEqual(reboot.call('files_read', 1, h, BUFFER + 4096, 9), 9)
        self.assertEqual(bytes(reboot.uc.mem_read(BUFFER + 4096, 9)), b'untouched')

    def test_one_mib_file_read_and_capacity_bound(self):
        m = self.mount([(b'max', 1, 0, b'z' * 1048576)])
        m.uc.mem_write(BUFFER, b'/ram/max\0')
        h = m.call('files_open', 1, BUFFER, 3)
        self.assertEqual(m.call('files_seek', 1, h, 1048575), 0)
        self.assertEqual(m.call('files_read', 1, h, BUFFER + 4096, 1), 1)
        self.assertEqual(bytes(m.uc.mem_read(BUFFER + 4096, 1)), b'z')
        self.assertEqual(signed(m.call('files_write', 1, h, BUFFER + 4096, 1)), -3)

    def test_long_path_syscalls_from_isolated_el0_process(self):
        nodes = [(bytes([97 + i]) * 31, 2, i, b'') for i in range(7)]
        path = b'/ram/' + b'/'.join(node[0] for node in nodes) + b'/' + b'z' * 26
        self.assertEqual(len(path), 255)
        m = FileProcessMachine(self.image)
        self.assertEqual(m.call('files_unmount'), 0)
        m.attach_disk(new_image(3, nodes=nodes))
        self.assertEqual(m.mount_files(), 0)
        data = bytearray(0xf00)
        payload(0, struct.pack('<II256s', 7, 0, path), data)
        payload(0x200, struct.pack('<QII256s', 0, 5, 0, b'hello'), data)
        payload(0x400, struct.pack('<4I256s256s', 0, 0, 0, 0, path, b''), data)
        payload(0x700, path + b'\0', data)
        payload(0x800, struct.pack('<II256s', 1, 0, b'x' * 256), data)
        payload(0xa00, struct.pack('<4I256s256s', 0, 0, 0, 0, b'x' * 256, b''), data)
        p = Program(PROGRAM_BASE).mov(19, DATA)
        call(p, 40, 0, 0xe00).mov_reg(20, 0).str(20, 19, 0x200)
        call(p, 34, 0x200, 0xe08)
        call(p, 42, 0x400, 0xe10)
        p.mov_reg(0, 20).mov(8, 36).svc().str(0, 19, 0xe18)
        call(p, 41, 0x700, 0xe20)
        call(p, 40, 0x800, 0xe28)
        call(p, 42, 0xa00, 0xe30)
        call(p, 40, 1, 0xe38)      # Misaligned long open returns FILE_BAD.
        p.syscall(EXIT, 0)
        process = m.run_program(p, bytes(data))
        results = [signed(n) for n in struct.unpack('<8Q', m.user_read(process, DATA + 0xe00, 64))]
        self.assertGreater(results[0], 0)
        self.assertEqual(results[1:], [5, 0, 0, 0, -1, -1, -1])
        fields = struct.unpack('<4I256s256s', m.user_read(process, DATA + 0x400, 528))
        self.assertEqual((fields[2], fields[3], fields[5].rstrip(b'\0')), (1, 5, path))
        self.assertEqual(m.reap(process), (OK, 0))

    def test_long_directory_unmapped_and_readonly_payloads_fault_process(self):
        # Invalid mappings terminate the process, rather than returning an error
        # into compromised userspace. Misalignment alone is a normal FILE_BAD.
        for address in (DATA + 0xf00, USER_BASE):
            with self.subTest(address=hex(address)):
                m = FileProcessMachine(self.image)
                p = Program(PROGRAM_BASE).syscall(42, address).syscall(EXIT, 99)
                process = m.run_program(p, bytes(0xf00))
                self.assertEqual(m.reap(process), (OK, FAULT_ADDRESS))


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
