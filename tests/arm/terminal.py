"""Run the built-in terminal as an isolated EL0 process on the linked kernel.

Usage: python tests/arm/terminal.py path/to/kernel8.elf -v
Requires the same Unicorn/Capstone dependencies as memory_management.py.
The embedded program image is loaded by process_create and runs under its own
translation tables with real SVC entry; devices are mocked at the kernel's
driver functions. See cpu.py for what the CPU model does and does not cover.
"""
import struct
import sys
import unittest

from unicorn import arm64_const as regs

from cpu import COUNTER_HZ, CpuMachine, X, USER_BASE, USER_END, OK
from memory_management import Image, Machine

EVENT_WAIT = 14
STACK_BYTES = 16384


class TerminalMachine(CpuMachine):
    def __init__(self, image):
        super().__init__(image)
        self.keys, self.touches, self.rows, self.output = [], [], {}, bytearray()
        self.buttons = []
        self.app_clock_us = 0
        self.presentations = 0
        self.parked = False
        self.idle_timer_wakes = 0
        self.model_idle_timer = False
        self.lowest_user_sp = USER_END
        self.stub('canvas_text_columns', result=60)
        self.stub('canvas_text_rows', result=50)
        self.stub('key_read', self.key)
        self.stub('touch_read', self.touch)
        self.stub('canvas_text_row', self.text_row)
        self.stub('canvas_button', self.button)
        self.stub('canvas_present', self.present)
        self.stub('canvas_clear', lambda *_: self.rows.clear())
        self.stub('time_us', lambda *_: self.app_clock_us & 0xffffffff)
        self.stub('uart_write_bytes', lambda pointer, length, *_: self.output.extend(self.uc.mem_read(pointer, length)))
        for name in ('fb_wait_frame', 'pi4_drivers_poll', 'canvas_set_color'):
            self.stub(name)

    def key(self, pointer, *_):
        event = self.keys.pop(0) if self.keys else (0, 0)
        self.uc.mem_write(pointer, struct.pack('<2I', *event))
        return bool(event[0])

    def touch(self, pointer, *_):
        event = self.touches.pop(0) if self.touches else (0, 0, 0)
        event_type, x, y, *ids = event
        self.uc.mem_write(pointer, struct.pack('<IiiI', event_type, x, y, ids[0] if ids else 0))
        return bool(event_type)

    def text_row(self, pointer, *_):
        row, length, fg, bg, cursor, text = struct.unpack('<5I96s', self.uc.mem_read(pointer, 116))
        assert row < 50 and length == 60
        assert not USER_BASE <= pointer < USER_END, 'drawing received a user pointer'
        self.rows[row] = text[:length].decode('ascii').rstrip()
        return 1

    def button(self, pointer, *_):
        button = struct.unpack('<8I8s', self.uc.mem_read(pointer, 40))
        assert button[0] + button[2] <= 480 and button[1] + button[3] <= 800
        self.buttons.append(button)
        return 1

    def present(self, *_):
        self.presentations += 1

    def syscall(self, uc, address, size, _):
        if self.parked:
            self.parked = False   # Resume the wait syscall that ended the last step.
            return
        super().syscall(uc, address, size, _)
        self.lowest_user_sp = min(self.lowest_user_sp, uc.reg_read(regs.UC_ARM64_REG_SP_EL0))
        if uc.reg_read(X[0]) == EVENT_WAIT:
            self.parked = True
            self.cpu.idle = True
            uc.emu_stop()

    def wait(self, uc, address, size, _):
        if self.model_idle_timer and self.cpu.armed and not self.owners()[0]:
            # Advance the hardware clock to the armed deadline during WFE.
            ticks = self.cpu.timer_log[-2][1]
            self.clock_us += (ticks * 1000000 + COUNTER_HZ - 1) // COUNTER_HZ
            self.cpu.remaining = 0
            self.idle_timer_wakes += 1
        super().wait(uc, address, size, _)

    def launch(self, booted=False, terminal=True, service=False):
        """Boot the embedded shell and optionally open the terminal tile."""
        if not booted:
            self.boot(cores=1)
        start, end = self.image.symbols['terminal_image'], self.image.symbols['terminal_image_end']
        self.app = self.call('process_create', start, end - start, 1)
        assert self.app
        if service:
            self.start('task_run_with_service', self.image.symbols['event_service'], 8000)
        else:
            self.start('task_run')
        self.step()
        if terminal:
            self.tap(75, 66)
            self.step()

    def tap(self, x, y):
        self.touches.extend(((1, x, y), (3, x, y)))
        self.step()

    def step(self):
        """Finish the pending event wait and run one application step."""
        assert self.run(until=lambda: self.parked, cores=1, rounds=200000) is None, 'terminal exited'

    def command(self, text):
        self.keys.extend((1, ord(c)) for c in text)
        self.keys.append((2, 0))
        while self.keys: self.step()


class TerminalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.image = Image(sys.argv[1])

    def test_idle_timer_services_devices_and_wakes_home_on_input(self):
        m = TerminalMachine(self.image)
        m.model_idle_timer = True
        polls = []

        def poll(*_):
            polls.append(m.idle_timer_wakes)
            if m.idle_timer_wakes >= 2 and all(wake < 2 for wake in polls[:-1]):
                m.touches.extend(((1, 185, 66), (3, 185, 66)))
            return 0

        m.stub('pi4_drivers_poll', poll)
        m.launch(terminal=False, service=True)
        m.step()
        self.assertEqual(m.rows[7].strip(), 'Files')
        self.assertGreaterEqual(m.idle_timer_wakes, 2)
        self.assertIn(('irq', 1), {(kind, origin) for _, kind, origin, _ in m.entries})


    def test_embedded_program_is_a_separately_linked_user_image(self):
        start, end = self.image.symbols['terminal_image'], self.image.symbols['terminal_image_end']
        m = Machine(self.image)
        magic, entry, text, rodata, data, bss = struct.unpack('<6Q', m.uc.mem_read(start, 48))
        self.assertEqual(magic, 0x31676d4945544e56)
        self.assertEqual((text % 4096, rodata % 4096), (0, 0))
        self.assertEqual(text + rodata + data, end - start)
        self.assertTrue(USER_BASE + 48 <= entry < USER_BASE + text)
        self.assertGreater(bss, 4096)   # The terminal model lives in the process, not the kernel.
        # Application and syscall-wrapper code is no longer part of the kernel link.
        for name in ('user_task', 'terminal_app_step', 'terminal_init', 'sys_frame_wait', 'user_stack',
                     '__user_start', 'enter_user_space'):
            self.assertNotIn(name, self.image.symbols)

    def test_home_opens_terminal_and_returns_without_losing_input(self):
        m = TerminalMachine(self.image)
        m.launch(terminal=False)
        self.assertEqual(m.rows[1].strip(), 'Vantage OS')
        presentations = m.presentations
        m.tap(350, 66)
        self.assertEqual(m.presentations, presentations)
        m.tap(185, 66)
        self.assertEqual(m.rows[7].strip(), 'Files')
        self.assertIn('Storage unavailable', m.rows[12])
        m.tap(40, 50)
        m.tap(295, 66)
        self.assertEqual(m.rows[7].strip(), 'Network')
        self.assertIn('No IP address', m.rows[14])
        m.tap(40, 50)
        m.tap(75, 66)
        m.step()
        m.keys.append((1, ord('x')))
        m.step()
        self.assertEqual(m.rows[5], '> x')
        m.tap(40, 16)
        self.assertEqual(m.rows[1].strip(), 'Vantage OS')
        m.tap(75, 66)
        m.step()
        self.assertEqual(m.rows[5], '> x')

    def test_bluetooth_tile_opens_by_keyboard_when_unavailable(self):
        m = TerminalMachine(self.image)
        if 'pi4_bt_status' in self.image.symbols:
            m.stub('pi4_bt_status', lambda pointer, *_: m.uc.mem_write(pointer, bytes(16)))
        m.launch(terminal=False)
        m.keys.append((1, ord('4')))
        m.step()
        self.assertEqual(m.rows[7].strip(), 'Bluetooth')
        self.assertEqual(m.rows[9].strip(), 'Bluetooth unavailable')
        presentations = m.presentations
        m.keys.append((2, 0))
        m.step()
        m.tap(75, 200)
        self.assertEqual(m.presentations, presentations)
        m.keys.append((6, 0))
        m.step()
        self.assertEqual(m.rows[1].strip(), 'Vantage OS')

    def test_bluetooth_scan_refreshes_device_details_and_status(self):
        if 'pi4_bt_status' not in self.image.symbols:
            self.skipTest('requires a Bluetooth-enabled kernel')
        m = TerminalMachine(self.image)
        state, scan_result = 1, -2  # BT_BOOTING, BT_BUSY
        devices = []

        def status(pointer, *_):
            assert not USER_BASE <= pointer < USER_END
            m.uc.mem_write(pointer, struct.pack('<2IiI', state, len(devices), 0, 0))

        def scan(*_):
            nonlocal state
            if scan_result == 1:
                state = 3  # BT_SCANNING
            return scan_result

        def device(index, pointer, *_):
            assert not USER_BASE <= pointer < USER_END
            if index >= len(devices):
                return 0
            m.uc.mem_write(pointer, struct.pack('<6sBBi32s', *devices[index]))
            return 1

        m.stub('pi4_bt_status', status)
        m.stub('pi4_bt_scan_start', scan)
        m.stub('pi4_bt_device', device)
        m.launch(terminal=False)
        m.tap(405, 66)
        self.assertEqual(m.rows[9].strip(), 'Starting Bluetooth...')

        state = 2  # BT_READY
        m.app_clock_us += 250000
        m.step()
        self.assertEqual(m.rows[15].strip(), 'No BLE devices found')
        m.tap(75, 200)
        self.assertEqual(m.rows[10].strip(), 'Could not start scan')

        scan_result = 1
        m.keys.append((2, 0))
        m.step()
        self.assertEqual(m.rows[9].strip(), 'Scanning for BLE devices...')
        self.assertNotIn(10, m.rows)
        address = bytes.fromhex('665544332211')
        long_name = b'ABCDEFGHIJKLMNOPQRSTUVWXYZ123456'
        devices.append((address, 0, 0, -42, long_name))
        m.app_clock_us += 250000
        m.step()
        self.assertEqual(m.rows[15].strip(), long_name.decode())
        self.assertEqual(m.rows[16].strip(), '11:22:33:44:55:66  -42 dBm')

        devices[0] = (address, 0, 0, 127, b'')
        m.app_clock_us += 250000
        m.step()
        self.assertEqual(m.rows[15].strip(), 'Unnamed device')
        self.assertEqual(m.rows[16].strip(), '11:22:33:44:55:66  RSSI unavailable')
        state = 2
        m.app_clock_us += 250000
        m.step()
        self.assertEqual(m.rows[9].strip(), 'Ready to scan')
        self.assertEqual(m.rows[15].strip(), 'Unnamed device')

    def test_isolated_app_uses_its_own_stack_and_real_kernel_stats(self):
        m = TerminalMachine(self.image)
        m.launch()
        self.assertEqual(m.rows[2], 'Vantage terminal')
        self.assertEqual(m.rows[5], '>')
        m.command('help')
        self.assertIn('Show commands', m.output.decode())
        m.command('mem')
        self.assertIn('RAM: 128 MiB', m.output.decode())
        self.assertIn('Heap used: 0/8388608 bytes', m.output.decode())
        self.assertNotIn('Simulator', m.output.decode())
        m.command('clear')
        self.assertEqual(m.rows[2], '>')
        self.assertTrue(all(not text for row, text in m.rows.items() if 2 < row < 33))
        self.assertTrue(any(b[-1].startswith(b'Shift') for b in m.buttons))
        # Tap H, E, L, P and ENTER through the linked touch syscall path.
        for x, y in ((286, 661), (124, 611), (425, 661), (448, 611), (425, 761)):
            m.touches.extend(((1, x, y), (3, x, y)))
        m.step()
        self.assertIn('help   Show commands', m.output.decode())
        self.assertGreater(m.presentations, 0)
        # Every kernel entry came from EL0 inside the process's address space.
        asid = m.process_field(m.app, 36, 4)
        self.assertTrue(asid)
        self.assertEqual({caller for _, caller, *_ in m.syscalls}, {asid})
        # It was also preempted by timer ticks, always from EL0, and kept working.
        self.assertEqual({(kind, origin) for _, kind, origin, _ in m.entries}, {('sync', 0), ('irq', 0)})
        self.assertTrue(all(USER_BASE <= pc < USER_END for *_, pc in m.entries))
        # A stack overflow would fault on the unmapped page below the stack.
        self.assertGreater(m.lowest_user_sp, USER_END - STACK_BYTES)
        self.assertEqual(m.interject('process_info', m.app, 0x7bff000), OK)
        self.assertEqual(struct.unpack('<QI', m.uc.mem_read(0x7bff000, 12))[1], 2)   # Still live.

    def test_two_contacts_type_and_keep_independent_pressed_state(self):
        m = TerminalMachine(self.image)
        m.launch()
        m.command('clear')
        # Hold H while a second finger taps E and drags off L.
        m.touches.extend(((1, 286, 661, 2), (1, 124, 611, 6), (3, 124, 611, 6),
                          (1, 425, 661, 6), (2, 0, 500, 6), (3, 425, 661, 6)))
        m.step()
        self.assertEqual(m.rows[2], '> e')
        m.touches.append((3, 286, 661, 2))
        m.step()
        self.assertEqual(m.rows[2], '> eh')

    def test_delete_hold_delay_repeat_release_drag_and_clock_wrap(self):
        m = TerminalMachine(self.image)
        m.launch()
        m.command('clear')
        m.keys.extend((1, ord(c)) for c in 'abcdef')
        m.app_clock_us = 0xfffffff0
        m.touches.append((1, 435, 711))
        m.step()
        self.assertEqual(m.rows[2], '> abcde')  # First delete is immediate.
        for elapsed, expected in ((399999, '> abcde'), (1, '> abcd'),
                                  (69999, '> abcd'), (1, '> abc'), (1000000, '> ab')):
            m.app_clock_us += elapsed
            m.step()
            self.assertEqual(m.rows[2], expected)
        m.touches.append((3, 435, 711))
        m.app_clock_us += 1000000
        m.step()
        self.assertEqual(m.rows[2], '> ab')  # No release delete or late repeat.
        m.touches.append((1, 435, 711))
        m.step()
        self.assertEqual(m.rows[2], '> a')
        m.touches.append((2, 0, 500))
        m.app_clock_us += 1000000
        m.step()
        self.assertEqual(m.rows[2], '> a')  # Drag out stops repeat.
        m.touches.append((3, 435, 711))
        m.step()
        self.assertEqual(m.rows[2], '> a')
        m.touches.append((1, 435, 711))
        m.step()
        for _ in range(3):
            m.app_clock_us += 500000
            m.step()
            self.assertEqual(m.rows[2], '>')  # Empty input stays bounded.

    def test_linked_syscall_rejects_buffers_from_kernel_callers(self):
        # Without a calling process there is no address space to resolve against.
        m = TerminalMachine(self.image)
        m.boot(cores=1)
        for call in (1, 4, 6, 7, 8, 10, 11, 13):
            for pointer in (0, self.image.symbols['__kernel_stack_top'] - 256, USER_BASE, USER_BASE + 4096,
                            USER_END - 256):
                with self.subTest(call=call, pointer=hex(pointer)):
                    self.assertEqual(m.call('handle_sys_call', call, pointer), 0)
        self.assertEqual(m.output, b'')
        self.assertEqual(m.rows, {})

    def test_freestanding_copy_and_zero_handle_unaligned_buffers(self):
        m = Machine(self.image)
        source, target = 0x7000001, 0x7001003
        data = bytes(range(256))
        m.uc.mem_write(source, data)
        for size in (0, 1, 7, 8, 31, 65, 132, 256):
            with self.subTest(size=size):
                m.uc.mem_write(target - 1, b'\xa5' * 258)
                self.assertEqual(m.call('memcpy', target, source, size), target)
                self.assertEqual(m.uc.mem_read(target, size), data[:size])
                self.assertEqual(m.uc.mem_read(target - 1, 1), b'\xa5')
                self.assertEqual(m.uc.mem_read(target + size, 1), b'\xa5')
                self.assertEqual(m.call('memset', target, 0x123, size), target)
                self.assertEqual(m.uc.mem_read(target, size), b'\x23' * size)
                self.assertEqual(m.uc.mem_read(target + size, 1), b'\xa5')


if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    unittest.main(argv=[sys.argv[0]] + sys.argv[2:])
