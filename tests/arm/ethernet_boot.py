"""Bring up the linked GENET Ethernet driver from pi4_drivers_start.

Usage: python ethernet_boot.py v3d0-no-assets.elf ethernet-disabled.elf -v
The first ELF is built with V3D=0 and no Wi-Fi assets, the second with V3D=0
ETHERNET=0. The real hardware adapter, property mailbox, GENET driver and
NetLink glue run; GENET with its PHY, the mailbox and the timer are modelled
at MMIO level and DMA uses emulator memory. This is not a Pi 4 test.
"""
import struct
import sys
import unittest

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs
from memory_management import Image, Machine, STATS

GENET, TIMER, MAILBOX = 0xfd580000, 0xfe003000, 0xfe00b000
MBOX_READ, MBOX_STATUS, MBOX_WRITE = 0xfe00b880, 0xfe00b898, 0xfe00b8a0
MAC = bytes.fromhex('dca632123456')
MDIO, UMAC_MAC0, UMAC_MAC1, UMAC_CMD = 0xe14, 0x80c, 0x810, 0x808
RX_DESC, RX_RING, RDMA, TX_DESC, TX_RING, TDMA = 0x2000, 0x3000, 0x3040, 0x4000, 0x5000, 0x5040
BUSY = 1 << 29


class Genet:
    """GENET v5 registers, ring 16 and a BCM54213PE answering on MDIO address 1."""

    def __init__(self, read, write):
        self.memory_read, self.memory_write = read, write
        self.regs, self.sent = {}, []
        self.cable = self.restarted = self.mdio_stuck = False
        self.busy = self.rx_prod = self.tx_cons = self.rx_slot = self.tx_slot = self.accesses = 0
        self.phy = {0: 0x1140, 2: 0x600d, 3: 0x84a2}

    def read(self, offset):
        self.accesses += 1
        if offset == 0: return 0x06000000
        if offset == MDIO:
            if self.mdio_stuck or self.busy:
                self.busy = max(0, self.busy - 1)
                return self.regs.get(MDIO, 0) | BUSY
            return self.regs.get(MDIO, 0) & ~BUSY
        if offset in (RDMA + 8, TDMA + 8): return 0 if self.regs.get(offset - 4, 0) & 1 else 1
        if offset == RX_RING + 8: return self.rx_prod
        if offset == TX_RING + 8: return self.tx_cons
        return self.regs.get(offset, 0)

    def write(self, offset, value):
        self.accesses += 1
        if offset == MDIO and value & BUSY and not self.mdio_stuck: return self.mdio(value)
        if offset == RX_RING + 8: self.rx_prod = value & 0xffff
        elif offset == TX_RING + 8: self.tx_cons = value & 0xffff
        else: self.regs[offset] = value
        if offset == TX_RING + 0xc and self.regs.get(TDMA + 4, 0) & 1: self.transmit()

    def mdio(self, command):
        address, reg, op = (command >> 21) & 31, (command >> 16) & 31, (command >> 26) & 3
        assert address == 1
        data = command & 0xffff
        if op == 2:
            linked = self.cable and self.restarted and self.phy[0] & 0x1000
            data = {1: 0x7949 | (0x24 if linked else 0), 5: 0x45e1, 10: 0x0c00}.get(reg, self.phy.get(reg, 0))
        elif reg == 0 and data & 0x8000:
            self.phy, self.restarted = {0: 0x1140, 2: 0x600d, 3: 0x84a2}, False
        elif reg == 0:
            self.phy[0], self.restarted = data & ~0x200, self.restarted or bool(data & 0x200)
        else:
            self.phy[reg] = data
        self.regs[MDIO] = (command & 0x1fff0000) | (data if op == 2 else 0) | BUSY
        self.busy = 1

    def ring(self, base): return (self.regs[base + 0x1c] + 1) // 3

    def descriptor(self, base):
        return self.regs[base + 4] | self.regs[base + 8] << 32

    def transmit(self):
        while self.tx_cons != self.regs[TX_RING + 0xc] & 0xffff:
            desc = TX_DESC + self.tx_slot * 12
            self.sent.append(self.memory_read(self.descriptor(desc), self.regs[desc] >> 16))
            self.tx_cons = (self.tx_cons + 1) & 0xffff
            self.tx_slot = (self.tx_slot + 1) % self.ring(TX_RING)

    def deliver(self, frame, flags=0x6000):
        desc = RX_DESC + self.rx_slot * 12
        self.memory_write(self.descriptor(desc), b'\0\0' + frame)
        self.regs[desc] = (len(frame) + 2) << 16 | flags
        self.rx_prod = (self.rx_prod + 1) & 0xffff
        self.rx_slot = (self.rx_slot + 1) % self.ring(RX_RING)


class Board(Machine):
    def __init__(self, image):
        super().__init__(image)
        self.sctlr = 5  # As after mmu_init: the driver's ownership check passes.
        self.log, self.clock, self.token, self.properties = [], 0, None, 0
        self.genet = Genet(lambda address, size: bytes(self.uc.mem_read(address, size)),
                           lambda address, data: self.uc.mem_write(address, data))
        for base, size in ((GENET, 0x10000), (TIMER, 0x1000), (MAILBOX, 0x1000)):
            self.uc.mem_map(base, size)
            self.uc.hook_add(UC_HOOK_MEM_READ, self.mmio_read, begin=base, end=base + size - 1)
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self.mmio_write, begin=base, end=base + size - 1)
        for name in ('uart_writeText', 'pi4_rng_start', 'sdio_init'):
            address = image.symbols[name]
            self.uc.hook_add(UC_HOOK_CODE, self.stub, user_data=name, begin=address, end=address)

    def stub(self, uc, address, size, name):
        assert name != 'sdio_init', 'radio started without assets'
        if name == 'uart_writeText':
            pointer = uc.reg_read(regs.UC_ARM64_REG_X0)
            self.log.append(bytes(uc.mem_read(pointer, 200)).split(b'\0', 1)[0].decode())
        uc.reg_write(regs.UC_ARM64_REG_X0, (-5) & 0xffffffffffffffff)  # RNG modelled in network.py.
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def mmio_read(self, uc, access, address, size, value, _):
        assert size == 4
        if address == TIMER + 4:
            self.clock = (self.clock + 50) & 0xffffffff
            result = self.clock
        elif address == MBOX_STATUS: result = 0 if self.token is not None else 0x40000000
        elif address == MBOX_READ: result, self.token = self.token, None
        elif GENET <= address < GENET + 0x10000: result = self.genet.read(address - GENET)
        else: raise AssertionError(f'unexpected read {address:#x}')
        uc.mem_write(address, struct.pack('<I', result))

    def mmio_write(self, uc, access, address, size, value, _):
        assert size == 4
        value &= 0xffffffff
        if GENET <= address < GENET + 0x10000: return self.genet.write(address - GENET, value)
        assert address == MBOX_WRITE and value & 15 == 8
        self.properties += 1
        words = list(struct.unpack('<8I', uc.mem_read(value & ~15, 32)))
        assert words[:5] == [32, 0, 0x00010003, 8, 0] and words[7] == 0
        # The firmware reports the 6-byte value length, not the buffer size.
        words[1], words[4] = 0x80000000, 0x80000006
        words[5:7] = struct.unpack('<2I', MAC + b'\0\0')
        uc.mem_write(value & ~15, struct.pack('<8I', *words))
        self.token = value

    def start(self):
        self.call('pi4_drivers_start')
        return ''.join(self.log)

    def driver(self, name, *args):
        return self.call(name, self.image.symbols['genet'], *args) & 0xffffffff


class EthernetBootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image, cls.disabled = Image(sys.argv[1]), Image(sys.argv[2])

    def test_present_controller_comes_up_without_a_cable(self):
        board = Board(self.image)
        log = board.start()
        self.assertIn('[OK] Ethernet ready (MAC dc:a6:32:12:34:56)', log)
        self.assertEqual(board.properties, 1)
        genet, regs_ = self.image.symbols['genet'], board.genet.regs
        self.assertEqual((regs_[UMAC_MAC0], regs_[UMAC_MAC1]), (0xdca63212, 0x3456))
        self.assertEqual(regs_[UMAC_CMD] & 3, 3)
        self.assertEqual((regs_[RDMA + 4], regs_[TDMA + 4]), (0x20001, 0x20001))
        # Identity DMA: descriptors hold the ARM physical address of the buffers.
        self.assertEqual((regs_[RX_DESC + 4], regs_[RX_DESC + 8]), (genet, 0))
        self.assertEqual(board.driver('genet_carrier', STATS), 0)

        board.genet.cable = True
        board.clock += 200000
        self.assertEqual(board.driver('genet_poll'), 1)
        self.assertEqual(board.driver('genet_carrier', STATS), 1)
        self.assertEqual(struct.unpack('<I', board.uc.mem_read(STATS, 4))[0], 1000)
        frame = MAC + bytes(range(6)) + b'\x08\x06' + bytes(range(28))
        board.uc.mem_write(STATS + 256, frame)
        self.assertEqual(board.driver('genet_send', STATS + 256, len(frame)), 1)
        self.assertEqual(board.genet.sent, [frame + bytes(60 - len(frame))])
        incoming = b'\xff' * 6 + bytes(range(100))
        board.genet.deliver(incoming)
        board.genet.deliver(incoming, flags=0x6002)  # CRC error: dropped
        self.assertEqual(board.driver('genet_poll'), 1)
        self.assertEqual(board.driver('genet_receive', STATS + 2048, 1514), len(incoming))
        self.assertEqual(bytes(board.uc.mem_read(STATS + 2048, len(incoming))), incoming)
        self.assertEqual(board.driver('genet_receive', STATS + 2048, 1514), 0)

    def test_failed_phy_bring_up_reports_and_continues(self):
        board = Board(self.image)
        board.genet.mdio_stuck = True
        log = board.start()
        self.assertIn('[INFO] Ethernet unavailable (-1)', log)
        self.assertNotIn('Ethernet ready', log)
        self.assertEqual(board.genet.regs.get(RDMA + 4), 0)

    def test_disabled_build_never_touches_the_controller(self):
        board = Board(self.disabled)
        log = board.start()
        self.assertIn('[INFO] Ethernet disabled at build time (ETHERNET=0)', log)
        self.assertEqual((board.genet.accesses, board.properties), (0, 0))


if __name__ == '__main__':
    if len(sys.argv) < 3: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
