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
| `ls` | List stored files |
| `cat name` | Read a file |
| `edit name` | Edit a text file |
| `rm name` | Delete a file |

Backspace edits, Ctrl+C cancels a line, and Ctrl+L clears the display while keeping
your input. On-screen Shift applies to the next character. Hold Del to repeat
deletion; release or drag off to stop.

`mem` shows host-managed display memory in the simulator and kernel allocator
statistics on Pi.

## Scope

Input is printable ASCII, limited to 127 characters per command. Overlong input is
discarded. The terminal does not yet run programs, and has no
command history or physical USB keyboard driver.

Physical Pi touch, serial and display validation is pending. Desktop and mocked
ARM tests are described in the [developer guide](DEVELOPMENT.md#terminal).

## Font

The bitmap font uses [JetBrains Mono v2.304](https://github.com/JetBrains/JetBrainsMono/tree/v2.304)
under the [SIL Open Font License](../drivers/video/OFL.txt). Normal builds use the
included bitmap; regeneration is documented in the developer guide.

## Persistent text files

With storage mounted, use `ls`, `cat notes`, `rm notes`, or `edit notes`.
Short names refer to `/ram/`; full `/ram/name` paths also work. The editor is a
bounded ASCII line editor, not a full-screen cursor editor:

```text
edit notes
My first line
Another line
.save
.quit
cat notes
rm notes
```

Existing text is loaded when entering `edit`. New lines append to the buffer.
Use `.show` to display it, `.clear` to replace its contents, `.save` to write it,
`.delete` to remove the file and exit, or `.quit` to discard unsaved changes.
Start a line with `..` to store a literal leading dot. Commands use dots so they
can also be entered on the graphical keyboard. Files hold at most 4096 bytes;
input lines hold at most 127 characters. Binary files are rejected by the editor.

Saving truncates the file and writes it in 256-byte chunks through `SYS_FILE_*`.
Each chunk is durable separately: a power loss during save can leave an empty or
partially saved file. Failed saves retain the in-memory buffer but may already
have changed the disk file. There is no atomic whole-document replacement yet.

On Pi, storage is available only in the explicitly enabled storage-validation
image described in [STORAGE.md](STORAGE.md); normal SD boot remains disabled.
In the simulator, provision a regular image with `tools/storage_image.py` and
set `VANTAGE_STORAGE_IMAGE` to its path before launch. An unmounted filesystem
reports `Storage unavailable` rather than silently saving to RAM.
