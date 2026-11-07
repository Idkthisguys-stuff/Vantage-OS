# Built-in terminal

The simulator and Pi run the same terminal app with a graphical QWERTY keyboard.
It supports built-in commands, line editing and scrolling.

## Usage

Type with your PC keyboard or click the on-screen keys. On Pi, use the touchscreen
or serial at **115200 baud, 8N1**, with local echo disabled.

| Command | Action |
|---|---|
| `help` | List commands |
| `clear` | Clear the terminal |
| `mem` | Show memory information |
| `echo text` | Print text |
| `about` | About the terminal |

Backspace edits, Ctrl+C cancels a line, and Ctrl+L clears the display while keeping
your input. On-screen Shift applies to the next character. Hold Del to repeat
deletion; release or drag off to stop.

`mem` shows host-managed display memory in the simulator and kernel allocator
statistics on Pi.

## Scope

Input is printable ASCII, limited to 127 characters per command. Overlong input is
discarded. The terminal does not yet run programs or access files, and has no
command history or physical USB keyboard driver.

Physical Pi touch, serial and display validation is pending. Desktop and mocked
ARM tests are described in the [developer guide](DEVELOPMENT.md#terminal).

## Font

The bitmap font uses [JetBrains Mono v2.304](https://github.com/JetBrains/JetBrainsMono/tree/v2.304)
under the [SIL Open Font License](../drivers/video/OFL.txt). Normal builds use the
included bitmap; regeneration is documented in the developer guide.
