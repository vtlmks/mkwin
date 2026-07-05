// Copyright (c) 2026 Peter Fors
// SPDX-License-Identifier: MIT

#ifndef MKWIN_H
#define MKWIN_H

#include <stdint.h>
#include <stddef.h>

// mkwin is the window/platform layer: it owns the OS window, its shared-memory
// framebuffer, DPI scale, and the input/event pump. It knows nothing about
// widgets, layout, theme, icons, or text layout - those live in the host
// (mkgui, or a bespoke program such as a skinned media player).
//
// Consumption is source-level and single-TU: the host #includes mkwin.h for the
// API and #includes mkwin_<platform>.c exactly once into its own translation
// unit. MKWIN_API is therefore `static` by default; define MKWIN_LIBRARY to
// build mkwin as a standalone object with external linkage instead.

#ifdef MKWIN_LIBRARY
#define MKWIN_API
#else
#define MKWIN_API static
#endif

// Optional modules, compiled in only when the host asks for them:
//   MKWIN_FONT     - FreeType/GDI glyph rasterization into host-owned storage.
//   MKWIN_GLVIEW   - GLX/WGL child windows for embedded OpenGL views.
// A program that renders its own text (bitmap font, etc) and needs no GL leaves
// both undefined and links neither.

// ---------------------------------------------------------------------------
// Event model
// ---------------------------------------------------------------------------

// mkwin delivers input as neutral data the host pulls with mkwin_poll. It never
// calls back into host logic; the host decides what each event means.

enum {
	MKWIN_EV_NONE,
	MKWIN_EV_EXPOSE,
	MKWIN_EV_RESIZE,
	MKWIN_EV_MOTION,
	MKWIN_EV_BUTTON_PRESS,
	MKWIN_EV_BUTTON_RELEASE,
	MKWIN_EV_KEY,
	MKWIN_EV_CLOSE,
	MKWIN_EV_LEAVE,
	MKWIN_EV_FOCUS_OUT,
	MKWIN_EV_DROP,
};

// Keyboard modifier bits carried in mkwin_event.keymod.
#define MKWIN_MOD_SHIFT     (1u << 0)
#define MKWIN_MOD_CONTROL   (1u << 2)
#define MKWIN_MOD_ALT       (1u << 3)

struct mkwin_event {
	uint32_t type;
	int32_t x, y;
	int32_t width, height;
	uint32_t button;
	uint32_t keysym;
	uint32_t keymod;
	char text[32];
	int32_t text_len;
	int32_t popup_idx;
};

// ---------------------------------------------------------------------------
// Cursors and window styles
// ---------------------------------------------------------------------------

enum {
	MKWIN_CURSOR_DEFAULT,
	MKWIN_CURSOR_H_RESIZE,
	MKWIN_CURSOR_V_RESIZE,
};

// Window-creation flags. Only platform-visible styling lives here; higher-level
// semantics (hidden-on-startup, hide-on-close policy) stay in the host.
#define MKWIN_UNDECORATED   (1u << 0)

// ---------------------------------------------------------------------------
// Host-published state
// ---------------------------------------------------------------------------

// mkwin owns the framebuffer, dimensions and scale, but the host reads them on
// every frame. To spare a large host from routing thousands of reads through
// accessors, it may hand mkwin_open the addresses of its own fields; mkwin
// writes through them whenever the value changes (open, resize, DPI change).
// All pointers are optional - pass a zeroed struct to use the accessors below
// exclusively. The accessors are always valid regardless.
struct mkwin_host {
	uint32_t **out_fb;
	int32_t *out_w;
	int32_t *out_h;
	float *out_scale;
};

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

struct mkwin_icon_size {
	uint32_t *pixels;
	int32_t w, h;
};

// Opaque handles. Definitions live in mkwin_types_<platform>.h, included only
// by the implementation; consumers hold pointers.
struct mkwin_window;
struct mkwin_popup;

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

// The OS connection (X Display / Win32 message context) is opened lazily by the
// first mkwin_open and shared by every window; it is torn down when the last
// window is destroyed. parent == NULL makes a top-level window; non-NULL makes a
// transient/child of that window. host may be NULL.
MKWIN_API struct mkwin_window *mkwin_open(struct mkwin_window *parent, const char *title, int32_t w, int32_t h, uint32_t flags, const char *app_class, struct mkwin_host *host);
MKWIN_API void mkwin_map(struct mkwin_window *win);
MKWIN_API void mkwin_unmap(struct mkwin_window *win);
MKWIN_API void mkwin_destroy(struct mkwin_window *win);

// ---------------------------------------------------------------------------
// Framebuffer and presentation
// ---------------------------------------------------------------------------

MKWIN_API uint32_t *mkwin_framebuffer(struct mkwin_window *win);
MKWIN_API void mkwin_size(struct mkwin_window *win, int32_t *out_w, int32_t *out_h);
MKWIN_API float mkwin_scale(struct mkwin_window *win);
MKWIN_API void mkwin_present(struct mkwin_window *win);
MKWIN_API void mkwin_present_region(struct mkwin_window *win, int32_t x, int32_t y, int32_t w, int32_t h);
MKWIN_API void mkwin_flush(struct mkwin_window *win);
// Flush and wait for the server to finish processing (X11 XSync); no-op where
// presentation is synchronous (Win32).
MKWIN_API void mkwin_sync(struct mkwin_window *win);

// ---------------------------------------------------------------------------
// Event pump
// ---------------------------------------------------------------------------

// mkwin_poll fills ev and returns non-zero when an event was dequeued, zero when
// the queue is empty. mkwin_wait blocks on the shared OS fd/queue until input
// arrives or timeout_ns elapses (negative = block indefinitely).
MKWIN_API uint32_t mkwin_poll(struct mkwin_window *win, struct mkwin_event *ev);
MKWIN_API uint32_t mkwin_pending(struct mkwin_window *win);
MKWIN_API void mkwin_wait(struct mkwin_window *win, int64_t timeout_ns);

// Extra file descriptors folded into mkwin_wait alongside the OS connection, so
// a host timer (timerfd, eventfd, pipe) can wake an otherwise-idle loop. The
// host adds a descriptor when it starts watching it and removes it when done.
// On backends without pollable descriptors (Win32) these are inert, and the
// host must instead bound its mkwin_wait timeout by its own next deadline.
MKWIN_API void mkwin_wait_fd_add(int32_t fd);
MKWIN_API void mkwin_wait_fd_remove(int32_t fd);

// Registered by the host to keep animating during a platform modal move/resize
// loop, when the host's own event loop is suspended (Win32 sizing/moving). The
// callback fires roughly per-frame for the loop's duration. On X11 there is no
// such modal loop, so it never fires; hosts register it unconditionally.
MKWIN_API void mkwin_set_modal_frame_cb(struct mkwin_window *win, void (*cb)(struct mkwin_window *win, void *user), void *user);

// ---------------------------------------------------------------------------
// Geometry and window management
// ---------------------------------------------------------------------------

MKWIN_API void mkwin_move(struct mkwin_window *win, int32_t x, int32_t y);
MKWIN_API void mkwin_position(struct mkwin_window *win, int32_t *out_x, int32_t *out_y);
MKWIN_API void mkwin_resize(struct mkwin_window *win, int32_t w, int32_t h);
MKWIN_API void mkwin_set_min_size(struct mkwin_window *win, int32_t min_w, int32_t min_h);
MKWIN_API void mkwin_begin_drag(struct mkwin_window *win);
MKWIN_API void mkwin_set_shape(struct mkwin_window *win, const int32_t *points_xy, uint32_t point_count);
// Shape the window from the alpha channel of a w*h ARGB buffer: pixels whose
// alpha is >= alpha_threshold are kept, the rest are cut away. Row-span based,
// so a per-pixel skin mask maps directly onto the platform region primitive.
MKWIN_API void mkwin_set_shape_mask(struct mkwin_window *win, const uint32_t *argb, int32_t w, int32_t h, uint32_t alpha_threshold);
MKWIN_API void mkwin_clear_shape(struct mkwin_window *win);
MKWIN_API void mkwin_set_title(struct mkwin_window *win, const char *title);
// Non-zero when the window was created transient to a parent (dialog/child).
MKWIN_API uint32_t mkwin_is_child(struct mkwin_window *win);
MKWIN_API void mkwin_screen_size(struct mkwin_window *win, int32_t *out_w, int32_t *out_h);
MKWIN_API void mkwin_translate_coords(struct mkwin_window *win, int32_t local_x, int32_t local_y, int32_t *out_screen_x, int32_t *out_screen_y);

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

MKWIN_API void mkwin_set_cursor(struct mkwin_window *win, uint32_t cursor);
MKWIN_API void mkwin_set_class_hint(struct mkwin_window *win, const char *instance, const char *cls);
MKWIN_API void mkwin_set_icon(struct mkwin_window *win, const struct mkwin_icon_size *sizes, uint32_t count);

// ---------------------------------------------------------------------------
// Popups (override-redirect overlays: menus, tooltips, dropdowns)
// ---------------------------------------------------------------------------

MKWIN_API struct mkwin_popup *mkwin_popup_open(struct mkwin_window *parent, int32_t x, int32_t y, int32_t w, int32_t h);
MKWIN_API uint32_t *mkwin_popup_framebuffer(struct mkwin_popup *popup);
MKWIN_API void mkwin_popup_present(struct mkwin_popup *popup);
MKWIN_API void mkwin_popup_close(struct mkwin_popup *popup);

// ---------------------------------------------------------------------------
// Clipboard and drag-and-drop
// ---------------------------------------------------------------------------

MKWIN_API void mkwin_clipboard_set(struct mkwin_window *win, const char *text, uint32_t len);
// Returns a malloc'd, NUL-terminated buffer the caller frees; *out_len excludes
// the terminator. Returns NULL when the selection is empty or not text.
MKWIN_API char *mkwin_clipboard_get(struct mkwin_window *win, uint32_t *out_len);

// Advertise the window as an XDnD/OLE drop target. After an MKWIN_EV_DROP event
// the dropped file paths are readable until the next drop; the returned strings
// are owned by mkwin.
MKWIN_API void mkwin_drop_enable(struct mkwin_window *win);
MKWIN_API uint32_t mkwin_drop_count(struct mkwin_window *win);
MKWIN_API const char *mkwin_drop_path(struct mkwin_window *win, uint32_t index);

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

MKWIN_API uint64_t mkwin_now_ns(void);
MKWIN_API void mkwin_sleep_ns(uint64_t ns);

// ---------------------------------------------------------------------------
// Native-handle escape hatch
// ---------------------------------------------------------------------------

// The native connection handle (X11 Display* / Win32 HDC-less HINSTANCE cast to
// void*). Only host code that must talk to a native API directly - binding a GLX
// context to an mkwin_glview, say - needs this. Ordinary hosts never call it.
MKWIN_API void *mkwin_display(struct mkwin_window *win);

// ---------------------------------------------------------------------------
// Optional: font rasterization (MKWIN_FONT)
// ---------------------------------------------------------------------------

// mkwin exposes only the platform-specific primitive: open a font at a pixel
// size and rasterize one glyph at a time. Glyph-range policy, staging, and atlas
// packing stay in the host's renderer - mkwin never sees them. A host with its
// own bitmap font leaves MKWIN_FONT undefined and links none of this.
#ifdef MKWIN_FONT

struct mkwin_font;

// One rasterized glyph. `coverage` is an 8-bit alpha bitmap of width*height
// bytes, owned by the font and valid only until the next mkwin_font_glyph call.
struct mkwin_glyph_bitmap {
	int32_t width;
	int32_t height;
	int32_t bearing_x;
	int32_t bearing_y;
	int32_t advance;
	const uint8_t *coverage;
};

// Opens `path` (or a discovered system sans-serif when path is NULL) at
// pixel_size. Writes ascent and line height (both in pixels) to the out params.
// Returns NULL on failure, in which case the host supplies fallback metrics.
MKWIN_API struct mkwin_font *mkwin_font_open(const char *path, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height);
MKWIN_API void mkwin_font_set_size(struct mkwin_font *font, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height);
MKWIN_API uint32_t mkwin_font_glyph(struct mkwin_font *font, uint32_t codepoint, struct mkwin_glyph_bitmap *out);
MKWIN_API void mkwin_font_close(struct mkwin_font *font);

#endif // MKWIN_FONT

// ---------------------------------------------------------------------------
// Optional: embedded OpenGL views (MKWIN_GLVIEW)
// ---------------------------------------------------------------------------

// mkwin creates and places a native child window; the host binds its own GL
// context to it using mkwin_glview_xid together with mkwin_display.
#ifdef MKWIN_GLVIEW

struct mkwin_glview;

MKWIN_API struct mkwin_glview *mkwin_glview_create(struct mkwin_window *win, int32_t x, int32_t y, int32_t w, int32_t h);
// The native child-window handle (X11 Window / Win32 HWND) as an opaque pointer;
// the host casts it back to bind a GL context. Returned as a pointer so a 64-bit
// HWND is never truncated.
MKWIN_API void *mkwin_glview_handle(struct mkwin_glview *view);
MKWIN_API void mkwin_glview_reposition(struct mkwin_glview *view, int32_t x, int32_t y, int32_t w, int32_t h);
MKWIN_API void mkwin_glview_show(struct mkwin_glview *view, uint32_t visible);
MKWIN_API void mkwin_glview_destroy(struct mkwin_glview *view);

#endif // MKWIN_GLVIEW

#endif // MKWIN_H
