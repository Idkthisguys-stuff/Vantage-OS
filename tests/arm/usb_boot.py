"""Exercise the linked USB probe with modelled PCIe, xHCI and VideoCore.

Usage: python tests/arm/usb_boot.py usb-enabled.elf usb-disabled.elf -v
Both images use V3D=0, ETHERNET=0 and no Wi-Fi assets.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs
from memory_management import FRAMEBUFFER, Image, Machine

PCIE, XHCI, TIMER, MAILBOX = 0xfd500000, 0x600000000, 0xfe003000, 0xfe00b000


class Board(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.log = []
        self.clock = self.token = self.host_reads = self.xhci_reads = 0
        self.link = True
        self.host = {0: 0x271114e4, 0x8000: 0x34831106, 0x8008: 0x0c033001}
        for base, size in ((PCIE, 0xa000), (XHCI, 0x1000), (TIMER, 0x1000), (MAILBOX, 0x1000)):
            self.uc.mem_map(base, size)
            self.uc.hook_add(UC_HOOK_MEM_READ, self.read, begin=base, end=base + size - 1)
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=base, end=base + size - 1)
        for name in ('uart_writeText', 'uart_writeHex', 'pi4_rng_start'):
            address = image.symbols[name]
            self.uc.hook_add(UC_HOOK_CODE, self.stub, user_data=name, begin=address, end=address)

    def stub(self, uc, address, size, name):
        arg = uc.reg_read(regs.UC_ARM64_REG_X0)
        if name == 'uart_writeText':
            self.log.append(bytes(uc.mem_read(arg, 180)).split(b'\0', 1)[0].decode())
        elif name == 'uart_writeHex':
            self.log.append(f'{arg:#x}')
        else:
            uc.reg_write(regs.UC_ARM64_REG_X0, (-5) & 0xffffffffffffffff)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def read(self, uc, access, address, size, value, _):
        assert size == 4
        if address == TIMER + 4:
            self.clock += 100
            result = self.clock
        elif address == MAILBOX + 0x898:
            result = 0 if self.token else 0x40000000
        elif address == MAILBOX + 0x880:
            result, self.token = self.token, 0
        elif PCIE <= address < PCIE + 0xa000:
            self.host_reads += 1
            offset = address - PCIE
            result = self.host.get(offset, 0)
            if offset == 0x4068:
                result = 0xb0 if self.link and not self.host.get(0x9210, 0) & 1 else 0x80
            elif offset == 0x8010:
                result = 0xfffff004 if result == 0xffffffff else result | 4
        else:
            assert address in (XHCI, XHCI + 4)
            assert self.host[0x8004] == 0x402 and self.host[0x8010] == 0xf8000000
            self.xhci_reads += 1
            result = 0x01000020 if address == XHCI else 0x06000020
        uc.mem_write(address, struct.pack('<I', result))

    def write(self, uc, access, address, size, value, _):
        assert size == 4
        value &= 0xffffffff
        if PCIE <= address < PCIE + 0xa000:
            offset = address - PCIE
            if offset in (4, 0x8004):
                assert not value & 4
            self.host[offset] = value
            return
        assert address == MAILBOX + 0x8a0 and value & 15 == 8
        physical = value & ~15
        words = list(struct.unpack('<7I', uc.mem_read(physical, 28)))
        assert words == [28, 0, 0x30058, 4, 0, 0x100000, 0]
        words[1], words[4] = 0x80000000, 0x80000004
        uc.mem_write(physical, struct.pack('<7I', *words))
        self.token = value

    def start(self):
        self.call('mmu_init', FRAMEBUFFER, 0x200000)
        table = self.call('mmu_kernel_level1')
        self.aperture = struct.unpack('<Q', self.uc.mem_read(table + 8 * (XHCI >> 30), 8))[0]
        self.call('pi4_drivers_start')
        return ''.join(self.log)


class UsbBootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.enabled, cls.disabled = Image(sys.argv[1]), Image(sys.argv[2])

    def test_probe_reports_controller(self):
        board = Board(self.enabled)
        self.assertIn('[OK] VL805 xHCI PCIe probe: version 0x100, logical ports 0x6', board.start())
        self.assertEqual(board.aperture & 3, 3)
        self.assertEqual(board.xhci_reads, 2)

    def test_failed_link_leaves_controller_in_reset(self):
        board = Board(self.enabled)
        board.link = False
        self.assertIn('VL805 xHCI probe failed at link (-1)', board.start())
        self.assertEqual(board.host[0x9210], 1)
        self.assertEqual(board.xhci_reads, 0)

    def test_disabled_kernel_leaves_pcie_untouched(self):
        board = Board(self.disabled)
        self.assertNotIn('VL805', board.start())
        self.assertEqual((board.aperture, board.host_reads, board.xhci_reads), (0, 0, 0))


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
