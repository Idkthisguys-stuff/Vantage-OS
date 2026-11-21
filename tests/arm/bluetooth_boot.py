"""Exercise the linked Pi 4 Bluetooth startup using a PL011 and radio model.

Usage: python tests/arm/bluetooth_boot.py enabled.elf disabled.elf -v
Use V3D=0, ETHERNET=0, USB=0 and no Wi-Fi assets for both builds.
"""
import struct
import sys
import unittest
from collections import deque

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs
from memory_management import FRAMEBUFFER, Image, Machine

GPIO, PL011, TIMER, MAILBOX = 0xfe200000, 0xfe201000, 0xfe003000, 0xfe00b000
OUTPUT = 0x5400000


class Board(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.clock = self.token = 0
        self.gpio, self.uart, self.power, self.commands = {}, {}, [], []
        self.log, self.rx, self.tx = [], deque(), bytearray()
        self.respond = True
        for base in (GPIO, PL011, TIMER, MAILBOX):
            self.uc.mem_map(base, 0x1000)
            self.uc.hook_add(UC_HOOK_MEM_READ, self.read, begin=base, end=base + 0xfff)
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=base, end=base + 0xfff)
        for name in ('uart_writeText', 'uart_writeHex', 'pi4_rng_start'):
            pc = image.symbols[name]
            self.uc.hook_add(UC_HOOK_CODE, self.stub, user_data=name, begin=pc, end=pc)

    def stub(self, uc, address, size, name):
        if name == 'uart_writeText':
            ptr = uc.reg_read(regs.UC_ARM64_REG_X0)
            self.log.append(bytes(uc.mem_read(ptr, 120)).split(b'\0')[0].decode())
        if name == 'pi4_rng_start':
            uc.reg_write(regs.UC_ARM64_REG_X0, 0xfffffffffffffff9)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def read(self, uc, access, address, size, value, _):
        assert size == 4
        if address == TIMER + 4:
            self.clock += 100
            result = self.clock & 0xffffffff
        elif address == MAILBOX + 0x898:
            result = 0 if self.token else 0x40000000
        elif address == MAILBOX + 0x880:
            result, self.token = self.token, 0
        elif address == PL011:
            result = self.rx.popleft()
        elif address == PL011 + 0x18:
            result = 0 if self.rx else 0x10
        elif address in (PL011 + 0xfe0, PL011 + 0xfe4):
            result = 0x11 if address & 4 == 0 else 0x10
        elif GPIO <= address < GPIO + 0x1000:
            result = self.gpio.get(address - GPIO, 0)
        else:
            raise AssertionError(f'unexpected MMIO read {address:#x}')
        uc.mem_write(address, struct.pack('<I', result))

    def write(self, uc, access, address, size, value, _):
        assert size == 4
        if GPIO <= address < GPIO + 0x1000:
            self.gpio[address - GPIO] = value
        elif address == PL011:
            self.tx.append(value)
            if len(self.tx) >= 4 and len(self.tx) == 4 + self.tx[3]:
                kind, opcode, _ = struct.unpack('<BHB', self.tx[:4])
                assert kind == 1 and self.power[-1] == 1
                self.commands.append(opcode)
                self.tx.clear()
                if self.respond:
                    self.rx.extend(struct.pack('<BBBBHB', 4, 0x0e, 4, 1, opcode, 0))
        elif PL011 < address < PL011 + 0x1000:
            self.uart[address - PL011] = value
        elif address == MAILBOX + 0x8a0:
            self.tag(uc, value)
        else:
            raise AssertionError(f'unexpected MMIO write {address:#x}')

    def tag(self, uc, token):
        assert token & 15 == 8
        pointer = token & ~15
        words = list(struct.unpack('<8I', uc.mem_read(pointer, 32)))
        assert words[0] == 32 and words[3] == 8 and words[7] == 0
        words[1] = 0x80000000
        if words[2] == 0x30002:
            assert words[5] == 2
            words[4], words[6] = 0x80000008, 48000000
        else:
            assert words[2] == 0x38041 and words[5] == 128
            self.power.append(words[6])
            words[4], words[5] = 0, 0  # GPIO acknowledgement used by some Pi firmware.
        uc.mem_write(pointer, struct.pack('<8I', *words))
        self.token = token

    def start(self):
        self.call('mmu_init', FRAMEBUFFER, 0x200000)
        self.call('pi4_drivers_start')
        return ''.join(self.log)

    def status(self):
        self.call('pi4_bt_status', OUTPUT)
        return struct.unpack('<IIiI', self.uc.mem_read(OUTPUT, 16))

    def poll(self, times=1):
        for _ in range(times):
            self.call('pi4_drivers_poll')


class BluetoothBootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.enabled, cls.disabled = Image(sys.argv[1]), Image(sys.argv[2])

    def test_start_uses_uart0_and_radio_enable(self):
        board = Board(self.enabled)
        self.assertIn('Bluetooth HCI initialization started', board.start())
        self.assertEqual(board.power, [0, 1])
        self.assertEqual(board.gpio[0x0c] & 0xfff, 0xfff)
        self.assertEqual((board.uart[0x24], board.uart[0x28], board.uart[0x30]), (26, 3, 0xc301))
        board.poll(8)
        self.assertEqual(board.commands, [0x0c03, 0x0c01, 0x2001])
        self.assertEqual(board.status(), (2, 0, 0, 0))

    def test_scan_reads_an_advertisement_over_uart(self):
        board = Board(self.enabled)
        board.start()
        board.poll(8)
        self.assertEqual(board.call('pi4_bt_scan_start'), 1)
        board.poll(4)
        self.assertEqual(board.commands[-2:], [0x200b, 0x200c])

        address = bytes.fromhex('010203040506')
        name = b'Vantage Beacon'
        ad = bytes((len(name) + 1, 0x09)) + name
        report = bytes((2, 1, 0, 0)) + address + bytes((len(ad),)) + ad + bytes((200,))
        board.rx.extend(bytes((4, 0x3e, len(report))) + report)
        board.poll()
        self.assertEqual(board.status(), (3, 1, 0, 0))
        self.assertEqual(board.call('pi4_bt_device', 0, OUTPUT), 1)
        received = struct.unpack('<6sBBi32s', board.uc.mem_read(OUTPUT, 44))
        self.assertEqual(received[:4], (address, 0, 0, -56))
        self.assertEqual(received[4].rstrip(b'\0'), name)

        board.clock += 5_000_000
        board.poll(2)
        self.assertEqual(board.commands[-1], 0x200c)
        self.assertEqual(board.status(), (2, 1, 0, 0))

    def test_controller_timeout_reports_failure(self):
        board = Board(self.enabled)
        board.respond = False
        board.start()
        board.clock += 5_000_000
        board.poll()
        self.assertEqual(board.status(), (4, 0, -4, 0))

    def test_disabled_build_leaves_bluetooth_alone(self):
        board = Board(self.disabled)
        self.assertNotIn('Bluetooth', board.start())
        self.assertFalse(board.power or board.gpio or board.uart or board.commands)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
