# mkwin API reference

mkwin is the window/platform layer: it owns the OS window, its shared-memory
framebuffer, DPI scale, and the input/event pump. It knows nothing about widgets,
layout, theme, icons, or text layout - those belong to the host.

This document is the full API surface. For the overview, dependencies, and a
runnable example, see the [README](../README.md).

## Contents

- [Concepts](#concepts)
- [Window lifecycle](#window-lifecycle)
- [Framebuffer and presentation](#framebuffer-and-presentation)
- [Event pump](#event-pump)
- [Events](#events)
- [Geometry and window management](#geometry-and-window-management)
- [Appearance](#appearance)
- [Popups](#popups)
- [Clipboard and drag-and-drop](#clipboard-and-drag-and-drop)
- [Time](#time)
- [Native-handle escape hatch](#native-handle-escape-hatch)
- [Font rasterization (MKWIN_FONT)](#font-rasterization-mkwin_font)
- [Embedded OpenGL views (MKWIN_GLVIEW)](#embedded-opengl-views-mkwin_glview)

## Concepts

### Consumption and linkage

mkwin is source-level and single-TU. Include `mkwin.h` for the API, and include
`mkwin_<platform>.c` exactly once into one translation unit. `MKWIN_API` is
`static` by default, so the whole library folds into a unity build with no
exported symbols. Define `MKWIN_LIBRARY` before including to build it as a
standalone object with external linkage.

Two optional modules are compiled in only on request:

- `MKWIN_FONT` - glyph rasterization (FreeType on Linux, GDI on Windows).
- `MKWIN_GLVIEW` - embedded OpenGL child windows (GLX / WGL).

### The connection is shared and lazy

The OS connection (an X11 `Display`, or the Win32 message context) is opened
lazily by the first `mkwin_open` and shared by every window created afterwards. It
is torn down automatically when the last window is destroyed. There is no explicit
init/shutdown call.

### Ownership and host-published state

mkwin owns the framebuffer, the window dimensions, and the DPI scale. A host reads
those every frame. Two ways to read them:

1. **Accessors** - `mkwin_framebuffer`, `mkwin_size`, `mkwin_scale`. Always valid.
2. **Published fields** - hand `mkwin_open` a `struct mkwin_host` holding the
   addresses of your own variables; mkwin writes through those pointers whenever a
   value changes (open, resize, DPI change). This spares a large host thousands of
   accessor calls. Any pointer may be `NULL`, and the whole struct may be `NULL`.

```c
struct mkwin_host {
	uint32_t **out_fb;   // receives the framebuffer base on open/resize
	int32_t   *out_w;    // receives width
	int32_t   *out_h;    // receives height
	float     *out_scale;// receives DPI scale
};
```

The framebuffer is `width * height` 32-bit pixels, row-major, one `uint32_t` per
pixel in `0x00RRGGBB` order. It is reallocated on resize; never cache the base
pointer across a resize - read it again (or from your published `out_fb`).

### Event model

mkwin never calls into host logic. Input arrives as neutral data you pull with
`mkwin_poll`; the host decides what each event means. The one exception is the
modal-frame callback (see `mkwin_set_modal_frame_cb`), which exists solely because
Win32 suspends your loop during a window move/resize.

## Window lifecycle

```c
struct mkwin_window *mkwin_open(struct mkwin_window *parent, const char *title,
                                int32_t w, int32_t h, uint32_t flags,
                                const char *app_class, struct mkwin_host *host);
void mkwin_map(struct mkwin_window *win);
void mkwin_unmap(struct mkwin_window *win);
void mkwin_destroy(struct mkwin_window *win);
```

- **`mkwin_open`** creates a window and returns a handle, or `NULL` on failure.
  `parent == NULL` makes a top-level window; a non-NULL parent makes a
  transient/child (dialog) of it. `flags` may include `MKWIN_UNDECORATED`.
  `app_class` is the WM class / window-class string. `host` may be `NULL`. The
  window is created unmapped.
- **`mkwin_map`** / **`mkwin_unmap`** show and hide the window.
- **`mkwin_destroy`** destroys the window and its resources. Destroying the last
  window closes the shared OS connection.

### Flags

| Flag | Meaning |
|---|---|
| `MKWIN_UNDECORATED` | No title bar or border (override the WM decoration). |

## Framebuffer and presentation

```c
uint32_t *mkwin_framebuffer(struct mkwin_window *win);
void  mkwin_size(struct mkwin_window *win, int32_t *out_w, int32_t *out_h);
float mkwin_scale(struct mkwin_window *win);
void  mkwin_present(struct mkwin_window *win);
void  mkwin_present_region(struct mkwin_window *win, int32_t x, int32_t y, int32_t w, int32_t h);
void  mkwin_flush(struct mkwin_window *win);
void  mkwin_sync(struct mkwin_window *win);
```

- **`mkwin_framebuffer`** returns the current pixel buffer base.
- **`mkwin_size`** writes the current width/height.
- **`mkwin_scale`** returns the DPI scale factor (1.0 = 96 dpi). Overridable at
  runtime with the `MKWIN_SCALE` environment variable (`1.5`, `150%`, `144dpi`).
- **`mkwin_present`** blits the whole framebuffer to the window.
- **`mkwin_present_region`** blits only the given rectangle - cheaper for small
  updates.
- **`mkwin_flush`** flushes buffered output to the server without waiting.
- **`mkwin_sync`** flushes and waits for the server to finish (X11 `XSync`); a
  no-op where presentation is already synchronous (Win32).

## Event pump

```c
uint32_t mkwin_poll(struct mkwin_window *win, struct mkwin_event *ev);
uint32_t mkwin_pending(struct mkwin_window *win);
void     mkwin_wait(struct mkwin_window *win, int64_t timeout_ns);
void     mkwin_wait_fd_add(int32_t fd);
void     mkwin_wait_fd_remove(int32_t fd);
void     mkwin_set_modal_frame_cb(struct mkwin_window *win,
                                  void (*cb)(struct mkwin_window *win, void *user), void *user);
```

- **`mkwin_poll`** fills `ev` and returns non-zero when an event was dequeued, zero
  when the queue is empty. Drain it in a `while` loop.
- **`mkwin_pending`** returns non-zero if at least one event is queued.
- **`mkwin_wait`** blocks on the shared OS connection until input arrives or
  `timeout_ns` elapses. A negative timeout blocks indefinitely.
- **`mkwin_wait_fd_add`** / **`mkwin_wait_fd_remove`** fold extra file descriptors
  (a `timerfd`, `eventfd`, or pipe) into `mkwin_wait`, so a host timer can wake an
  otherwise-idle loop. Add a descriptor when you start watching it, remove it when
  done. On Win32 there is no pollable fd set, so these are inert; there, bound your
  `mkwin_wait` timeout by your own next deadline instead.
- **`mkwin_set_modal_frame_cb`** registers a callback that fires roughly once per
  frame while the platform is in a modal move/resize loop (Win32 sizing/moving),
  when your own event loop is suspended. Use it to keep animating and redrawing. On
  X11 there is no such modal loop, so it never fires - register it unconditionally
  and the code stays cross-platform.

## Events

`mkwin_poll` fills a `struct mkwin_event`:

```c
struct mkwin_event {
	uint32_t type;         // MKWIN_EV_*
	int32_t  x, y;         // pointer position (MOTION / BUTTON_*)
	int32_t  width, height;// new client size (RESIZE)
	uint32_t button;       // 1=left, 2=middle, 3=right, 4/5=wheel up/down
	uint32_t keysym;       // key symbol (KEY); X11 keysym space on both platforms
	uint32_t keymod;       // MKWIN_MOD_* bitmask
	char     text[32];     // UTF-8 text produced by the key (KEY), NUL-terminated
	int32_t  text_len;     // bytes in text, excluding the terminator
	int32_t  popup_idx;    // which popup the event targets, or -1 for the window
};
```

### Event types

| Type | Meaning | Fields used |
|---|---|---|
| `MKWIN_EV_NONE` | No event (sentinel). | - |
| `MKWIN_EV_EXPOSE` | A region needs repainting. | - |
| `MKWIN_EV_RESIZE` | The window was resized. | `width`, `height` |
| `MKWIN_EV_MOTION` | Pointer moved. | `x`, `y`, `keymod` |
| `MKWIN_EV_BUTTON_PRESS` | Pointer button pressed. | `x`, `y`, `button`, `keymod` |
| `MKWIN_EV_BUTTON_RELEASE` | Pointer button released. | `x`, `y`, `button` |
| `MKWIN_EV_KEY` | Key pressed. | `keysym`, `keymod`, `text`, `text_len` |
| `MKWIN_EV_CLOSE` | The window manager asked to close. | - |
| `MKWIN_EV_LEAVE` | Pointer left the window. | - |
| `MKWIN_EV_FOCUS_OUT` | The window lost keyboard focus. | - |
| `MKWIN_EV_DROP` | Files were dropped (see drag-and-drop). | - |

By `MKWIN_EV_RESIZE`, the framebuffer has already been reallocated and the size /
published fields updated; re-read the base pointer.

### Modifiers

| Bit | |
|---|---|
| `MKWIN_MOD_SHIFT` | Shift held. |
| `MKWIN_MOD_CONTROL` | Control held. |
| `MKWIN_MOD_ALT` | Alt held. |

### Cursors

Passed to `mkwin_set_cursor`:

| | |
|---|---|
| `MKWIN_CURSOR_DEFAULT` | Normal arrow. |
| `MKWIN_CURSOR_H_RESIZE` | Horizontal resize. |
| `MKWIN_CURSOR_V_RESIZE` | Vertical resize. |

## Geometry and window management

```c
void mkwin_move(struct mkwin_window *win, int32_t x, int32_t y);
void mkwin_position(struct mkwin_window *win, int32_t *out_x, int32_t *out_y);
void mkwin_resize(struct mkwin_window *win, int32_t w, int32_t h);
void mkwin_set_min_size(struct mkwin_window *win, int32_t min_w, int32_t min_h);
void mkwin_begin_drag(struct mkwin_window *win);
void mkwin_set_shape(struct mkwin_window *win, const int32_t *points_xy, uint32_t point_count);
void mkwin_set_shape_mask(struct mkwin_window *win, const uint32_t *argb, int32_t w, int32_t h, uint32_t alpha_threshold);
void mkwin_clear_shape(struct mkwin_window *win);
void mkwin_set_title(struct mkwin_window *win, const char *title);
uint32_t mkwin_is_child(struct mkwin_window *win);
void mkwin_screen_size(struct mkwin_window *win, int32_t *out_w, int32_t *out_h);
void mkwin_translate_coords(struct mkwin_window *win, int32_t local_x, int32_t local_y, int32_t *out_screen_x, int32_t *out_screen_y);
```

- **`mkwin_move`** / **`mkwin_position`** set and read the window's top-left in
  screen coordinates.
- **`mkwin_resize`** requests a new client size.
- **`mkwin_set_min_size`** clamps the window's minimum size (WM size hints).
- **`mkwin_begin_drag`** starts a window-manager move drag from the current pointer
  grab - use it to make a custom (undecorated) title bar draggable.
- **`mkwin_set_shape`** shapes the window to a polygon given as `point_count`
  interleaved `x,y` pairs.
- **`mkwin_set_shape_mask`** shapes the window from the alpha channel of a `w*h`
  ARGB buffer: pixels with alpha >= `alpha_threshold` are kept, the rest cut away.
  Row-span based, so a per-pixel skin mask maps straight onto the platform region.
- **`mkwin_clear_shape`** removes any shape, restoring a rectangular window.
- **`mkwin_set_title`** sets the title-bar text.
- **`mkwin_is_child`** returns non-zero if the window was created transient to a
  parent.
- **`mkwin_screen_size`** writes the size of the screen the window is on.
- **`mkwin_translate_coords`** maps a window-local point to absolute screen
  coordinates (used to place popups).

## Appearance

```c
void mkwin_set_cursor(struct mkwin_window *win, uint32_t cursor);
void mkwin_set_class_hint(struct mkwin_window *win, const char *instance, const char *cls);
void mkwin_set_icon(struct mkwin_window *win, const struct mkwin_icon_size *sizes, uint32_t count);
```

- **`mkwin_set_cursor`** sets the pointer cursor (`MKWIN_CURSOR_*`).
- **`mkwin_set_class_hint`** sets the WM instance/class strings.
- **`mkwin_set_icon`** sets the window icon from one or more ARGB bitmaps at
  different sizes:

```c
struct mkwin_icon_size {
	uint32_t *pixels;  // w*h ARGB
	int32_t   w, h;
};
```

## Popups

Override-redirect overlays for menus, tooltips, and dropdowns: borderless,
undecorated, not managed by the window manager.

```c
struct mkwin_popup *mkwin_popup_open(struct mkwin_window *parent, int32_t x, int32_t y, int32_t w, int32_t h);
uint32_t *mkwin_popup_framebuffer(struct mkwin_popup *popup);
void mkwin_popup_present(struct mkwin_popup *popup);
void mkwin_popup_close(struct mkwin_popup *popup);
```

- **`mkwin_popup_open`** creates and shows a popup at absolute screen position
  `x, y` with size `w, h` (convert with `mkwin_translate_coords`).
- **`mkwin_popup_framebuffer`** returns the popup's own pixel buffer to draw into.
- **`mkwin_popup_present`** blits it.
- **`mkwin_popup_close`** destroys it.

Events targeting a popup arrive through the parent window's `mkwin_poll` with
`ev.popup_idx` set to the popup's index (rather than `-1`).

## Clipboard and drag-and-drop

```c
void  mkwin_clipboard_set(struct mkwin_window *win, const char *text, uint32_t len);
char *mkwin_clipboard_get(struct mkwin_window *win, uint32_t *out_len);

void        mkwin_drop_enable(struct mkwin_window *win);
uint32_t    mkwin_drop_count(struct mkwin_window *win);
const char *mkwin_drop_path(struct mkwin_window *win, uint32_t index);
```

- **`mkwin_clipboard_set`** puts `len` bytes of text on the clipboard.
- **`mkwin_clipboard_get`** returns a `malloc`'d, NUL-terminated copy of the
  clipboard text that the caller frees; `*out_len` is the length excluding the
  terminator. Returns `NULL` when the selection is empty or not text. On X11 large
  selections are transferred via the INCR protocol transparently.
- **`mkwin_drop_enable`** advertises the window as a drop target (XDnD / OLE).
- After an `MKWIN_EV_DROP` event, **`mkwin_drop_count`** returns how many files were
  dropped and **`mkwin_drop_path`** returns each path (index `0 .. count-1`). The
  strings are owned by mkwin and valid until the next drop.

## Time

```c
uint64_t mkwin_now_ns(void);
void     mkwin_sleep_ns(uint64_t ns);
```

- **`mkwin_now_ns`** returns a monotonic timestamp in nanoseconds.
- **`mkwin_sleep_ns`** sleeps for the given number of nanoseconds.

Nanoseconds only - convert at the call site if you need other units.

## Native-handle escape hatch

```c
void *mkwin_display(struct mkwin_window *win);
```

Returns the native connection handle (an X11 `Display *` on Linux) as a `void *`.
Only host code that must talk to a native API directly - binding a GLX context to
an `mkwin_glview`, for example - needs this. Ordinary hosts never call it.

## Font rasterization (MKWIN_FONT)

Compiled only when `MKWIN_FONT` is defined. mkwin exposes just the platform glyph
primitive: open a font at a pixel size and rasterize one glyph at a time. Glyph
ranges, staging, and atlas packing stay in the host's renderer - mkwin never sees
them. A host with its own bitmap font leaves `MKWIN_FONT` undefined and links none
of this.

```c
struct mkwin_font;

struct mkwin_glyph_bitmap {
	int32_t width, height;      // coverage bitmap dimensions
	int32_t bearing_x, bearing_y;
	int32_t advance;            // pen advance in pixels
	const uint8_t *coverage;    // width*height 8-bit alpha, owned by the font
};

struct mkwin_font *mkwin_font_open(const char *path, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height);
void     mkwin_font_set_size(struct mkwin_font *font, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height);
uint32_t mkwin_font_glyph(struct mkwin_font *font, uint32_t codepoint, struct mkwin_glyph_bitmap *out);
void     mkwin_font_close(struct mkwin_font *font);
```

- **`mkwin_font_open`** opens `path`, or a discovered system sans-serif when `path`
  is `NULL`, at `pixel_size`. Writes ascent and line height (pixels) to the out
  params. Returns `NULL` on failure, in which case the host supplies fallback
  metrics. The font file can also be overridden with the `MKWIN_FONT_FILE`
  environment variable.
- **`mkwin_font_set_size`** re-opens the same font at a new pixel size.
- **`mkwin_font_glyph`** rasterizes one codepoint into `out`. The `coverage` bitmap
  is owned by the font and valid only until the next `mkwin_font_glyph` call - copy
  it into your atlas before rasterizing the next glyph. Returns non-zero on success.
- **`mkwin_font_close`** releases the font.

## Embedded OpenGL views (MKWIN_GLVIEW)

Compiled only when `MKWIN_GLVIEW` is defined. mkwin creates and places a native
child window; the host binds its own GL context to it using `mkwin_glview_handle`
together with `mkwin_display`.

```c
struct mkwin_glview;

struct mkwin_glview *mkwin_glview_create(struct mkwin_window *win, int32_t x, int32_t y, int32_t w, int32_t h);
void *mkwin_glview_handle(struct mkwin_glview *view);
void  mkwin_glview_reposition(struct mkwin_glview *view, int32_t x, int32_t y, int32_t w, int32_t h);
void  mkwin_glview_show(struct mkwin_glview *view, uint32_t visible);
void  mkwin_glview_destroy(struct mkwin_glview *view);
```

- **`mkwin_glview_create`** creates a child window at `x, y` sized `w, h` inside the
  parent window.
- **`mkwin_glview_handle`** returns the native child-window handle (X11 `Window` /
  Win32 `HWND`) as an opaque pointer - cast it back to bind a GL context. Returned
  as a pointer so a 64-bit `HWND` is never truncated.
- **`mkwin_glview_reposition`** moves/resizes the child.
- **`mkwin_glview_show`** shows or hides it.
- **`mkwin_glview_destroy`** destroys it.
