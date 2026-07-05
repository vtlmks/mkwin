// Copyright (c) 2026 Peter Fors
// SPDX-License-Identifier: MIT

// Platform (X11) type definitions for mkwin. Included after mkwin.h. A host that
// needs the native handle types (X11 Window/Display for a GL binding) may include
// this directly; otherwise consumers hold opaque pointers to the mkwin handles.

#ifndef MKWIN_TYPES_LINUX_H
#define MKWIN_TYPES_LINUX_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/shape.h>
#include <sys/shm.h>
#include <unistd.h>
#include <poll.h>
#include <sys/timerfd.h>

#ifdef MKWIN_FONT
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#endif

#define MKWIN_MAX_WINDOWS   16
#define MKWIN_MAX_POPUPS    8
#define MKWIN_MAX_WAIT_FDS  64
#define MKWIN_DROP_MAX      256
#define MKWIN_DEFERRED_SIZE 64

// Concurrent outgoing INCR clipboard transfers we can serve at once. One per
// requestor that pulls a selection too large for a single X request.
#define MKWIN_CLIP_INCR_MAX 8

// One in-flight INCR send: a snapshot of the selection data being streamed to
// a single requestor, chunk by chunk, as it deletes the property between
// chunks. data is owned (malloc'd) so a later clipboard change can't pull the
// rug out from under an active transfer.
struct mkwin_clip_incr {
	Window requestor;
	Atom property;
	Atom type;
	char *data;
	uint32_t len;
	uint32_t offset;
};

struct mkwin_atoms {
	Atom wm_delete;
	Atom clipboard;
	Atom utf8_string;
	Atom targets;
	Atom incr;
	Atom mkwin_clip_prop;
	Atom net_wm_pid;
	Atom xdnd_aware;
	Atom xdnd_enter;
	Atom xdnd_position;
	Atom xdnd_status;
	Atom xdnd_drop;
	Atom xdnd_finished;
	Atom xdnd_leave;
	Atom xdnd_action_copy;
	Atom xdnd_selection;
	Atom text_uri_list;
};

// An override-redirect overlay (menu/tooltip/dropdown). mkwin owns the X window
// and its shared-memory framebuffer; the host draws into mkwin_popup_framebuffer
// and calls mkwin_popup_present.
struct mkwin_popup {
	struct mkwin_window *parent;
	Window xwin;
	XShmSegmentInfo shm;
	XImage *img;
	uint32_t *pixels;
	int32_t x, y, w, h;
};

// An embedded child window for host-managed OpenGL. mkwin creates and places
// the X window; the host binds a GL context to it via mkwin_glview_handle +
// mkwin_display.
struct mkwin_glview {
	struct mkwin_window *win;
	Window xwin;
};

#ifdef MKWIN_FONT
struct mkwin_font {
	FT_Library lib;
	FT_Face face;
};
#endif

// The window handle: OS window, its shared-memory framebuffer, DPI scale, the
// per-window input/deferred-event state, and the resources it owns. Display-
// level handles (dpy/screen/root/visual/colormap/depth/atoms/xim) are opened by
// the first window and copied into every later window on the same connection.
struct mkwin_window {
	Display *dpy;
	Window root;
	Window win;
	int32_t screen;
	GC gc;
	Visual *visual;
	Colormap colormap;
	uint32_t depth;
	struct mkwin_atoms atoms;
	XIM xim;
	XIC xic;

	// Framebuffer (owned).
	XShmSegmentInfo shm;
	XImage *img;
	size_t shm_cap;
	uint32_t *pixels;
	int32_t win_w, win_h;
	float scale;

	// Relationship / identity.
	struct mkwin_window *parent;
	uint32_t is_child;
	char app_class[64];
	struct mkwin_host host;

	// Never fired on X11 (no modal move/resize loop); stored for API symmetry.
	void (*modal_frame_cb)(struct mkwin_window *win, void *user);
	void *modal_frame_user;

	// Cursors.
	Cursor cursor_default;
	Cursor cursor_h_resize;
	Cursor cursor_v_resize;
	uint32_t cursor_active;

	// Drag-and-drop (incoming files).
	Window xdnd_source;
	uint32_t xdnd_uri_ok;
	uint32_t drop_enabled;
	char *drop_files[MKWIN_DROP_MAX];
	uint32_t drop_count;

	// Clipboard.
	char *clip_text;
	uint32_t clip_len;
	uint32_t clip_cap;
	struct mkwin_clip_incr incr_send[MKWIN_CLIP_INCR_MAX];
	uint32_t incr_send_count;

	// Deferred events destined for this window, queued while another window is
	// the one being polled.
	struct mkwin_event deferred[MKWIN_DEFERRED_SIZE];
	uint32_t deferred_head;
	uint32_t deferred_tail;

	// Override-redirect overlays owned by this window, kept compact so an index
	// is stable for the host to mirror.
	struct mkwin_popup *popups[MKWIN_MAX_POPUPS];
	uint32_t popup_count;
};

#endif // MKWIN_TYPES_LINUX_H
