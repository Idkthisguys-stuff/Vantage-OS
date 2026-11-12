"""Check Wi-Fi startup without assets, using a V3D=0 linked image.

Usage: python tests/arm/wifi_boot.py no-assets.elf wifi-disabled.elf -v
"""
import sys
import unittest

from unicorn import UC_HOOK_CODE
from unicorn import arm64_const as regs
from memory_management import Image, Machine, RETURN


def start(image):
    machine = Machine(image)
    messages, radio = [], []

    def text(uc, address, size, data):
        ptr = uc.reg_read(regs.UC_ARM64_REG_X0)
        messages.append(bytes(uc.mem_read(ptr, 256)).split(b'\0', 1)[0])
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    def sdio(uc, address, size, data):
        radio.append(True)
        uc.emu_stop()

    def rng(uc, address, size, data):
        # Entropy hardware is modeled in network.py; report it absent here.
        uc.reg_write(regs.UC_ARM64_REG_X0, (-5) & 0xffffffffffffffff)
        uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))

    machine.uc.mem_map(0xfe003000, 0x1000)  # Free-running timer read by the network clock.
    # GENET reads as absent (revision 0); the MMU counts as on so the probe runs.
    machine.uc.mem_map(0xfd580000, 0x10000)
    machine.sctlr = 5
    for name, callback in (('uart_writeText', text), ('sdio_init', sdio), ('pi4_rng_start', rng)):
        address = image.symbols[name]
        machine.uc.hook_add(UC_HOOK_CODE, callback, begin=address, end=address)
    machine.uc.reg_write(regs.UC_ARM64_REG_SP, image.symbols['__kernel_stack_top'])
    machine.uc.reg_write(regs.UC_ARM64_REG_X30, RETURN)
    machine.uc.emu_start(image.symbols['pi4_drivers_start'], RETURN, count=10000)
    return b'\n'.join(messages), radio


class WifiBootTests(unittest.TestCase):
    def test_absent_assets_skip_radio(self):
        log, radio = start(Image(sys.argv[1]))
        self.assertIn(b'Wi-Fi skipped: supply firmware', log)
        self.assertIn(b'Hardware RNG unavailable', log)
        self.assertIn(b'[INFO] Ethernet unavailable (no GENET v5 controller)', log)
        self.assertFalse(radio)

    def test_disabled_skips_radio(self):
        log, radio = start(Image(sys.argv[2]))
        self.assertIn(b'Wi-Fi disabled at build time', log)
        self.assertIn(b'[INFO] Ethernet unavailable (no GENET v5 controller)', log)
        self.assertFalse(radio)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
