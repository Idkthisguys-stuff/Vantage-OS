# Built-in terminal

Vantage boots a small terminal application on the existing 480x800 logical
canvas: 60 columns by 50 rows of 8x16 ASCII cells. The bottom 272 pixels hold a
graphical QWERTY keyboard, leaving 33 rows for terminal output. The simulator runs the same
shell and application code with an SDL backend. On Pi, the application runs at
EL0 and uses syscalls for input, rendering, frame pacing, statistics and serial
output. The hardware display keeps its existing panel rotation.

## Input and commands

In the simulator, focus the window and type normally. SDL text events supply
characters; keydown events supply Enter, Backspace, Ctrl+C and Ctrl+L. Ordinary
keydown events do not duplicate text. Key repeats use SDL's normal event stream.
The terminal disables the touch-coordinate overlay so it does not cover text.
The on-screen keyboard also accepts mouse clicks in the simulator and touch on Pi.
The keyboard has rounded keycaps, shadows, staggered letter rows and a wide Space
bar. Letters, digits and punctuation use double-size 16x32 labels; control labels
are enlarged where they fit. Pressed keys highlight; releasing inside the same
key types it. Dragging off cancels the tap. Shift highlights when active and
applies to the next character. Del deletes immediately on press, then repeats
after 400 ms at 70 ms intervals while held. Release or dragging off stops repeats;
an application stall does not produce a burst of overdue deletes.
Space inserts a space, Enter submits, and Cancel cancels the command.
Comma, period, digits and shifted number symbols are available. This terminal-specific
keyboard repeats only Del and has no full punctuation layout or USB keyboard driver.

On Pi, type through the existing mini-UART serial connection, configured for
115200 baud, 8 data bits, no parity and one stop bit. Disable the serial client's
local echo. Output is mirrored to serial. CR, LF and CRLF are accepted; an RX
overrun discards the affected command through its next line ending. This is a
polling interface for interactive typing, without flow control for bulk pastes.
Physical USB keyboard support is a separate input-driver task.

| Command/key | Behavior |
|---|---|
| `help` | List built-in commands |
| `clear` | Clear output and show a fresh prompt |
| `mem` | Show the current backend's memory information |
| `echo text` | Print text after the command |
| `about` | Describe the built-in shell |
| Backspace | Remove the last typed character, including across a wrapped line |
| Ctrl+C | Cancel the current command |
| Ctrl+L | Clear output while preserving the current command |

The shell accepts printable ASCII, scrolls the visible screen, and has a static
underline cursor. It has no command history, scrollback, cursor navigation,
Unicode/IME support, quoting, pipelines, file commands or executable loading.
Unsupported serial ANSI navigation sequences are consumed, not inserted into
the command. A line is limited to 127 characters (or less on a smaller terminal).
Overlong commands are discarded rather than executing a retained prefix.

## Memory information

On Pi, `mem` reads the real `MemoryStats` through the core-0 EL1 syscall path:
firmware RAM bytes, free/allocatable pages and used/total kernel heap bytes.
These are allocator counters, not per-process accounting.

In the simulator, `mem` explicitly reports **host-managed memory** and the
SDL back-buffer allocation size. The application does not boot an ARM kernel or
simulate the kernel allocator. The real C allocators and DTB parser continue to
run in their separate native tests.

## Implementation and ownership

- `user/env/terminal.c` owns the bounded screen model, line editing and commands.
  It has no SDL, MMIO or kernel-global dependencies and is tested independently.
- `user/env/terminal_app.c` keeps mutable state in `.user.bss` on hardware. It
  reads at most 64 keyboard and 64 touch events per step, redraws dirty rows and
  the keyboard when needed, and presents only when something changed. Button
  geometry is shared by rendering and hit testing.
- `drivers/video/graphics.c` renders the printable ASCII font from
  `drivers/video/font8x16.h` at its native 8x16 size. Row, length and cursor bounds are
  checked before drawing. The caller's existing canvas color is preserved.
- `sim/platform/display_sim.c` routes keyboard and touch into independent bounded
  queues. Keyboard overflow cancels the affected line at its next Enter instead
  of executing truncated input; touch overflow resets the gesture with release.
- `drivers/input/key.c` maps nonblocking mini-UART input to the same key events.
- `kernel/memory.c` supplies freestanding `memcpy`/`memset` entry points required
  by compiler-generated structure copies and initialization. Its Makefile flags
  prevent loop-pattern optimization from generating recursive library calls.

New syscall payloads contain no nested pointers. `init/main.c` validates their
complete user-memory range and alignment, snapshots input structures on the
kernel stack, and writes only validated output buffers. IDs are:

| ID | Operation | Payload |
|---|---|---|
| 6 | Poll key input | `KeyEvent` output, 4-byte alignment |
| 7 | Draw text row | `TextRow` input, 4-byte alignment |
| 8 | Console geometry/memory info | `ConsoleInfo` output, 8-byte alignment |
| 9 | Wait for frame pacing | None |
| 10 | Write bounded serial bytes | `ConsoleWrite` input, 4-byte alignment |
| 11 | Draw rounded button with centered label | `CanvasButton` input, 4-byte alignment |
| 12 | Read wrapping microsecond clock | None; returns a 32-bit counter |

The button renderer validates pixel bounds, radius, text scale (0/1 native, 2 double)
and its inline eight-byte label (at most seven characters plus terminator), and
preserves the canvas color. Hold timing uses unsigned elapsed-time subtraction
so the microsecond counter's rollover does not interrupt repeat behavior.

The existing touch/shape syscalls are retained. This single-app interface does
not provide process isolation, a PTY subsystem or per-application terminal sessions.

## Validation

The simulator's CTest suite includes:

- `terminal_model`: commands, backend labels, editing, wrap/scroll behavior,
  maximum input and 20,000 deterministic mixed input events with memory guards.
- `serial_input`: the real UART/key code against mocked MMIO, including CRLF,
  ANSI filtering, receive overruns and bounded binary output.
- `terminal_smoke`: injected SDL keyboard and touchscreen events through the real syscall wrappers and
  shell to rendered pixels; font, cursor and row bounds; independent input queues
  and overflow cancellation; rounded button pixels, gaps, pressed feedback,
  release-to-type, drag cancellation, double-size label pixels and invalid button
  geometry/labels/scales.
- `user_memory`: rejection of invalid pointers for all new pointer-taking calls.

With a fresh GNU AArch64 build and the existing Unicorn/Capstone dependencies:

```sh
python tests/arm/terminal.py /path/to/kernel8.elf -v
```

This executes linked app code using its real user stack, actual syscall wrappers
and kernel dispatcher, with modeled SVC transitions and mocked devices. It checks
real kernel memory statistics, user-stack guards, syscall bounds, the freestanding
memory routines, and Del repeat delay/cadence, release, drag cancellation, empty
input and clock rollover. It does not prove hardware privilege enforcement,
UART behavior or Pi display/cache correctness. Confirm those on Pi, including
touch keyboard coordinates, typing commands, comparing serial/display output,
scrolling, and `mem` counters.

Font source: [JetBrains Mono v2.304 Regular](https://github.com/JetBrains/JetBrainsMono/tree/v2.304),
under the SIL Open Font License 1.1; retain `drivers/video/OFL.txt` when distributing
the font. The font is a third-party asset, not an original Vantage typeface.
`drivers/video/gen_font8x16.py` generates the bitmap header using Pillow:

```sh
python -m pip install pillow
python drivers/video/gen_font8x16.py --ttf /path/to/JetBrainsMono-Regular.ttf
```

The generated header is included in the source tree, so normal kernel/simulator
builds do not need Python, Pillow or a font download.
