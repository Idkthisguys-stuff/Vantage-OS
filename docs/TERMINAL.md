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
| `pwd` | Print current directory |
| `cd [path]` | Change directory (default `/ram`) |
| `mkdir path` | Create a persistent directory |
| `rmdir path` | Remove an empty directory |
| `ls [path]` | List current or named directory; directories end in `/` |
| `cat name` | Read a file |
| `edit name` | Edit a text file |
| `rm name` | Delete a file |

Backspace edits, Ctrl+C cancels a line, and Ctrl+L clears the display while keeping
your input. On-screen Shift applies to the next character. Hold Del to repeat
deletion; release or drag off to stop.

`mem` shows host-managed display memory in the simulator and kernel allocator
statistics on Pi.

## Scope

Input is printable ASCII, limited to 287 characters per command (less on small
screens). This accommodates a command plus a 255-byte path. Overlong input is
discarded. The terminal does not yet run programs, and has no command history or
physical USB keyboard driver.

Physical Pi touch, serial and display validation is pending. Desktop and mocked
ARM tests are described in the [developer guide](DEVELOPMENT.md#terminal).

## Font

The bitmap font uses [JetBrains Mono v2.304](https://github.com/JetBrains/JetBrainsMono/tree/v2.304)
under the [SIL Open Font License](../drivers/video/OFL.txt). Normal builds use the
included bitmap; regeneration is documented in the developer guide.

## Persistent text files

With storage mounted, use `ls`, `cat notes`, `rm notes`, or `edit notes`.
The current directory starts at `/ram`. Relative paths use that directory;
absolute `/ram/...` paths also work. `/` aliases the protected mount root, and
`..` there stays at root. `.` and `..` traverse existing directories, so missing
parents cannot be bypassed. Names use ASCII letters, digits, `.`, `_`, `-` (31
bytes per component). V3 volumes allow 255-byte total paths, 256 files/directories
combined and 1 MiB per file, subject to shared disk capacity. Existing V1/2 volumes
retain 47-byte paths, 16 entries and 4096-byte files until explicitly migrated;
see [STORAGE.md](STORAGE.md#migrate-an-existing-data-image). Parents must already
exist; `rmdir` refuses nonempty directories and root, and `rm` deletes files only.
There is no recursive removal. For example: `mkdir docs`, `cd docs`, `edit note`,
`cd ..`, `ls docs`. The on-screen comma key becomes `/` while Shift is active.

`edit notes` opens
a compact, full-screen, touch-first ASCII editor above the keyboard. Existing
files load into memory; missing files start empty and are created only on Save.

Tap a line where you want to insert text, then type with the on-screen keyboard
or desktop keyboard. The selected line is highlighted; there is no pointer or
blinking text cursor. The header shows the insertion line/column, byte count and
an asterisk for unsaved changes. Backspace removes the preceding character;
Enter splits the line. Text that looks like a shell or dot command is literal
document content while the editor is open.

| Touch control | Action |
|---|---|
| Save | Write the whole buffer to the current filename |
| Exit | Return to the shell; with unsaved changes, tap Discard again to confirm |
| Up / Down | Select the adjacent line; scroll when it leaves the view |
| Left / Right | Move the insertion point one character, including across lines |
| New | Insert a newline at the end of the selected line |
| Drop | Remove the selected line |

To cancel a pending discard, select text, edit, or use another control. Saving
also cancels the discard confirmation. The on-screen Cancel key (or Ctrl+C) acts
like Exit. Dragging away from a control before release cancels that touch.
Long lines scroll horizontally to keep the insertion position in view; `<` at
the left of the text means its beginning is off-screen. Each line has a two-row
touch target. The editor needs at least 40 columns and 12 text rows.

The editor deliberately retains a **4096-byte document limit**, independently of
the filesystem's 1 MiB file limit. Larger files are refused without truncation;
`cat` streams them without loading the entire file into the editor buffer.
Editor lines are not subject to the shell's command limit. An edit
that would exceed capacity is rejected without discarding the buffer. Binary
files are rejected; existing tabs and carriage returns display as spaces and
are preserved unless edited. Use `rm name` at the shell to delete a saved file.
The graphical display is required for editor navigation and Save; the serial
console can supply text but does not mirror the full-screen editor view.

Saving truncates the file and writes it in 256-byte chunks through `SYS_FILE_*`.
Each chunk is durable separately: a power loss during save can leave an empty or
partially saved file. Failed saves retain the in-memory buffer but may already
have changed the disk file. There is no atomic whole-document replacement yet.

On Pi, mounting requires the explicit clock and partition settings described in
[STORAGE.md](STORAGE.md); hardware acceptance is still pending.
In the simulator, provision a regular image with `tools/storage_image.py` and
set `VANTAGE_STORAGE_IMAGE` to its path before launch. An unmounted filesystem
reports `Storage unavailable` rather than silently saving to RAM.
