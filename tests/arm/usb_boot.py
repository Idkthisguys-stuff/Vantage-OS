"""Exercise the linked Pi 4 USB probe and VL805 command ring.

Usage: python tests/arm/usb_boot.py usb-enabled.elf usb-disabled.elf -v
Both images use V3D=0, ETHERNET=0 and no Wi-Fi assets. The enabled kernel
must include USB 2.0 hub support for the boot keyboard and mouse case.
"""
import struct
import sys
import unittest
from collections import deque

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs

from memory_management import FRAMEBUFFER, STATS, Image, Machine

PCIE, XHCI, TIMER, MBOX = 0xfd500000, 0x600000000, 0xfe003000, 0xfe00b000
PCI_COMMAND, EP_COMMAND = 0x04, 0x8004
PCI_MEMORY, PCI_MASTER, PCI_INTX_DISABLE = 2, 4, 1 << 10
PCI_BAR, PCI_BDF = 0xf8000000, 1 << 20
INBOUND_LO, INBOUND_HI, PCIE_CONTROL, SW_INIT = 0x4034, 0x4038, 0x4008, 0x9210
PERST, BRIDGE_RESET = 1, 2
SHIFTED_DMA_BASE = 0x200000000

CAP_LENGTH, PORTS, MAX_SLOTS, SCRATCHPADS = 0x20, 4, 32, 4
USBCMD, USBSTS, PAGESIZE = (CAP_LENGTH + offset for offset in (0, 4, 8))
CRCR, DCBAAP, CONFIG = (CAP_LENGTH + offset for offset in (0x18, 0x30, 0x38))
PORTSC = tuple(CAP_LENGTH + 0x400 + port * 16 for port in range(PORTS))
RUNTIME, DOORBELL = 0x600, 0x800
IMAN, IMOD, ERSTSZ, ERSTBA, ERDP = (RUNTIME + offset for offset in (0x20, 0x24, 0x28, 0x30, 0x38))
RUN, RESET = 1, 2
HALTED, EVENT_INTERRUPT, NOT_READY = 1, 1 << 3, 1 << 11
PORT_POWER, EHB = 1 << 9, 1 << 3
MBOX_READ, MBOX_STATUS, MBOX_WRITE = 0x880, 0x898, 0x8a0
TRB = struct.Struct('<QII')
TRB_LINK, TRB_NO_OP, EVENT_COMMAND_COMPLETION, CC_SUCCESS = 6, 23, 33, 1

HW_OK, HW_TIMEOUT, HW_PROTOCOL = 1, -1, -2
STAGE_LINK, STAGE_READY, STAGE_NO_OP, STAGE_RUNNING = 2, 6, 10, 11


class Ring:
    def __init__(self, bus):
        self.base = self.next = bus
        self.cycle = 1

    def take(self, board):
        address = self.next
        trb = TRB.unpack(board.dma_read(address, TRB.size, 16))
        assert trb[2] & 1 == self.cycle, f'Unpublished TRB at {address:#x}: {trb}'
        if (trb[2] >> 10) & 63 == TRB_LINK:
            assert trb == (self.base, 0, TRB_LINK << 10 | 2 | self.cycle)
            self.next = address = self.base
            self.cycle ^= 1
            trb = TRB.unpack(board.dma_read(address, TRB.size, 16))
            assert trb[2] & 1 == self.cycle and (trb[2] >> 10) & 63 != TRB_LINK
        self.next += TRB.size
        return address, trb


class Board(Machine):
    def __init__(self, image, *, transport=False, dma_base=0, completion='success'):
        super().__init__(image)
        self.transport, self.dma_base, self.completion = transport, dma_base, completion
        self.log = []
        self.clock = self.host_reads = self.host_writes = self.xhci_reads = 0
        self.link = True
        self.host = {
            0: 0x271114e4, 0x8000: 0x34831106, 0x8008: 0x0c033001,
            INBOUND_LO: 18 if dma_base else 0, INBOUND_HI: dma_base >> 32,
        }
        # AC64, 64-byte contexts, per-port power control and Supported Protocol
        # capabilities with USB 3 on ports 1-2 and USB 2 on ports 3-4.
        self.capabilities = {
            0: 0x01000000 | CAP_LENGTH, 4: PORTS << 24 | 1 << 8 | MAX_SLOTS,
            8: SCRATCHPADS << 27 | 0x11, 0x10: (0x500 // 4) << 16 | 0x0d,
            0x14: DOORBELL, 0x18: RUNTIME,
            0x500: 0x03000402, 0x504: 0x20425355, 0x508: 0x0201, 0x50c: 0,
            0x510: 0x02000002, 0x514: 0x20425355, 0x518: 0x0203, 0x51c: 0,
        }
        self.reset_at = self.halt_at = self.mailbox_token = None
        self.resets = self.firmware_requests = self.acknowledgements = 0
        self.commands = []
        self.track_cache = False
        self.cache_ops = []
        self.ram = {}
        self.bss = image.symbols['__bss_start'], image.symbols['__bss_end']
        self.reset_xhci()
        for base, size in ((PCIE, 0xa000), (XHCI, 0x1000), (TIMER, 0x1000), (MBOX, 0x1000)):
            self.uc.mem_map(base, size)
            self.uc.hook_add(UC_HOOK_MEM_READ, self.read, begin=base, end=base + size - 1)
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=base, end=base + size - 1)
        # These unrelated startup drivers have their own hardware models.
        for name in ('uart_writeText', 'uart_writeHex', 'pi4_rng_start', 'pi4_bt_start'):
            if name in image.symbols:
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

    def privileged(self, uc, address, size, instruction):
        mnemonic, operands = instruction
        if self.track_cache and mnemonic == 'dc':
            operation, register = operands.split(', ')
            if operation in ('cvac', 'ivac', 'civac'):
                line = uc.reg_read(getattr(regs, 'UC_ARM64_REG_' + register.upper())) & ~63
                self.cache_ops.append((operation, line))
        if mnemonic == 'dsb':
            # Unicorn gives the CPU and external masters one coherent view.
            # Keep a second RAM view that changes only at cache maintenance.
            for operation, line in self.cache_ops:
                if operation == 'ivac':
                    if line in self.ram:
                        uc.mem_write(line, bytes(self.ram[line]))
                else:
                    self.ram[line] = uc.mem_read(line, 64)
            self.cache_ops.clear()
        super().privileged(uc, address, size, instruction)

    def shared_parts(self, address, size):
        assert self.bss[0] <= address < address + size <= self.bss[1], (
            f'Buffer outside kernel BSS: {address:#x}+{size:#x}')
        while size:
            line = address & ~63
            offset = address - line
            count = min(size, 64 - offset)
            assert line in self.ram, f'Unpublished cache line at {line:#x}'
            yield self.ram[line], offset, count
            address += count
            size -= count

    def shared_read(self, address, size):
        return b''.join(line[offset:offset + count]
                        for line, offset, count in self.shared_parts(address, size))

    def shared_write(self, address, data):
        start = 0
        for line, offset, count in self.shared_parts(address, len(data)):
            line[offset:offset + count] = data[start:start + count]
            start += count

    def inbound_base(self):
        lo, hi = self.host.get(INBOUND_LO, 0), self.host.get(INBOUND_HI, 0)
        size = lo & 31
        assert size in (17, 18), f'Invalid PCIe inbound size field {size}'
        assert self.host.get(PCIE_CONTROL, 0) >> 27 == size
        base = hi << 32 | (lo & ~31)
        assert base == self.dma_base
        return base

    def dma_address(self, bus, alignment):
        assert self.link_up() and all(self.host[reg] & PCI_MASTER for reg in (PCI_COMMAND, EP_COMMAND))
        assert bus and bus % alignment == 0, f'Misaligned DMA address {bus:#x}'
        return bus - self.inbound_base()

    def dma_read(self, bus, size, alignment=1):
        return self.shared_read(self.dma_address(bus, alignment), size)

    def dma_write(self, bus, data, alignment=1):
        self.shared_write(self.dma_address(bus, alignment), data)

    def link_up(self):
        return self.link and not self.host.get(SW_INIT, 0) & (BRIDGE_RESET | PERST)

    def pcie_read(self, offset):
        self.host_reads += 1
        result = self.host.get(offset, 0)
        if offset == 0x4068:
            return 0xb0 if self.link_up() else 0x80
        if 0x8000 <= offset < 0x9000:
            self.check_endpoint()
        if offset == 0x8010:
            return 0xfffff004 if result == 0xffffffff else result | 4
        return result

    def check_endpoint(self):
        assert self.link_up() and self.host.get(0x9000) == PCI_BDF
        assert self.host.get(0x18, 0) & 0xffffff == 0x010100

    def pcie_write(self, offset, value):
        self.host_writes += 1
        if 0x8000 <= offset < 0x9000:
            self.check_endpoint()
        if offset in (0x8010, 0x8014):
            assert not self.host.get(EP_COMMAND, 0) & PCI_MEMORY
        if offset in (PCI_COMMAND, EP_COMMAND):
            assert value & PCI_INTX_DISABLE
            if value & PCI_MASTER:
                assert self.transport and value & PCI_MEMORY and self.link_up()
                self.inbound_base()
        if offset == SW_INIT and value & PERST:
            assert self.xhci[USBSTS] & HALTED
        self.host[offset] = value

    def check_xhci_decode(self):
        assert self.link_up()
        for offset in (PCI_COMMAND, EP_COMMAND):
            assert self.host.get(offset, 0) & PCI_MEMORY
        assert self.host.get(0x8010, 0) & ~15 == PCI_BAR and self.host.get(0x8014) == 0
        assert (self.host[0x4010] << 32 | self.host[0x400c]) == PCI_BAR
        window = self.host[0x4070]
        start_mb = self.host[0x4080] << 12 | (window >> 4 & 0xfff)
        end_mb = self.host[0x4084] << 12 | (window >> 20 & 0xfff)
        assert start_mb << 20 == XHCI and end_mb >= start_mb
        bridge_window = self.host[0x20]
        assert (bridge_window & 0xfff0) << 16 == PCI_BAR
        assert (bridge_window & 0xfff00000) + 0x100000 - PCI_BAR >= 0x1000
        if self.transport:
            assert self.firmware_requests
        else:
            assert not any(self.host.get(reg, 0) & 31 for reg in (0x402c, INBOUND_LO, 0x403c))

    def reset_xhci(self):
        self.xhci = {
            USBCMD: 0, USBSTS: HALTED, PAGESIZE: 1, CONFIG: 0,
            CRCR: 0, CRCR + 4: 0, DCBAAP: 0, DCBAAP + 4: 0,
            IMAN: 0, IMOD: 0, ERSTSZ: 0, ERSTBA: 0, ERSTBA + 4: 0, ERDP: 0, ERDP + 4: 0,
            **dict.fromkeys(PORTSC, 0),
        }
        self.event_tail = 0
        self.event_cycle = 1
        self.event_acks = deque()

    def update_xhci(self):
        if self.halt_at is not None and self.clock >= self.halt_at:
            self.xhci[USBSTS] |= HALTED
            self.halt_at = None
        if self.reset_at is not None and self.clock >= self.reset_at:
            self.reset_xhci()
            self.resets += 1
            self.reset_at = None

    def xhci_read(self, offset):
        self.xhci_reads += 1
        self.check_xhci_decode()
        if not self.transport:
            assert offset in (0, 4)
        self.update_xhci()
        if offset in self.capabilities:
            return self.capabilities[offset]
        assert offset in self.xhci, f'Unexpected xHCI read {offset:#x}'
        return self.xhci[offset]

    def xhci_write(self, offset, value):
        assert self.transport
        self.check_xhci_decode()
        self.update_xhci()
        if offset == USBCMD:
            assert value in (0, RUN, RESET), f'Unexpected polled USBCMD {value:#x}'
            assert self.reset_at is None
            if value == RESET:
                assert self.xhci[USBSTS] & HALTED and not self.xhci[USBCMD] & RUN
                self.xhci[USBSTS] |= NOT_READY
                self.reset_at = self.clock + 300
            elif value == RUN:
                assert self.resets and self.xhci[USBSTS] == HALTED
                self.load_rings()
                self.xhci[USBSTS] &= ~HALTED
            elif self.xhci[USBCMD] & RUN:
                self.halt_at = self.clock + 300
        elif DOORBELL <= offset <= DOORBELL + 4 * self.xhci[CONFIG]:
            assert self.xhci[USBCMD] & RUN and not self.xhci[USBSTS] & HALTED
            self.doorbell_write((offset - DOORBELL) // 4, value)
            return
        elif offset == USBSTS:
            assert not value & ~(EVENT_INTERRUPT | 1 << 4)
            self.xhci[USBSTS] &= ~value
            return
        elif offset == IMAN:
            assert value in (0, 1), 'Polling must leave IMAN.IE clear'
            self.xhci[IMAN] &= ~value
            return
        elif offset == ERDP:
            busy = self.xhci[ERDP] & EHB & ~value
            value = (value & ~EHB) | busy
        elif offset == ERDP + 4:
            if self.event_acks:
                dequeue = value << 32 | self.xhci[ERDP]
                assert dequeue == self.event_acks.popleft(), f'Unexpected event dequeue {dequeue:#x}'
                self.acknowledgements += 1
        elif offset in PORTSC:
            self.port_write(offset, value)
            return
        else:
            assert offset in (CRCR, CRCR + 4, DCBAAP, DCBAAP + 4, CONFIG, IMOD,
                              ERSTSZ, ERSTBA, ERSTBA + 4), f'Unexpected xHCI write {offset:#x}'
            assert self.xhci[USBSTS] == HALTED
        self.xhci[offset] = value

    def port_write(self, offset, value):
        assert value == PORT_POWER
        self.xhci[offset] = value

    def xhci_address(self, offset):
        return self.xhci[offset + 4] << 32 | self.xhci[offset]

    def load_rings(self):
        slots = self.xhci[CONFIG]
        assert 0 < slots <= MAX_SLOTS
        assert self.xhci[ERSTSZ] == 1 and self.xhci[IMAN] == 0

        dcbaa = self.dma_address(self.xhci_address(DCBAAP), 64)
        scratch, *devices = struct.unpack(f'<{slots + 1}Q', self.shared_read(dcbaa, (slots + 1) * 8))
        assert not any(devices)
        scratch_array = self.dma_address(scratch, 64)
        scratchpads = struct.unpack(f'<{SCRATCHPADS}Q', self.shared_read(scratch_array, SCRATCHPADS * 8))

        segment = self.dma_address(self.xhci_address(ERSTBA), 64)
        event, entries, reserved = TRB.unpack(self.shared_read(segment, TRB.size))
        assert 16 <= entries <= 4096 and reserved == 0
        assert self.xhci_address(ERDP) == event
        event_ram = self.dma_address(event, 64)
        assert not any(self.shared_read(event_ram, entries * TRB.size))
        self.event_bus, self.event_entries = event, entries

        crcr = self.xhci_address(CRCR)
        assert crcr & 63 == 1, 'Command ring must start with cycle state 1'
        self.command_ring = Ring(crcr & ~63)
        command = self.dma_address(self.command_ring.base, 64)
        assert not any(self.shared_read(command, TRB.size))
        regions = [(dcbaa, (slots + 1) * 8), (scratch_array, SCRATCHPADS * 8),
                   (segment, TRB.size), (event_ram, entries * TRB.size), (command, TRB.size)]
        for bus in scratchpads:
            page = self.dma_address(bus, 4096)
            self.shared_read(page, 4096)
            regions.append((page, 4096))
        regions.sort()
        assert all(address + size <= next_address
                   for (address, size), (next_address, _) in zip(regions, regions[1:]))

    def doorbell_write(self, slot, target):
        assert slot == target == 0
        address, trb = self.command_ring.take(self)
        self.command(address, trb)

    def command(self, address, trb):
        assert trb == (0, 0, TRB_NO_OP << 10 | 1), f'Invalid No Op command TRB: {trb}'
        self.commands.append(self.clock)
        if self.completion == 'timeout':
            return
        pointer = address + (TRB.size if self.completion == 'wrong_pointer' else 0)
        self.complete(EVENT_COMMAND_COMPLETION, pointer)

    def complete(self, kind, pointer, *, slot=0, dci=0, code=CC_SUCCESS, residue=0):
        assert len(self.event_acks) < self.event_entries - 1, 'Event ring full'
        event = self.event_bus + self.event_tail * TRB.size
        control = slot << 24 | dci << 16 | kind << 10 | self.event_cycle
        self.dma_write(event, TRB.pack(pointer, code << 24 | residue, control), 16)
        self.event_tail += 1
        if self.event_tail == self.event_entries:
            self.event_tail = 0
            self.event_cycle ^= 1
        self.event_acks.append(self.event_bus + self.event_tail * TRB.size)
        self.xhci[ERDP] |= EHB
        self.xhci[USBSTS] |= EVENT_INTERRUPT
        self.xhci[IMAN] |= 1

    def mailbox_read(self, offset):
        assert self.transport
        if offset == MBOX_STATUS:
            return 0x40000000 if self.mailbox_token is None else 0
        assert offset == MBOX_READ and self.mailbox_token is not None
        token, self.mailbox_token = self.mailbox_token, None
        return token

    def mailbox_write(self, offset, token):
        assert self.transport and offset == MBOX_WRITE and token & 15 == 8
        assert self.link_up() and self.mailbox_token is None
        assert all(self.host[reg] == PCI_INTX_DISABLE | PCI_MEMORY | PCI_MASTER
                   for reg in (PCI_COMMAND, EP_COMMAND))
        self.inbound_base()
        address = token & ~15
        request = struct.unpack('<7I', self.shared_read(address, 28))
        assert request == (28, 0, 0x00030058, 4, 0, PCI_BDF, 0), request
        self.shared_write(address + 4, struct.pack('<4I', 0x80000000, request[2], 4, 0x80000004))
        self.mailbox_token = token
        self.firmware_requests += 1

    def read(self, uc, access, address, size, value, _):
        assert size == 4 and address % 4 == 0
        if address == TIMER + 4:
            self.clock += 100
            result = self.clock
        elif PCIE <= address < PCIE + 0xa000:
            result = self.pcie_read(address - PCIE)
        elif XHCI <= address < XHCI + 0x1000:
            result = self.xhci_read(address - XHCI)
        else:
            assert MBOX <= address < MBOX + 0x1000
            result = self.mailbox_read(address - MBOX)
        uc.mem_write(address, struct.pack('<I', result))

    def write(self, uc, access, address, size, value, _):
        assert size == 4 and address % 4 == 0
        value &= 0xffffffff
        if PCIE <= address < PCIE + 0xa000:
            self.pcie_write(address - PCIE, value)
        elif XHCI <= address < XHCI + 0x1000:
            self.xhci_write(address - XHCI, value)
        else:
            assert MBOX <= address < MBOX + 0x1000
            self.mailbox_write(address - MBOX, value)

    def initialize(self):
        self.call('mmu_init', FRAMEBUFFER, 0x200000)
        table = self.call('mmu_kernel_level1')
        self.aperture = struct.unpack('<Q', self.uc.mem_read(table + 8 * (XHCI >> 30), 8))[0]
        self.track_cache = True

    def controller_call(self, function):
        ops = self.call('pi4_hardware_ops')
        self.uc.mem_write(STATS, b'\xa5' * 16)
        result = self.call(function, ops, STATS)
        return struct.unpack('<i', struct.pack('<I', result & 0xffffffff))[0], struct.unpack(
            '<4I', self.uc.mem_read(STATS, 16))


# xHCI PORTSC and USB 2.0 hub-class port bits use different encodings.
PORT_CONNECTED, PORT_ENABLED, PORT_RESET = 1, 2, 1 << 4
PORT_SPEED_SHIFT, PORT_CHANGE = 10, 1 << 17
PORT_RESET_CHANGE = 1 << 21
HUB_POWER, HUB_LOW_SPEED = 1 << 8, 1 << 9
C_CONNECTION, C_RESET = 1, 1 << 4
EVENT_TRANSFER, EVENT_PORT_CHANGE = 32, 34
CC_SHORT = 13
ROOT_PORT = 3
HUB_TTT = 2


class UsbDevice:
    def __init__(self, speed, endpoint, packet, protocol=0):
        self.speed, self.endpoint, self.packet = speed, endpoint, packet
        self.dci = 2 * endpoint + 1
        self.ep0_packet = 64 if speed == 3 else 8
        hub = speed == 3
        dev_class = (9, 0, 1) if hub else (0, 0, 0)
        self.descriptor = bytes((18, 1, 0, 2, *dev_class, self.ep0_packet,
                                 0x34, 0x12, 0x00, endpoint, 0x00, 1, 0, 0, 0, 1))
        interface = bytes((9, 4, 0, 0, 1, 9, 0, 0, 0) if hub else
                          (9, 4, 0, 0, 1, 3, 1, protocol, 0))
        if not hub:
            interface += bytes((9, 0x21, 0x11, 1, 0, 1, 0x22, 63 if protocol == 1 else 50, 0))
        self.interval = {1: 8, 2: 16, 3: 4}[speed]
        endpoint_desc = bytes((7, 5, 0x80 | endpoint, 3, packet, 0, self.interval))
        length = 9 + len(interface) + len(endpoint_desc)
        self.config = bytes((9, 2, length, 0, 1, 1, 0, 0xc0 if hub else 0x80, 0 if hub else 25))
        self.config += interface + endpoint_desc


class HubPort:
    def __init__(self, device):
        self.device = device
        self.status = self.change = 0
        self.connected_at = self.power_at = self.reset_at = self.reset_done_at = None

    def connect(self, clock, connected):
        if connected:
            self.status |= PORT_CONNECTED
            self.connected_at = clock
        else:
            self.status &= ~(PORT_CONNECTED | PORT_ENABLED | PORT_RESET)
            self.reset_at = None
        self.change |= C_CONNECTION

    def update(self, clock):
        if self.reset_at is not None and clock >= self.reset_at:
            self.status = self.status & ~PORT_RESET | PORT_ENABLED
            self.change |= C_RESET
            self.reset_done_at, self.reset_at = self.reset_at, None

    def control(self, clock, setup):
        self.update(clock)
        kind, request, value, _, length = setup
        if kind == 0xa3 and request == 0:
            assert (value, length) == (0, 4)
            assert self.power_at is not None and clock >= self.power_at + 20_000
            status = self.status
            if status & PORT_ENABLED and self.device.speed == 2:
                status |= HUB_LOW_SPEED
            return struct.pack('<HH', status, self.change)
        assert kind == 0x23 and length == 0, setup
        if (request, value) == (3, 8):  # SET_FEATURE(PORT_POWER)
            self.status |= HUB_POWER
            self.power_at = clock
        elif (request, value) == (3, 4):  # SET_FEATURE(PORT_RESET)
            assert self.status & HUB_POWER and self.status & PORT_CONNECTED
            assert self.reset_at is None and self.change == 0
            assert clock >= max(self.power_at + 20_000, self.connected_at) + 100_000
            self.status = self.status & ~PORT_ENABLED | PORT_RESET
            self.reset_at = clock + 20_000
        else:
            assert request == 1 and 16 <= value <= 20, setup  # CLEAR_FEATURE(C_PORT_*)
            self.change &= ~(1 << (value - 16))
        return b''


class Slot:
    def __init__(self):
        self.device = None
        self.rings = {}
        self.pending = None
        self.configured = self.boot_protocol = False


class HubBoard(Board):
    """A single-TT high-speed hub with a full-speed keyboard and low-speed mouse."""

    def __init__(self, image):
        super().__init__(image, transport=True, dma_base=SHIFTED_DMA_BASE)
        self.hub = UsbDevice(speed=3, endpoint=1, packet=1)
        self.ports = {1: HubPort(UsbDevice(1, 2, 8, protocol=1)),
                      2: HubPort(UsbDevice(2, 3, 3, protocol=2))}
        self.hub_slot = None
        self.slots = {}
        self.root_reset_at = None
        for name in ('uart_read_byte', 'i2c_read_reg'):
            address = image.symbols[name]
            self.uc.hook_add(UC_HOOK_CODE, self.stub, user_data=name, begin=address, end=address)

    def stub(self, uc, address, size, name):
        if name in ('uart_read_byte', 'i2c_read_reg'):
            uc.reg_write(regs.UC_ARM64_REG_X0, 0)
            uc.reg_write(regs.UC_ARM64_REG_PC, uc.reg_read(regs.UC_ARM64_REG_X30))
        else:
            super().stub(uc, address, size, name)

    def root_change(self):
        self.xhci[USBSTS] |= 1 << 4
        self.complete(EVENT_PORT_CHANGE, ROOT_PORT << 24)

    def port_write(self, offset, value):
        if offset != PORTSC[ROOT_PORT - 1]:
            return super().port_write(offset, value)
        assert value & PORT_POWER and not value & ~(PORT_POWER | PORT_RESET | (0x7f << 17))
        status = self.xhci[offset]
        status &= ~(value & (0x7f << 17))
        if not status & PORT_POWER:
            status |= PORT_POWER | PORT_CONNECTED | PORT_CHANGE
            self.root_change()
        if value & PORT_RESET:
            assert status & PORT_CONNECTED and self.root_reset_at is None
            status = status & ~PORT_ENABLED | PORT_RESET
            self.root_reset_at = self.clock + 50_000
        self.xhci[offset] = status

    def update_xhci(self):
        super().update_xhci()
        if self.root_reset_at is not None and self.clock >= self.root_reset_at:
            offset = PORTSC[ROOT_PORT - 1]
            self.xhci[offset] = (self.xhci[offset] & ~PORT_RESET |
                                 PORT_ENABLED | PORT_RESET_CHANGE | (3 << PORT_SPEED_SHIFT))
            self.root_reset_at = None
            self.root_change()

    def command(self, address, trb):
        parameter, status, control = trb
        kind, slot_id = (control >> 10) & 63, control >> 24
        if kind == TRB_NO_OP:
            return super().command(address, trb)
        assert self.commands and status == 0 and control & 0x3fe == 0
        assert control & 0x00ff0000 == 0, f'Unexpected command flags {control:#x}'
        if kind == 9:  # Enable Slot. Choose the highest free ID.
            assert slot_id == parameter == 0
            slot_id = max(set(range(1, self.xhci[CONFIG] + 1)) - self.slots.keys())
            self.slots[slot_id] = Slot()
        elif kind == 10:  # Disable Slot.
            assert parameter == 0 and self.slots[slot_id].device is not self.hub
            del self.slots[slot_id]
        elif kind in (11, 12):  # Address Device or Configure Endpoint.
            self.context_command(slot_id, kind, parameter)
        else:
            raise AssertionError(f'Unexpected command type {kind} for slot {slot_id}')
        self.complete(EVENT_COMMAND_COMPLETION, address, slot=slot_id)

    def context_command(self, slot_id, kind, pointer):
        slot = self.slots[slot_id]
        # The fixture advertises CSZ=1. The first context is input control, then
        # slot, then the endpoint contexts indexed by Device Context Index.
        drop, add = struct.unpack('<II', self.dma_read(pointer, 8, 64))
        dw0, dw1, dw2 = struct.unpack('<III', self.dma_read(pointer + 64, 12))
        route, speed, entries = dw0 & 0xfffff, dw0 >> 20 & 15, dw0 >> 27
        assert drop == 0 and dw1 >> 16 & 0xff == ROOT_PORT
        if route:
            port = self.ports[route]
            assert self.slots[self.hub_slot].configured
            assert port.status & PORT_ENABLED and not port.change & C_RESET
            assert self.clock >= port.reset_done_at + 10_000
            device = port.device
            tt = self.hub_slot | route << 8
        else:
            assert self.xhci[PORTSC[ROOT_PORT - 1]] & PORT_ENABLED
            device, tt = self.hub, 0
        hub_fields = kind == 12 and device is self.hub
        assert speed == device.speed and dw0 >> 25 & 3 == (2 if hub_fields else 0)
        assert dw1 >> 24 == (len(self.ports) if hub_fields else 0)
        assert dw2 & 0x3ffff == tt | ((HUB_TTT << 16) if hub_fields else 0)
        dci = 1 if kind == 11 else device.dci
        assert entries == dci and add == 1 | (1 << dci)
        ep0, ep1, dequeue, average = struct.unpack('<IIQI', self.dma_read(pointer + 64 * (dci + 1), 20))
        packet = device.ep0_packet if dci == 1 else device.packet
        ep_type = 4 if dci == 1 else 7
        esit = {1: 6, 2: 7, 3: 3}[speed]
        interval = 0 if dci == 1 else esit
        assert ep0 == interval << 16
        assert ep1 == packet << 16 | ep_type << 3 | 6
        assert dequeue & 63 == 1 and average == (8 if dci == 1 else (packet << 16 | packet))
        assert not any(self.dma_read(dequeue & ~63, TRB.size, 64))
        output, = struct.unpack('<Q', self.dma_read(self.xhci_address(DCBAAP) + 8 * slot_id, 8, 8))
        if kind == 11:
            assert slot.device is None and not any(self.dma_read(output, 32 * 64, 64))
            slot.device = device
            if device is self.hub:
                assert self.hub_slot is None
                self.hub_slot = slot_id
        else:
            assert slot.device is device and not slot.configured
        slot.rings[dci] = Ring(dequeue & ~63)

        # Publish the xHC-owned slot and endpoint state in the Output Context.
        slot_state = (2 if kind == 11 else 3) << 27 | slot_id
        self.dma_write(output, struct.pack('<4I', dw0, dw1, dw2, slot_state), 64)
        self.dma_write(output + 64 * dci, struct.pack('<IIQI', ep0 | 1, ep1, dequeue, average), 64)

    def doorbell_write(self, slot_id, target):
        if slot_id == 0:
            return super().doorbell_write(slot_id, target)
        slot = self.slots[slot_id]
        ring = slot.rings[target]
        if target == 1:
            self.control(slot_id, slot, ring)
        else:
            assert slot.configured and (slot.device is self.hub or slot.boot_protocol)
            assert slot.pending is None and target == slot.device.dci
            address, (buffer, length, control) = ring.take(self)
            assert length == slot.device.packet and control == 1 << 10 | 1 << 5 | ring.cycle
            self.dma_read(buffer, length)
            slot.pending = (address, buffer)

    def control(self, slot_id, slot, ring):
        _, (parameter, size, control) = ring.take(self)
        setup = struct.unpack('<BBHHH', struct.pack('<Q', parameter))
        kind, _, _, _, length = setup
        assert not length or kind & 0x80, setup
        assert size == 8 and control == 2 << 10 | 1 << 6 | (3 << 16 if length else 0) | ring.cycle
        response = self.request(slot, setup)
        assert len(response) <= length
        if length:
            address, (buffer, count, control) = ring.take(self)
            assert count == length and control == 3 << 10 | 1 << 16 | 1 << 2 | ring.cycle
            assert (buffer & 0xffff) + length <= 0x10000
            self.dma_write(buffer, response)
            if len(response) < length:
                self.complete(EVENT_TRANSFER, address, slot=slot_id, dci=1,
                              code=CC_SHORT, residue=length - len(response))
        address, (parameter, size, control) = ring.take(self)
        assert parameter == size == 0
        assert control == 4 << 10 | 1 << 5 | (1 << 16 if not length else 0) | ring.cycle
        self.complete(EVENT_TRANSFER, address, slot=slot_id, dci=1)

    def request(self, slot, setup):
        kind, request, value, index, length = setup
        device = slot.device
        if kind == 0x80 and request == 6 and index == 0:
            descriptor = {0x100: device.descriptor, 0x200: device.config}[value]
            return descriptor[:length]
        if setup == (0, 9, 1, 0, 0):
            assert device.dci in slot.rings and not slot.configured
            slot.configured = True
        elif device is self.hub:
            if (kind, request, value, index) == (0xa0, 6, 0x2900, 0):
                # This hub implements descriptor reads in the Addressed state.
                descriptor = bytes((9, 0x29, 2, 1 | (HUB_TTT << 5), 0, 10, 0, 0, 0xff))
                return descriptor[:length]
            assert slot.configured, setup
            if setup == (0xa0, 0, 0, 0, 4):
                return bytes(4)
            return self.ports[index].control(self.clock, setup)
        elif setup == (0x21, 11, 0, 0, 0):  # SET_PROTOCOL(Boot)
            assert slot.configured
            slot.boot_protocol = True
        else:
            assert setup == (0x21, 10, 0, 0, 0) and slot.boot_protocol, setup  # SET_IDLE
        return b''

    def deliver_report(self, slot_id, report):
        slot = self.slots[slot_id]
        address, buffer = slot.pending
        assert len(report) == slot.device.packet
        self.dma_write(buffer, report)
        slot.pending = None
        self.complete(EVENT_TRANSFER, address, slot=slot_id, dci=slot.device.dci)

    def pending_child(self, port):
        return next((id_ for id_, slot in self.slots.items()
                     if slot.device is self.ports[port].device and slot.pending), None)

    def service(self):
        self.clock += 8_000
        status = 0
        for port, state in self.ports.items():
            state.update(self.clock)
            if state.change and state.status & HUB_POWER:
                status |= 1 << port
        hub = self.slots.get(self.hub_slot)
        if status and hub and hub.pending:
            self.deliver_report(self.hub_slot, bytes((status,)))
        self.call('event_service')


class UsbBootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.enabled, cls.disabled = Image(sys.argv[1]), Image(sys.argv[2])

    def assert_transport_stopped(self, board):
        self.assertEqual(board.xhci[USBCMD], 0)
        self.assertEqual(board.xhci[USBSTS], HALTED)
        self.assertEqual(board.host[SW_INIT] & (BRIDGE_RESET | PERST), PERST)
        self.assertEqual(board.host[PCI_COMMAND], PCI_INTX_DISABLE)
        self.assertEqual(board.host[EP_COMMAND], PCI_INTX_DISABLE)
        self.assertEqual(board.host[INBOUND_LO] & 31, 0)
        self.assertEqual(board.call('pi4_usb_port_count'), 0)

    def test_probe_reports_controller(self):
        board = Board(self.enabled, dma_base=SHIFTED_DMA_BASE)
        board.initialize()
        result, (stage, _, version, ports) = board.controller_call('pi4_usb_probe')
        self.assertEqual((result, stage, version, ports), (HW_OK, STAGE_READY, 0x100, PORTS))
        self.assertEqual(board.aperture & 3, 3)
        self.assertEqual(board.xhci_reads, 2)

    def test_failed_link_leaves_controller_in_reset(self):
        board = Board(self.enabled)
        board.link = False
        board.initialize()
        self.assertEqual(board.controller_call('pi4_usb_probe'), (HW_TIMEOUT, (STAGE_LINK, 0x80, 0, 0)))
        self.assertEqual(board.host[SW_INIT], PERST)
        self.assertEqual(board.xhci_reads, 0)

    def test_disabled_kernel_leaves_pcie_untouched(self):
        board = Board(self.disabled)
        board.initialize()
        board.call('pi4_drivers_start')
        self.assertNotIn('VL805', ''.join(board.log))
        self.assertEqual((board.aperture, board.host_reads, board.host_writes, board.xhci_reads), (0, 0, 0, 0))

    def test_start_completes_no_op_and_stop_disables_dma(self):
        for dma_base in (0, SHIFTED_DMA_BASE):
            with self.subTest(dma_base=hex(dma_base)):
                board = Board(self.enabled, transport=True, dma_base=dma_base)
                board.initialize()
                self.assertEqual(board.controller_call('pi4_usb_start'),
                                 (HW_OK, (STAGE_RUNNING, CC_SUCCESS, 0x100, PORTS)))
                self.assertEqual((board.firmware_requests, board.resets, len(board.commands)), (1, 1, 1))
                self.assertEqual(board.acknowledgements, 1)
                self.assertEqual((board.xhci[USBSTS], board.xhci[IMAN]), (0, 0))
                self.assertEqual(board.call('pi4_usb_port_count'), PORTS)
                self.assertEqual([board.xhci[port] for port in PORTSC], [0, 0, PORT_POWER, PORT_POWER])
                self.assertEqual(board.call('pi4_usb_stop'), HW_OK)
                self.assertEqual(board.resets, 2)
                self.assert_transport_stopped(board)

    def test_no_op_timeout_resets_and_can_restart(self):
        board = Board(self.enabled, transport=True, completion='timeout')
        board.initialize()
        self.assertEqual(board.controller_call('pi4_usb_start'),
                         (HW_TIMEOUT, (STAGE_NO_OP, 0, 0x100, PORTS)))
        self.assertEqual((board.firmware_requests, board.resets, len(board.commands)), (1, 2, 1))
        self.assertGreaterEqual(board.clock - board.commands[0], 100_000)
        self.assertEqual(board.acknowledgements, 0)
        self.assert_transport_stopped(board)

        board.completion = 'success'
        self.assertEqual(board.controller_call('pi4_usb_start'),
                         (HW_OK, (STAGE_RUNNING, CC_SUCCESS, 0x100, PORTS)))
        self.assertEqual((board.firmware_requests, board.resets, len(board.commands)), (2, 3, 2))
        self.assertEqual(board.acknowledgements, 1)
        self.assertEqual(board.call('pi4_usb_stop'), HW_OK)
        self.assert_transport_stopped(board)

    def test_no_op_rejects_completion_for_wrong_trb(self):
        board = Board(self.enabled, transport=True, completion='wrong_pointer')
        board.initialize()
        result, (stage, _, version, ports) = board.controller_call('pi4_usb_start')
        self.assertEqual((result, stage, version, ports), (HW_PROTOCOL, STAGE_NO_OP, 0x100, PORTS))
        self.assertEqual((board.resets, len(board.commands)), (2, 1))
        self.assert_transport_stopped(board)

    def service_until(self, board, condition, description):
        for _ in range(160):
            board.service()
            if condition():
                return
        self.fail(description)

    def test_boot_input_behind_usb2_hub(self):
        board = HubBoard(self.enabled)
        board.ports[1].connect(board.clock, True)
        board.initialize()
        board.call('pi4_drivers_start')
        self.assertIn('[OK] VL805 xHCI running:', ''.join(board.log))

        self.service_until(board, lambda: board.pending_child(1), 'Full-speed keyboard never polled')
        keyboard = board.pending_child(1)
        self.assertFalse(board.pending_child(2))
        board.ports[2].connect(board.clock, True)
        self.service_until(board, lambda: board.pending_child(2), 'Low-speed mouse never polled')
        mouse = board.pending_child(2)
        self.assertEqual(len({board.hub_slot, keyboard, mouse}), 3)

        # Shift+A and a left click with signed relative motion. Complete both
        # interrupt transfers before the same kernel event-service tick.
        board.deliver_report(keyboard, bytes((2, 0, 4, 0, 0, 0, 0, 0)))
        board.deliver_report(mouse, bytes((1, 6, 0xfc)))
        board.service()
        self.assertEqual(board.call('event_read_key', STATS), 1)
        self.assertEqual(struct.unpack('<II', board.uc.mem_read(STATS, 8)), (1, ord('A')))
        self.assertEqual(board.call('event_read_touch', STATS), 1)
        self.assertEqual(struct.unpack('<IiiI', board.uc.mem_read(STATS, 16)), (1, 246, 396, 10))
        self.assertEqual(board.call('event_read_key', STATS), 0)
        self.assertEqual(board.call('event_read_touch', STATS), 0)

        board.ports[2].connect(board.clock, False)
        self.service_until(board, lambda: mouse not in board.slots, 'Detached mouse slot was not disabled')
        self.assertEqual(board.call('event_read_touch', STATS), 1)
        self.assertEqual(struct.unpack('<IiiI', board.uc.mem_read(STATS, 16)), (3, -1, -1, 10))
        self.assertEqual(board.pending_child(1), keyboard)
        board.deliver_report(keyboard, bytes((0, 0, 5, 0, 0, 0, 0, 0)))
        board.service()
        self.assertEqual(board.call('event_read_key', STATS), 1)
        self.assertEqual(struct.unpack('<II', board.uc.mem_read(STATS, 8)), (1, ord('b')))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
