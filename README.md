# Ivia

A quiet, keyboard-first academic PDF reader, written in C++20 and Qt 6.

Ivia pairs a bare reading canvas with Vim movements, an Ex-style command line,
independent document buffers, search, persistent marks, and reference jumps.
The reading canvas uses Qt PDF directly; it does not embed a browser.

There are no persistent toolbars, buffer tabs, status bars, or scrollbars. The
default surround uses Sioyek's neutral light background, with black/white command input.
Press `:` or `/` for a small, single-line input over the canvas; it disappears on
Enter or Escape without resizing or moving the paper. Tab / Shift+Tab cycle
completions in place. Contents, marks and buffer lists appear only when requested.
Page and search information lives in the native window title; `Ctrl+G` or `:info`
briefly shows the filename, page and zoom. Other notices also disappear automatically.

The previous iris palette has been removed. Light mode takes its canvas and
command colors from Sioyek's shipped defaults; dark mode takes its base and input
colors from Zathura's defaults:

| Element | Light (`:set light`) | Dark (`:set dark`) |
| --- | --- | --- |
| Canvas | RGB 0.97, 0.97, 0.97 (approximately `#f7f7f7`) | `#000000` |
| Command input background | `#000000` | `#131313` |
| Command input text | `#ffffff` | `#9fbc00` |

Sources: [Sioyek prefs.config](https://github.com/ahrm/sioyek/blob/main/pdf_viewer/prefs.config),
[Sioyek main.cpp](https://github.com/ahrm/sioyek/blob/main/pdf_viewer/main.cpp), and
[Zathura config.c](https://github.com/pwmt/zathura/blob/develop/zathura/config.c).
PDF page colors are preserved in both modes. Search uses the upstream highlight
RGB values (green in Sioyek, yellow-green and green in Zathura); selection uses
Sioyek's yellow. Ivia applies transparency to these overlays. Additional UI grays
are neutral, with no iris tint, colored borders, or decorative page shadows.

## Build and run

Requirements: CMake 3.22+, a C++20 compiler, and **Qt 6.6+ with Widgets and PDF**.
Qt Test is required when `BUILD_TESTING=ON` (the default). Qt 6.10.3 is the version
used for the development build on macOS; newer macOS SDKs may require a newer Qt.

Install Qt using the Qt installer, selecting Qt PDF, or your system's Qt development
packages. Point `CMAKE_PREFIX_PATH` at your Qt installation if CMake cannot find it.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.3/macos
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

macOS:

```sh
open build/ivia.app
build/ivia.app/Contents/MacOS/ivia --demo
build/ivia.app/Contents/MacOS/ivia ~/papers/paper.pdf
```

Linux / Windows: run `build/ivia` / `build/ivia.exe` (the exact path can depend on
your CMake generator). `ivia --help` lists command-line options. `--no-session`
starts with an empty workspace. Explicit file arguments or `--demo` take precedence
over restoring the previous session.

To deploy with the Qt runtime libraries included:

```sh
cmake --install build --prefix build/package
```

On macOS this creates `build/package/ivia.app`. This is a local, unsigned build;
shipping a public release additionally requires platform packaging and signing.

## Keyboard

Keys apply in normal mode. `Esc` cancels a pending sequence, closes a panel,
clears the text selection, or leaves the command prompt. Most movements accept
a count: `8j`, `3J`, `12G`, `2gg`, `4n`.

| Keys | Action |
| --- | --- |
| `h j k l` | Scroll left, down, up, right |
| `Ctrl+D`, `Ctrl+U` | Half screen down / up |
| `Ctrl+F`, `Ctrl+B`, `Space` | Screen down / up / down |
| `gg`, `G`, `12G` | First page, last page, page 12 |
| `J`, `K` | Next / previous physical page |
| `+`, `-`, `0` | Zoom in, zoom out, fit width |
| `zi`, `zo`, `za`, `zz` | Zoom in, out, fit width, fit page |
| `/text`, `?text` + Enter | Search forward / backward |
| `n`, `N` | Next / previous search result, with wraparound |
| `f`, `F` | Follow / preview a visible link using keyboard hints |
| `gd`, `gp` | Follow / preview the link under the pointer; otherwise show hints |
| `gd` with a selected citation | Look up a numbered bibliography entry |
| `Ctrl+O`, `Ctrl+I` | Back / forward through jumps |
| `ma`, `'a` | Set mark a, return to mark a |
| `gt`, `gT` | Next / previous buffer |
| `t` | Toggle the document contents |
| `y` | Copy mouse-selected text |
| `:` | Open the command prompt |
| `Ctrl+G` | Briefly show filename, page and zoom |

On macOS, `Ctrl` is the physical Control key. `Command+O` opens PDFs;
on other platforms, use `Ctrl+Shift+O` so `Ctrl+O` remains available for jump history.
Mouse wheel / trackpad scrolling, mouse PDF links, Shift-click link previews,
mouse back/forward buttons, and Control/Command+wheel zoom also work.

## Commands

| Command | Action |
| --- | --- |
| `:e path/to/paper.pdf` | Open a PDF in a buffer; `:edit` is an alias |
| `:b`, `:buffers`, `:ls` | Show open buffers |
| `:b 2`, `:b unique-name` | Switch to a buffer by number or unique filename fragment |
| `:bn`, `:bp`, `:bd` | Next, previous, close buffer |
| `:42`, `:page 42` | Jump to physical page 42 |
| `:toc`, `:marks` | Show embedded contents or marks |
| `:mark a`, `:delmark a` | Set / delete a persistent mark |
| `:ref 12` | Look for bibliography entry `[12]`, `12.` or `12)` |
| `:zoom 125` | Set zoom to 125%; accepted range: 10–500% |
| `:fit width`, `:fit page` | Fit page width / whole page |
| `:noh`, `:nohlsearch` | Clear search and its highlights |
| `:reload` | Reload the current PDF from disk |
| `:set light`, `:set dark` | Change interface theme |
| `:help` | Open the full field guide |
| `:info` | Briefly show filename, page and zoom |
| `:q`, `:quit`, `:qa` | Save the reading session and exit |

Use Tab / Shift+Tab to cycle command, PDF path, and buffer completions. Up/Down recall prompt
history. Paths support spaces, surrounding quotes, and `~/`; relative `:e` paths
resolve beside the current PDF (or the working directory with no PDF open).
In panels, use `j`/`k` and Enter, or the mouse.

## Reading state and implementation

- Each buffer owns its PDF document, canvas, search, selection, marks, and jump
  history. Switching buffers retains their state.
- `QPdfPageRenderer` rasterizes visible pages on a worker thread. Each buffer has
  a bounded 48 MiB image cache. Raster dimensions are capped to bound memory at
  extreme zoom levels. PDF text remains available for search and selection.
- Search is case-insensitive, literal, and incremental, scanning one page per
  event-loop turn. Changing the query cancels the previous scan. Results are
  highlighted in green/yellow-green; the active result has a fine black frame. Broad queries stop at 20,000
  matches and display a message.
- Link hints use embedded PDF link geometry. Numbered reference fallback scans
  extracted text from the final page backward, one page per event-loop turn.
- Reading positions, zoom, open buffers, interface theme, window geometry, and
  document marks are saved through `QSettings` every five seconds and on exit.
  Closing a buffer retains its marks and position for reopening. Search and jump
  history are per-session. Document paths are saved locally; PDFs are never
  modified or uploaded.
- External HTTP(S) and mail links open in the system handler. Other URL schemes
  are rejected. Ivia performs no background network requests.

`src/reader_view.*` handles PDF layout, rendering, selection, search, links and
navigation. `src/main_window.*` owns buffers, UI, keyboard modes, panels and state.
`src/command.*` parses commands and paths. `src/demo.*` generates the original
four-page sample guide used by `--demo`.

## Scope of this first version

PDF only. Text search and reference inference require an existing text layer;
there is no OCR. Numbered reference lookup is a heuristic, not a semantic citation
resolver. It can mistake a numbered list for a bibliography and does not infer
author–year references. PDF links are preferable when present. Text selection is
mouse-driven and currently stays within one page. The dark theme changes the
interface; it preserves the original paper colors. Annotation editing, automatic
file watching, split views, and key remapping are not implemented.

## Tests

Qt Test integration tests generate their own PDF with text, an internal link, an
outline and numbered references. They cover invalid-file recovery, buffer
isolation, counted Vim motions, prompt history, command validation, marks and
session restore, link hints, citation fallback, jump history, search wraparound
and cancellation, and sample-guide generation. Tests use isolated temporary
settings and run with Qt's offscreen platform.

For reproducible UI captures:

```sh
mkdir -p build/screenshots
QT_QPA_PLATFORM=offscreen IVIA_SCREENSHOT_DIR="$PWD/build/screenshots" build/ivia_tests
```
