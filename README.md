# mkwin

A tiny window/platform layer for Linux (X11) and Windows (Win32). It owns the OS
window, a shared-memory framebuffer, DPI scale, and the input/event pump - and
nothing else. No widgets, no layout, no theming, no text layout. You draw 32-bit
pixels into a buffer it hands you and tell it to present.

mkwin was split out of the [mkgui](https://github.com/vtlmks/mkgui) toolkit so
the same window layer can back both mkgui *and* bespoke programs that do their
own rendering - a skinned media player, a demo, a tool - without dragging a whole
widget toolkit along. mkgui consumes mkwin as a submodule; a standalone program
consumes it the same way.

## Scope

**What it does:** create/destroy top-level and transient windows, hand you a
software framebuffer sized to the window, present it (XShm / GDI DIB), pump input
as neutral events you pull, and cover the platform-specific plumbing around it -
DPI scaling, window geometry and min-size, undecorated and arbitrarily *shaped*
windows, cursors, class/icon hints, override-redirect popups (menus/tooltips),
clipboard (including the X11 INCR protocol for large selections), file
drag-and-drop, a monotonic clock, and two opt-in modules (glyph rasterization,
embedded OpenGL child windows).

**What it deliberately does not do:** anything above the pixel. No drawing
primitives, no fonts-as-text, no widgets, no layout, no event *meaning*. mkwin
never calls back into your logic; it hands you input and a buffer and gets out of
the way.

## Platforms and dependencies

| | Linux | Windows |
|---|---|---|
| Windowing / present | Xlib + XShm | Win32 + GDI DIB |
| Required libs | `-lX11 -lXext` | `-lgdi32` |
| `MKWIN_FONT` (optional) | FreeType2 + fontconfig | GDI (no extra lib) |
| `MKWIN_GLVIEW` (optional) | GLX child window (host links `-lGL`) | WGL child window (host links `-lopengl32`) |

C99. Single translation unit.

## Consuming it

mkwin is **source-level and single-TU**. Your program includes the header for the
API and includes the platform `.c` exactly once into its own translation unit:

```c
#include "mkwin.h"
#ifdef _WIN32
#include "mkwin_win32.c"
#else
#include "mkwin_linux.c"
#endif
```

`MKWIN_API` is `static` by default, so everything folds into your unity build with
no exported symbols. Define `MKWIN_LIBRARY` to build mkwin as a standalone object
with external linkage instead.

The two optional modules are off unless you ask:

- `-DMKWIN_FONT` compiles the glyph-rasterization primitive (FreeType on Linux,
  GDI on Windows). A program with its own bitmap font leaves it undefined.
- `-DMKWIN_GLVIEW` compiles the embedded-OpenGL child-window support.

### As a submodule

```bash
git submodule add https://github.com/vtlmks/mkwin.git mkwin
```

then build with `-Imkwin` so the `#include`s resolve.

### Ownership model

mkwin owns the framebuffer, dimensions and DPI scale, but a host reads them every
frame. Rather than route every read through an accessor, you may hand `mkwin_open`
the addresses of your own fields via `struct mkwin_host`; mkwin writes through
them whenever a value changes (open, resize, DPI change). Pass a zeroed struct (or
`NULL`) to use the accessors (`mkwin_framebuffer`, `mkwin_size`, `mkwin_scale`)
exclusively - they are always valid.

## Minimal example

```c
#include "mkwin.h"
#include "mkwin_linux.c"   // once, in this TU (mkwin_win32.c on Windows)

int main(void) {
	uint32_t *fb;
	int32_t   w, h;
	float     scale;
	struct mkwin_host host = { &fb, &w, &h, &scale };

	struct mkwin_window *win = mkwin_open(NULL, "mkwin", 800, 600, 0, "mkwin-example", &host);
	if(!win) {
		return 1;
	}
	mkwin_map(win);

	uint32_t running = 1;
	while(running) {
		mkwin_wait(win, -1);            // block until there is input
		struct mkwin_event ev;
		while(mkwin_poll(win, &ev)) {
			if(ev.type == MKWIN_EV_CLOSE) {
				running = 0;
			}
			// fb / w / h are already up to date after MKWIN_EV_RESIZE
		}

		for(int32_t i = 0; i < w * h; ++i) {
			fb[i] = 0x00204060;         // one 0x00RRGGBB pixel per element
		}
		mkwin_present(win);
	}

	mkwin_destroy(win);
	return 0;
}
```

Build it (Linux, as a standalone object):

```bash
gcc -std=c99 -O2 -DMKWIN_LIBRARY example.c -o example -lX11 -lXext
```

## Documentation

Full API reference: [documentation/doc.md](documentation/doc.md).

## License

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 Peter Fors.
