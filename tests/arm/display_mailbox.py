"""Exercise linked Pi display mailbox code with scripted MMIO, not real firmware.

Usage: python tests/arm/display_mailbox.py path/to/kernel8.elf -v
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs
from memory_management import Image, Machine

STATUS, READ, WRITE, TIMER = 0xfe00b898, 0xfe00b880, 0xfe00b8a0, 0xfe003004
FULL, EMPTY = 0x80000000, 0x40000000


class MailboxMachine(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.clock, self.step = 0, 200000
        self.full, self.reply, self.bad = False, True, False
        self.queue, self.writes, self.presented, self.log = [], [], [], []
        self.buffer = image.symbols['mbox']
        self.buffer_accesses = 0
        self.uc.mem_map(0xfe00b000, 4096)
        self.uc.mem_map(0xfe003000, 4096)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_mmio, begin=0xfe00b000, end=0xfe00bfff)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.read_mmio, begin=TIMER, end=TIMER + 3)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write_mmio, begin=WRITE, end=WRITE + 3)
        self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.buffer_access,
                         begin=self.buffer, end=self.buffer + 191)
        address = image.symbols['render_present']
        self.uc.hook_add(UC_HOOK_CODE, self.present, begin=address, end=address)
        address = image.symbols['uart_writeText']
        self.uc.hook_add(UC_HOOK_CODE, self.uart, begin=address, end=address)
        self.set32('fbBase', 0x6000000, 8)
        self.set32('fbPitch', 3200)
        self.set32('pageHeight', 480)
        self.set32('drawPage', 1)

    def set32(self, name, value, size=4):
        self.uc.mem_write(self.image.symbols[name], value.to_bytes(size, 'little'))

    def get32(self, name):
        return struct.unpack('<I', self.uc.mem_read(self.image.symbols[name], 4))[0]

    def read_mmio(self, uc, access, address, size, value, _):
        if address == TIMER:
            self.clock = (self.clock + self.step) & 0xffffffff
            result = self.clock
        elif address == STATUS:
            result = (FULL if self.full else 0) | (0 if self.queue else EMPTY)
        elif address == READ:
            result = self.queue.pop(0)
        else:
            return
        uc.mem_write(address, struct.pack('<I', result))

    def write_mmio(self, uc, access, address, size, value, _):
        self.writes.append(value)
        if self.reply:
            words = list(struct.unpack('<48I', uc.mem_read(self.buffer, 192)))
            words[1] = 0x80000001 if self.bad else 0x80000000
            if words[2] == 0x48009:
                words[4] = 0x80000008
            uc.mem_write(self.buffer, struct.pack('<48I', *words))
            self.queue.append(value)

    def present(self, uc, address, size, _):
        self.presented.append(uc.reg_read(regs.UC_ARM64_REG_X1))
        uc.reg_write(regs.UC_ARM64_REG_X0, 1)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def uart(self, uc, address, size, _):
        pointer = uc.reg_read(regs.UC_ARM64_REG_X0)
        self.log.append(bytes(uc.mem_read(pointer, 200)).split(b'\0', 1)[0].decode())
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def buffer_access(self, uc, access, address, size, value, _):
        self.buffer_accesses += 1


class DisplayMailboxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(sys.argv[1])

    def setUp(self):
        self.m = MailboxMachine(self.image)

    def test_full_retry_and_successful_flip(self):
        m = self.m
        self.assertEqual(m.buffer % 64, 0)
        self.assertEqual(m.image.sizes['mbox'], 192)
        m.clock, m.full = 0xfff80000, True
        m.call('fb_swap')
        self.assertEqual((m.get32('drawPage'), m.get32('visiblePage')), (1, 0))
        self.assertEqual(m.writes, [])
        m.full = False
        m.call('fb_swap')
        self.assertEqual((m.get32('drawPage'), m.get32('visiblePage')), (2, 1))
        self.assertEqual(len(m.writes), 1)
        self.assertEqual(m.presented, [0x6000000 + 480 * 3200] * 2)

    def test_late_optional_property_and_unknown_reply(self):
        m = self.m
        optional_token = m.image.symbols['mailbox'] | 8
        self.assertNotEqual(optional_token, m.buffer | 8)
        m.queue.extend([optional_token, 0x987658])
        m.call('fb_swap')
        self.assertEqual(m.get32('drawPage'), 2)
        self.assertEqual(m.queue, [])

    def test_timeout_quarantines_and_freezes(self):
        m = self.m
        m.reply = False
        m.queue.append(0x123458)
        m.call('fb_swap')
        self.assertEqual((m.get32('drawPage'), m.get32('visiblePage')), (1, 0))
        self.assertEqual(len(m.writes), 1)
        before = bytes(m.uc.mem_read(m.buffer, 192))
        accesses = m.buffer_accesses
        m.call('fb_swap')
        self.assertEqual(m.call('mbox_call', 8), 0)
        self.assertEqual(bytes(m.uc.mem_read(m.buffer, 192)), before)
        self.assertEqual(m.buffer_accesses, accesses)
        self.assertEqual(len(m.writes), 1)
        self.assertEqual(len(m.presented), 1)
        self.assertEqual(len(m.log), 1)
        self.assertIn('scanout updates stopped', m.log[0])
        self.assertEqual(m.call('fb_hidden_page'), 0)

    def test_failed_or_invalid_ack_freezes(self):
        for bad in ('response', 'offset', 'tag', 'short', 'long'):
            with self.subTest(bad=bad):
                m = MailboxMachine(self.image)
                m.bad = bad == 'response'
                if bad != 'response':
                    def corrupt(uc, access, address, size, value, _, kind=bad):
                        if address == WRITE:
                            index = 6 if kind == 'offset' else 4
                            word = {'offset': 0, 'tag': 0, 'short': 0x80000004, 'long': 0x8000000c}[kind]
                            uc.mem_write(m.buffer + index * 4, struct.pack('<I', word))
                    m.uc.hook_add(UC_HOOK_MEM_WRITE, corrupt, begin=WRITE, end=WRITE + 3)
                m.call('fb_swap')
                self.assertEqual((m.get32('drawPage'), m.get32('visiblePage')), (1, 0))
                m.call('fb_swap')
                self.assertEqual(len(m.presented), 1)
                self.assertEqual(len(m.writes), 1)
                self.assertEqual(len(m.log), 1)

    def test_stopped_timer_is_bounded(self):
        # No MMIO hooks: the timer and FULL status remain constant, and the
        # poll limit runs without millions of Python callbacks.
        m = Machine(self.image)
        m.uc.mem_map(0xfe00b000, 4096)
        m.uc.mem_map(0xfe003000, 4096)
        m.uc.mem_write(STATUS, struct.pack('<I', FULL))
        m.call_timeout_us = 180_000_000
        m.call_instruction_limit = 200_000_000
        self.assertEqual(m.call('mbox_call', 8), 0)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
