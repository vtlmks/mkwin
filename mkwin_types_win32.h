// Copyright (c) 2026 Peter Fors
// SPDX-License-Identifier: MIT

// Platform (Win32/GDI) type definitions for mkwin. Included after mkwin.h. A
// host that needs the native handle type (HWND for a GL binding) may include
// this directly; otherwise consumers hold opaque pointers to the handles below.

#ifndef MKWIN_TYPES_WIN32_H
#define MKWIN_TYPES_WIN32_H

#include <windows.h>
#include <shellapi.h>

#define MKWIN_MAX_WINDOWS   16
#define MKWIN_MAX_POPUPS    8
#define MKWIN_MAX_GLVIEWS   16
#define MKWIN_DROP_MAX      256
#define MKWIN_EVQ_SIZE      256

// An override-redirect overlay (menu/tooltip/dropdown). mkwin owns the window
// and its GDI DIB framebuffer.
struct mkwin_popup {
	struct mkwin_window *parent;
	HWND hwnd;
	HDC hdc_mem;
	HBITMAP hbmp;
	HBITMAP hbmp_old;
	uint32_t *pixels;
	int32_t x, y, w, h;
};

// An embedded child window for host-managed OpenGL. Its geometry and visibility
// are tracked so the main window's BitBlt can exclude the child's rectangle
// (GDI would otherwise paint over the GL surface).
struct mkwin_glview {
	struct mkwin_window *win;
	HWND hwnd;
	int32_t last_x, last_y, last_w, last_h;
	uint32_t visible;
};

#ifdef MKWIN_FONT
struct mkwin_font {
	HDC dc;
	HFONT handle;
	uint8_t *scratch;
	uint32_t scratch_cap;
};
#endif

// The window handle: OS window, its GDI DIB framebuffer, DPI scale, a per-window
// event queue filled by the WndProc, and the resources it owns.
struct mkwin_window {
	HWND hwnd;
	HDC hdc_mem;
	HBITMAP hbmp;
	HBITMAP hbmp_old;

	// Framebuffer (owned).
	uint32_t *pixels;
	int32_t win_w, win_h;
	float scale;

	// Cursors.
	HCURSOR cursor_default;
	HCURSOR cursor_h_resize;
	HCURSOR cursor_v_resize;
	uint32_t cursor_active;

	// Relationship / identity.
	struct mkwin_window *parent;
	HWND parent_hwnd;
	uint32_t is_child;
	uint32_t undecorated;
	char app_class[64];
	struct mkwin_host host;

	// Minimum client size, enforced in WM_GETMINMAXINFO.
	int32_t min_w, min_h;

	// Per-window event queue, produced by the WndProc, drained by mkwin_poll.
	struct mkwin_event evq_buf[MKWIN_EVQ_SIZE];
	uint32_t evq_head;
	uint32_t evq_tail;

	// Drag-and-drop (incoming files).
	char *drop_files[MKWIN_DROP_MAX];
	uint32_t drop_count;
	uint32_t drop_enabled;

	// Override-redirect overlays owned by this window, kept compact.
	struct mkwin_popup *popups[MKWIN_MAX_POPUPS];
	uint32_t popup_count;

	// GL child windows, tracked for blit exclusion.
	struct mkwin_glview *glviews[MKWIN_MAX_GLVIEWS];
	uint32_t glview_count;

	// Fires per-frame during a modal move/resize loop so the host keeps
	// animating while its own event loop is suspended.
	void (*modal_frame_cb)(struct mkwin_window *win, void *user);
	void *modal_frame_user;
};

#endif // MKWIN_TYPES_WIN32_H
