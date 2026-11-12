"""Linked network syscalls, process-owned sockets, the terminal's network
commands and the RNG200 entropy source.

Usage: python tests/arm/network.py path/to/fresh/kernel8.elf -v
Programs run as isolated EL0 processes on the CPU model (see cpu.py). No radio
is attached in this image, so these check the boundary, ownership and
fallbacks; protocol behaviour is covered by the native network_stack test.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE

from a64 import Program
from cpu import CpuMachine, PROGRAM_BASE, USER_BASE, PAGE, FAULT_ADDRESS, OK, image, signed
from memory_management import Image
from processes import ProcessMachine
from terminal import TerminalMachine

STATUS, SCAN, JOIN, LEAVE, RESOLVE, SOCKET, TIME, PROFILE, RANDOM = range(48, 57)
BAD, UNAVAILABLE, OFFLINE, AGAIN, TIMEOUT, NO_SPACE, BAD_HANDLE = -1, -2, -3, -4, -5, -6, -7
OPEN, CONNECT, SEND, RECEIVE, CLOSE, SOCKET_STATUS = range(6)
PROFILE_ADD, PROFILE_REMOVE, PROFILE_LIST, PROFILE_CLEAR = range(4)
UDP, TCP, ICMP = 1, 2, 3
EXIT = 18
DATA = USER_BASE + PAGE
RNG = 0xfe104000


def call(p, number, offset, result):
    """Issue a call with the payload at DATA + offset; keep x0 at DATA + result."""
    return p.syscall(number, DATA + offset).str(0, 19, result)


def socket_call(operation, kind=0, handle=0, address=0, port=0, length=0, data=b''):
    return struct.pack('<IIQIHHIIiI1024s', operation, kind, handle, address, port, 0, length, 0, 0, 0, data)


def profile_call(operation, ssid=b'', passphrase=b'', flags=0, cursor=0):
    return struct.pack('<5I32s64sI', operation, cursor, flags, len(ssid), len(passphrase), ssid, passphrase, 0)


class NetProcessMachine(ProcessMachine):
    def run_program(self, program, data):
        process = self.create_process(image(program.code(), data=data))
        assert process and self.run_all() == OK
        return process


class NetworkSyscallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def test_el0_calls_use_copied_payloads_without_a_radio(self):
        m = NetProcessMachine(self.image)
        data = bytearray(0x1000)
        data[0x000:0x088] = b'\xa5' * 0x88
        data[0x100:0x108] = struct.pack('<II', 0, 0)
        data[0x108:0x108 + 14] = b'192.168.50.10\0'
        data[0x300:0x300 + 1064] = socket_call(OPEN, UDP)
        data[0x780:0x780 + 112] = struct.pack('<II32s64sII', 4, 8, b'home', b'password', 0, 0)
        p = Program(PROGRAM_BASE).mov(19, DATA)
        call(p, STATUS, 0x000, 0xf00)
        call(p, RESOLVE, 0x100, 0xf08)
        call(p, JOIN, 0x780, 0xf10)
        call(p, SOCKET, 0x300, 0xf18)
        p.str(0, 19, 0x308).mov(21, SOCKET_STATUS).str(21, 19, 0x300)
        call(p, SOCKET, 0x300, 0xf20)
        p.mov(21, CLOSE).str(21, 19, 0x300)
        call(p, SOCKET, 0x300, 0xf28)
        call(p, SOCKET, 0x300, 0xf30)
        call(p, STATUS, 0x004, 0xf38)      # Misaligned: an ordinary error.
        call(p, LEAVE, 0x000, 0xf40)
        call(p, SCAN, 0x100, 0xf48)
        p.syscall(EXIT, 0)
        process = m.run_program(p, bytes(data))
        results = [signed(v) for v in struct.unpack('<10Q', m.user_read(process, DATA + 0xf00, 80))]
        self.assertEqual(results[0], 0)
        self.assertEqual(results[1], 1)
        self.assertEqual(results[2], UNAVAILABLE)
        self.assertGreater(results[3], 0)
        self.assertEqual(results[4:], [0, 0, BAD_HANDLE, BAD, UNAVAILABLE, UNAVAILABLE])
        status = m.user_read(process, DATA, 136)
        self.assertEqual(status, bytes(136))   # Unavailable: every field cleared, nothing leaked.
        self.assertEqual(struct.unpack('<II', m.user_read(process, DATA + 0x100, 8)), (0, 0xc0a8320a))
        call_out = struct.unpack('<IIQIHH', m.user_read(process, DATA + 0x300, 24))
        self.assertEqual(call_out[2], results[3])
        self.assertGreaterEqual(call_out[5], 49152)   # Ephemeral UDP port reported by STATUS.
        self.assertEqual(m.reap(process), (OK, 0))

    def test_time_profiles_and_random_without_a_radio(self):
        m = NetProcessMachine(self.image)
        data = bytearray(0x1000)
        data[0x000:0x018] = b'\xa5' * 0x18
        data[0x100:0x178] = profile_call(PROFILE_ADD, b'home', b'password')
        list_request = profile_call(PROFILE_LIST, passphrase=b'\x5a' * 64)
        data[0x200:0x278] = list_request
        data[0x300:0x378] = profile_call(9, b'home')
        data[0x400:0x508] = struct.pack('<II', 16, 0) + b'\x77' * 256
        data[0x600:0x608] = struct.pack('<II', 0, 0)
        p = Program(PROGRAM_BASE).mov(19, DATA)
        call(p, TIME, 0x000, 0xf00)
        call(p, TIME, 0x004, 0xf08)        # Misaligned.
        call(p, PROFILE, 0x100, 0xf10)
        call(p, PROFILE, 0x200, 0xf18)
        call(p, PROFILE, 0x300, 0xf20)     # Unknown operation.
        call(p, PROFILE, 0x102, 0xf28)     # Misaligned.
        call(p, RANDOM, 0x400, 0xf30)
        call(p, RANDOM, 0x600, 0xf38)      # Zero length.
        call(p, RANDOM, 0x402, 0xf40)      # Misaligned.
        p.syscall(EXIT, 0)
        process = m.run_program(p, bytes(data))
        results = [signed(v) for v in struct.unpack('<9Q', m.user_read(process, DATA + 0xf00, 72))]
        self.assertEqual(results, [0, BAD, UNAVAILABLE, UNAVAILABLE, BAD, BAD, UNAVAILABLE, BAD, BAD])
        self.assertEqual(m.user_read(process, DATA, 0x18), bytes(0x18))   # Not synced: zeroed time.
        self.assertEqual(m.user_read(process, DATA + 0x200, 120), list_request)   # Nothing copied out.
        self.assertEqual(m.user_read(process, DATA + 0x408, 256), b'\x77' * 256)
        self.assertEqual(m.reap(process), (OK, 0))

    def test_exited_process_releases_every_socket(self):
        m = NetProcessMachine(self.image)
        data = bytearray(0x1000)
        data[0:1064] = socket_call(OPEN, TCP)
        leak = Program(PROGRAM_BASE).mov(19, DATA)
        for slot in range(9):
            call(leak, SOCKET, 0, 0xf00 + slot * 8)
        leak.syscall(EXIT, 0)
        first = m.run_program(leak, bytes(data))
        results = [signed(v) for v in struct.unpack('<9Q', m.user_read(first, DATA + 0xf00, 72))]
        self.assertTrue(all(r > 0 for r in results[:8]), results)
        self.assertEqual(results[8], NO_SPACE)
        self.assertEqual(m.reap(first), (OK, 0))
        again = Program(PROGRAM_BASE).mov(19, DATA)
        call(again, SOCKET, 0, 0xf00)
        again.syscall(EXIT, 0)
        second = m.run_program(again, bytes(data))
        self.assertGreater(signed(struct.unpack('<Q', m.user_read(second, DATA + 0xf00, 8))[0]), 0)
        self.assertEqual(m.reap(second), (OK, 0))

    def test_inaccessible_outputs_fault_the_process(self):
        # Status into read-only text, and a receive payload running off the data page.
        for number, address in ((STATUS, USER_BASE), (SOCKET, DATA + 0xc00), (RESOLVE, USER_BASE),
                                (TIME, USER_BASE), (TIME, DATA + 0xff8)):
            with self.subTest(number=number, address=hex(address)):
                m = NetProcessMachine(self.image)
                data = bytearray(0x1000)
                data[0xc00:0xc00 + 0x400] = socket_call(RECEIVE, length=16)[:0x400]
                p = Program(PROGRAM_BASE).syscall(number, address).syscall(EXIT, 99)
                process = m.run_program(p, bytes(data))
                self.assertEqual(m.reap(process), (OK, FAULT_ADDRESS))

    def test_send_payload_may_be_read_only(self):
        m = NetProcessMachine(self.image)
        p = Program(PROGRAM_BASE).mov(19, DATA).syscall(SOCKET, USER_BASE).str(0, 19, 0).syscall(EXIT, 0)
        process = m.run_program(p, bytes(16))
        # The image header decodes as an invalid handle: rejected, not a fault.
        self.assertEqual(signed(struct.unpack('<Q', m.user_read(process, DATA, 8))[0]), BAD_HANDLE)
        self.assertEqual(m.reap(process), (OK, 0))

    def test_kernel_callers_have_no_network_owner(self):
        m = NetProcessMachine(self.image)
        for number in range(STATUS, RANDOM + 1):
            for pointer in (0, m.image.symbols['__kernel_stack_top'] - 2048, USER_BASE):
                with self.subTest(number=number, pointer=hex(pointer)):
                    self.assertEqual(signed(m.call('handle_sys_call', number, pointer)), UNAVAILABLE)


class TerminalNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def test_commands_report_missing_radio_and_mask_the_passphrase(self):
        m = TerminalMachine(self.image)
        m.launch()
        m.command('net')
        self.assertIn('Not connected.\nWi-Fi: unavailable (no radio firmware)\n', m.output.decode())
        m.command('net join "Home Net"')
        self.assertTrue(any(text.startswith('Passphrase:') for text in m.rows.values()))
        m.command('correct horse battery')
        self.assertTrue(any(text == 'Passphrase: ' + '*' * 21 for text in m.rows.values()))
        self.assertNotIn(b'correct horse', m.output)
        self.assertIn('Wi-Fi unavailable (no radio firmware).', m.output.decode())
        m.command('net lookup vantage.test')
        self.assertIn('vantage.test: Network unavailable.', m.output.decode())
        m.command('net get https://vantage.test/')
        self.assertIn('vantage.test: Network unavailable.', m.output.decode().split('net get')[-1])
        m.command('net saved')
        saved = m.output.decode().split('net saved')[-1]
        self.assertIn('Network unavailable.', saved)
        m.command('net ping')
        self.assertIn('Usage: net ping HOST', m.output.decode())
        self.assertEqual(m.rows[max(r for r, t in m.rows.items() if t)], '>')


class RandomTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def machine(self, words, failing=False):
        m = CpuMachine(self.image)
        m.uc.mem_map(RNG, 0x1000)
        state = {'words': list(words), 'writes': [], 'reads': 0}

        def read(uc, access, address, size, value, _):
            state['reads'] += 1
            offset = address - RNG
            if offset == 0x18:
                uc.mem_write(address, struct.pack('<I', 0x20 if failing else 0))
            elif offset == 0x24:
                uc.mem_write(address, struct.pack('<I', len(state['words'])))
            elif offset == 0x20 and state['words']:
                uc.mem_write(address, struct.pack('<I', state['words'].pop(0)))

        def write(uc, access, address, size, value, _):
            state['writes'].append((address - RNG, value))

        m.uc.hook_add(UC_HOOK_MEM_READ, read, begin=RNG, end=RNG + 0xfff)
        m.uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=RNG, end=RNG + 0xfff)
        return m, state

    def test_working_generator_supplies_network_entropy(self):
        m, state = self.machine([0x11111111, 0x22222222, 0xdeadbeef, 0x12345678])
        self.assertEqual(signed(m.call('pi4_rng_start', m.call('pi4_hardware_ops'))), 1)
        self.assertIn((0, 1), state['writes'])          # RBGEN enabled.
        self.assertEqual(m.call('net_port_random'), 0xdeadbeef)
        self.assertEqual(m.call('net_port_random'), 0x12345678)

    def test_failed_or_empty_generator_is_reported_and_bypassed(self):
        for words, failing in (([1, 2, 3], True), ([], False), ([7, 7], False)):
            with self.subTest(words=words, failing=failing):
                m, state = self.machine(words, failing)
                self.assertEqual(signed(m.call('pi4_rng_start', m.call('pi4_hardware_ops'))), -5)
                if failing:
                    self.assertIn((0x04, 1), state['writes'])   # One soft reset was attempted.
                reads = state['reads']
                values = {m.call('net_port_random') for _ in range(8)}
                self.assertEqual(state['reads'], reads)           # The fallback leaves the device alone.
                self.assertGreater(len(values), 6)


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0]] + sys.argv[2:])
