// Copyright (c) 2026 Peter Fors
// SPDX-License-Identifier: MIT

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mkwin.h"
#include "mkwin_types_linux.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Library state
// ---------------------------------------------------------------------------

// Every window on the shared X connection, so event routing can reach a window
// that is not the one currently being polled. The first window opens the
// Display; the last window closed tears it down.
static struct mkwin_window *g_windows[MKWIN_MAX_WINDOWS];
static uint32_t g_window_count;

// Extra descriptors folded into mkwin_wait beside the X connection (host timers).
static int32_t g_wait_fds[MKWIN_MAX_WAIT_FDS];
static uint32_t g_wait_fd_count;

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_sleep_ns ]======================================[=]
MKWIN_API void mkwin_sleep_ns(uint64_t ns) {
	struct timespec ts;
	ts.tv_sec  = (time_t)(ns / 1000000000ull);
	ts.tv_nsec = (long)(ns % 1000000000ull);
	nanosleep(&ts, NULL);
}

// [=]===^=[ mkwin_now_ns ]========================================[=]
// Monotonic clock in nanoseconds. Same epoch the library uses internally, so
// host deadlines and library deadlines are commensurable.
MKWIN_API uint64_t mkwin_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Window registry
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_register ]======================================[=]
static void mkwin_register(struct mkwin_window *win) {
	if(g_window_count < MKWIN_MAX_WINDOWS) {
		g_windows[g_window_count++] = win;
	}
}

// [=]===^=[ mkwin_unregister ]====================================[=]
static void mkwin_unregister(struct mkwin_window *win) {
	for(uint32_t i = 0; i < g_window_count; ++i) {
		if(g_windows[i] == win) {
			for(uint32_t j = i; j + 1 < g_window_count; ++j) {
				g_windows[j] = g_windows[j + 1];
			}
			--g_window_count;
			return;
		}
	}
}

// [=]===^=[ mkwin_publish ]=======================================[=]
// Push the owned framebuffer pointer, dimensions and scale into the host-
// designated fields, if the host registered any. Called on open, resize and
// scale change so the host's cached view always matches mkwin's truth.
static void mkwin_publish(struct mkwin_window *win) {
	if(win->host.out_fb) {
		*win->host.out_fb = win->pixels;
	}
	if(win->host.out_w) {
		*win->host.out_w = win->win_w;
	}
	if(win->host.out_h) {
		*win->host.out_h = win->win_h;
	}
	if(win->host.out_scale) {
		*win->host.out_scale = win->scale;
	}
}

// ---------------------------------------------------------------------------
// Drag-and-drop file list (incoming)
// ---------------------------------------------------------------------------

// [=]===^=[ drop_free ]==========================================[=]
static void drop_free(struct mkwin_window *win) {
	for(uint32_t i = 0; i < win->drop_count; ++i) {
		free(win->drop_files[i]);
		win->drop_files[i] = NULL;
	}
	win->drop_count = 0;
}

// [=]===^=[ drop_add_path ]======================================[=]
static void drop_add_path(struct mkwin_window *win, char *path) {
	if(win->drop_count >= MKWIN_DROP_MAX) {
		return;
	}
	uint32_t len = (uint32_t)strlen(path);
	char *copy = (char *)malloc(len + 1);
	if(!copy) {
		return;
	}
	memcpy(copy, path, len + 1);
	win->drop_files[win->drop_count++] = copy;
}

// [=]===^=[ drop_uri_decode ]====================================[=]
static void drop_uri_decode(char *dst, char *src, uint32_t dst_size) {
	uint32_t di = 0;
	for(uint32_t si = 0; src[si] && di + 1 < dst_size; ++si) {
		if(src[si] == '%' && src[si + 1] && src[si + 2]) {
			char hex[3] = { src[si + 1], src[si + 2], 0 };
			dst[di++] = (char)strtoul(hex, NULL, 16);
			si += 2;
		} else {
			dst[di++] = src[si];
		}
	}
	dst[di] = '\0';
}

// [=]===^=[ drop_parse_uri_list ]================================[=]
// Scratch line buffers are file-scope: the single UI thread parses one drop at
// a time, and 8 KiB does not belong on the stack.
static char drop_line[4096];
static char drop_decoded[4096];
static void drop_parse_uri_list(struct mkwin_window *win, char *data, uint32_t len) {
	drop_free(win);
	char *p = data;
	char *end = data + len;
	while(p < end) {
		char *eol = p;
		while(eol < end && *eol != '\n' && *eol != '\r') {
			++eol;
		}
		uint32_t line_len = (uint32_t)(eol - p);
		if(line_len > 0 && p[0] != '#') {
			if(line_len >= sizeof(drop_line)) {
				line_len = sizeof(drop_line) - 1;
			}
			memcpy(drop_line, p, line_len);
			drop_line[line_len] = '\0';
			if(line_len > 7 && strncmp(drop_line, "file://", 7) == 0) {
				drop_uri_decode(drop_decoded, drop_line + 7, sizeof(drop_decoded));
				drop_add_path(win, drop_decoded);
			} else {
				drop_add_path(win, drop_line);
			}
		}
		p = eol;
		while(p < end && (*p == '\n' || *p == '\r')) {
			++p;
		}
	}
}

// [=]===^=[ mkwin_drop_enable ]===================================[=]
MKWIN_API void mkwin_drop_enable(struct mkwin_window *win) {
	win->drop_enabled = 1;
	Atom version = 5;
	XChangeProperty(win->dpy, win->win, win->atoms.xdnd_aware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&version, 1);
}

// [=]===^=[ mkwin_drop_count ]====================================[=]
MKWIN_API uint32_t mkwin_drop_count(struct mkwin_window *win) {
	return win->drop_count;
}

// [=]===^=[ mkwin_drop_path ]=====================================[=]
MKWIN_API const char *mkwin_drop_path(struct mkwin_window *win, uint32_t index) {
	if(index >= win->drop_count) {
		return NULL;
	}
	return win->drop_files[index];
}

// ---------------------------------------------------------------------------
// Framebuffer helpers
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_fb_create ]=====================================[=]
static uint32_t mkwin_fb_create(struct mkwin_window *win, XShmSegmentInfo *shm, XImage **img, uint32_t **pixels, int32_t w, int32_t h, size_t *out_cap) {
	*img = XShmCreateImage(win->dpy, win->visual, win->depth, ZPixmap, NULL, shm, (uint32_t)w, (uint32_t)h);
	if(!*img) {
		return 0;
	}
	size_t bytes = (size_t)((*img)->bytes_per_line * h);
	shm->shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
	if(shm->shmid < 0) {
		XDestroyImage(*img);
		*img = NULL;
		return 0;
	}
	shm->shmaddr = (char *)shmat(shm->shmid, NULL, 0);
	memset(shm->shmaddr, 0, bytes);
	(*img)->data = shm->shmaddr;
	shm->readOnly = False;
	XShmAttach(win->dpy, shm);
	XSync(win->dpy, False);
	*pixels = (uint32_t *)shm->shmaddr;
	if(out_cap) {
		*out_cap = bytes;
	}
	return 1;
}

// [=]===^=[ mkwin_fb_destroy ]====================================[=]
static void mkwin_fb_destroy(struct mkwin_window *win, XShmSegmentInfo *shm, XImage *img) {
	if(!img) {
		return;
	}
	XShmDetach(win->dpy, shm);
	XSync(win->dpy, False);
	shmdt(shm->shmaddr);
	shmctl(shm->shmid, IPC_RMID, NULL);
	img->data = NULL;
	XDestroyImage(img);
}

// ---------------------------------------------------------------------------
// WM_CLASS and window icon
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_set_class_hint ]================================[=]
MKWIN_API void mkwin_set_class_hint(struct mkwin_window *win, const char *instance, const char *cls) {
	XClassHint hint;
	hint.res_name = (char *)instance;
	hint.res_class = (char *)cls;
	XSetClassHint(win->dpy, win->win, &hint);
}

// [=]===^=[ mkwin_set_icon ]======================================[=]
MKWIN_API void mkwin_set_icon(struct mkwin_window *win, const struct mkwin_icon_size *sizes, uint32_t count) {
	Atom net_wm_icon = XInternAtom(win->dpy, "_NET_WM_ICON", False);
	size_t total = 0;
	for(uint32_t s = 0; s < count; ++s) {
		total += 2 + (size_t)(sizes[s].w * sizes[s].h);
	}
	unsigned long *buf = (unsigned long *)malloc(total * sizeof(unsigned long));
	if(!buf) {
		return;
	}
	size_t off = 0;
	for(uint32_t s = 0; s < count; ++s) {
		uint32_t npx = (uint32_t)(sizes[s].w * sizes[s].h);
		buf[off++] = (unsigned long)sizes[s].w;
		buf[off++] = (unsigned long)sizes[s].h;
		for(uint32_t i = 0; i < npx; ++i) {
			buf[off++] = (unsigned long)sizes[s].pixels[i];
		}
	}
	XChangeProperty(win->dpy, win->win, net_wm_icon, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)buf, (int)total);
	XFlush(win->dpy);
	free(buf);
}

// ---------------------------------------------------------------------------
// DPI scale detection
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_detect_scale ]==================================[=]
static float mkwin_detect_scale(struct mkwin_window *win) {
	char *env = getenv("MKWIN_SCALE");
	if(env) {
		char *end = NULL;
		double val = strtod(env, &end);
		if(end && end != env && val > 0.0) {
			if(end[0] == '%') {
				val /= 100.0;
			} else if((end[0] == 'd' || end[0] == 'D') && (end[1] == 'p' || end[1] == 'P') && (end[2] == 'i' || end[2] == 'I')) {
				val /= 96.0;
			}

			if(val >= 0.5 && val <= 4.0) {
				return (float)val;
			}
		}
	}

	Display *dpy = win->dpy;

	XrmInitialize();
	char *rms = XResourceManagerString(dpy);
	if(rms) {
		XrmDatabase db = XrmGetStringDatabase(rms);
		if(db) {
			char *type = NULL;
			XrmValue val;
			if(XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val)) {
				if(type && strcmp(type, "String") == 0 && val.addr) {
					double dpi = atof(val.addr);
					if(dpi > 48.0 && dpi < 960.0) {
						XrmDestroyDatabase(db);
						return (float)(dpi / 96.0);
					}
				}
			}
			XrmDestroyDatabase(db);
		}
	}

	int32_t width_px = DisplayWidth(dpy, win->screen);
	int32_t width_mm = DisplayWidthMM(dpy, win->screen);
	if(width_mm > 0) {
		float dpi = (float)width_px * 25.4f / (float)width_mm;
		if(dpi > 48.0f && dpi < 960.0f) {
			float scale = dpi / 96.0f;
			if(scale >= 1.2f) {
				return scale;
			}
		}
	}

	return 1.0f;
}

// ---------------------------------------------------------------------------
// Framebuffer resize
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_fb_resize ]=====================================[=]
// The software renderer tight-packs the framebuffer at stride = width, so the
// shared segment only has to hold bytes_per_line * height. Recreating it costs a
// shmget/shmat plus two XShmAttach/Detach round-trips (~3.5ms), which is fatal
// when a high-rate mouse floods ConfigureNotify during an interactive resize. So
// keep the segment across resizes and reallocate only when the new frame no
// longer fits, over-allocating 1.5x so a continuous drag grows a handful of
// times instead of once per event. The XImage header is client-side and cheap
// to swap, so it is rebuilt every time to carry the new dimensions.
static void mkwin_fb_resize(struct mkwin_window *win) {
	if(win->win_w <= 0 || win->win_h <= 0) {
		return;
	}
	XImage *img = XShmCreateImage(win->dpy, win->visual, win->depth, ZPixmap, NULL, &win->shm, (uint32_t)win->win_w, (uint32_t)win->win_h);
	if(!img) {
		return;
	}
	size_t need = (size_t)(img->bytes_per_line * win->win_h);
	if(win->img) {
		win->img->data = NULL;
		XDestroyImage(win->img);
		win->img = NULL;
	}
	if(need > win->shm_cap) {
		if(win->shm_cap) {
			XShmDetach(win->dpy, &win->shm);
			XSync(win->dpy, False);
			shmdt(win->shm.shmaddr);
			shmctl(win->shm.shmid, IPC_RMID, NULL);
			win->shm_cap = 0;
		}
		size_t cap = need + need / 2;
		win->shm.shmid = shmget(IPC_PRIVATE, cap, IPC_CREAT | 0600);
		if(win->shm.shmid < 0) {
			XDestroyImage(img);
			return;
		}
		win->shm.shmaddr = (char *)shmat(win->shm.shmid, NULL, 0);
		win->shm.readOnly = False;
		XShmAttach(win->dpy, &win->shm);
		XSync(win->dpy, False);
		win->shm_cap = cap;
	}
	img->data = win->shm.shmaddr;
	win->img = img;
	win->pixels = (uint32_t *)win->shm.shmaddr;
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_open ]==========================================[=]
// The first window on the process opens and owns the shared Display, visual,
// colormap, atoms and input method; every later window copies those handles.
// parent != NULL makes a transient/dialog of that window.
MKWIN_API struct mkwin_window *mkwin_open(struct mkwin_window *parent, const char *title, int32_t w, int32_t h, uint32_t flags, const char *app_class, struct mkwin_host *host) {
	struct mkwin_window *win = (struct mkwin_window *)calloc(1, sizeof(*win));
	if(!win) {
		return NULL;
	}
	if(host) {
		win->host = *host;
	}
	win->parent = parent;
	win->is_child = parent ? 1 : 0;
	if(app_class && app_class[0]) {
		strncpy(win->app_class, app_class, sizeof(win->app_class) - 1);
	}

	if(g_window_count == 0) {
		win->dpy = XOpenDisplay(NULL);
		if(!win->dpy) {
			free(win);
			return NULL;
		}
		win->screen = DefaultScreen(win->dpy);
		win->root = RootWindow(win->dpy, win->screen);

		XVisualInfo vinfo;
		if(XMatchVisualInfo(win->dpy, win->screen, 32, TrueColor, &vinfo)) {
			win->visual = vinfo.visual;
			win->depth = 32;
		} else {
			win->visual = DefaultVisual(win->dpy, win->screen);
			win->depth = (uint32_t)DefaultDepth(win->dpy, win->screen);
		}
		win->colormap = XCreateColormap(win->dpy, win->root, win->visual, AllocNone);

		win->atoms.wm_delete = XInternAtom(win->dpy, "WM_DELETE_WINDOW", False);
		win->atoms.clipboard = XInternAtom(win->dpy, "CLIPBOARD", False);
		win->atoms.utf8_string = XInternAtom(win->dpy, "UTF8_STRING", False);
		win->atoms.targets = XInternAtom(win->dpy, "TARGETS", False);
		win->atoms.incr = XInternAtom(win->dpy, "INCR", False);
		win->atoms.mkwin_clip_prop = XInternAtom(win->dpy, "MKWIN_CLIP", False);
		win->atoms.net_wm_pid = XInternAtom(win->dpy, "_NET_WM_PID", False);
		win->atoms.xdnd_aware = XInternAtom(win->dpy, "XdndAware", False);
		win->atoms.xdnd_enter = XInternAtom(win->dpy, "XdndEnter", False);
		win->atoms.xdnd_position = XInternAtom(win->dpy, "XdndPosition", False);
		win->atoms.xdnd_status = XInternAtom(win->dpy, "XdndStatus", False);
		win->atoms.xdnd_drop = XInternAtom(win->dpy, "XdndDrop", False);
		win->atoms.xdnd_finished = XInternAtom(win->dpy, "XdndFinished", False);
		win->atoms.xdnd_leave = XInternAtom(win->dpy, "XdndLeave", False);
		win->atoms.xdnd_action_copy = XInternAtom(win->dpy, "XdndActionCopy", False);
		win->atoms.xdnd_selection = XInternAtom(win->dpy, "XdndSelection", False);
		win->atoms.text_uri_list = XInternAtom(win->dpy, "text/uri-list", False);

		win->xim = XOpenIM(win->dpy, NULL, NULL, NULL);
	} else {
		struct mkwin_window *share = g_windows[0];
		win->dpy = share->dpy;
		win->screen = share->screen;
		win->root = share->root;
		win->visual = share->visual;
		win->colormap = share->colormap;
		win->depth = share->depth;
		win->atoms = share->atoms;
		win->xim = share->xim;
	}

	XSetWindowAttributes wa;
	wa.background_pixmap = None;
	wa.bit_gravity = NorthWestGravity;
	wa.colormap = win->colormap;
	wa.border_pixel = 0;
	win->win = XCreateWindow(win->dpy, win->root, 0, 0, (uint32_t)w, (uint32_t)h, 0, (int)win->depth, InputOutput, win->visual, CWBackPixmap | CWBitGravity | CWColormap | CWBorderPixel, &wa);
	XStoreName(win->dpy, win->win, title);
	mkwin_set_class_hint(win, parent ? "dialog" : "main", win->app_class[0] ? win->app_class : "mkwin");

	if(parent && !(flags & MKWIN_UNDECORATED)) {
		XSetTransientForHint(win->dpy, win->win, parent->win);
	}

	XSetWMProtocols(win->dpy, win->win, &win->atoms.wm_delete, 1);

	if(flags & MKWIN_UNDECORATED) {
		struct { unsigned long flags, functions, decorations; long input_mode; unsigned long status; } mwm = { 2, 0, 0, 0, 0 };
		Atom motif_wm_hints = XInternAtom(win->dpy, "_MOTIF_WM_HINTS", False);
		XChangeProperty(win->dpy, win->win, motif_wm_hints, motif_wm_hints, 32, PropModeReplace, (unsigned char *)&mwm, 5);
	}

	pid_t pid = getpid();
	XChangeProperty(win->dpy, win->win, win->atoms.net_wm_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	XSetWMClientMachine(win->dpy, win->win, &(XTextProperty){ .value = (unsigned char *)"localhost", .encoding = XA_STRING, .format = 8, .nitems = 9 });

	XSelectInput(win->dpy, win->win, ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask | KeyReleaseMask | FocusChangeMask | PropertyChangeMask);

	win->gc = XCreateGC(win->dpy, win->win, 0, NULL);

	win->win_w = w;
	win->win_h = h;

	mkwin_fb_create(win, &win->shm, &win->img, &win->pixels, w, h, &win->shm_cap);

	{
		XSizeHints hints = {0};
		hints.flags = PMinSize;
		if(flags & MKWIN_UNDECORATED) {
			hints.min_width = 1;
			hints.min_height = 1;
		} else {
			hints.min_width = 200;
			hints.min_height = 100;
		}
		XSetWMNormalHints(win->dpy, win->win, &hints);
	}

	win->cursor_default = XCreateFontCursor(win->dpy, XC_left_ptr);
	win->cursor_h_resize = XCreateFontCursor(win->dpy, XC_sb_h_double_arrow);
	win->cursor_v_resize = XCreateFontCursor(win->dpy, XC_sb_v_double_arrow);
	win->cursor_active = MKWIN_CURSOR_DEFAULT;

	if(win->xim) {
		win->xic = XCreateIC(win->xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win->win, XNFocusWindow, win->win, NULL);
	}

	win->scale = mkwin_detect_scale(win);

	XFlush(win->dpy);

	mkwin_register(win);
	mkwin_publish(win);
	return win;
}

// [=]===^=[ mkwin_map ]===========================================[=]
MKWIN_API void mkwin_map(struct mkwin_window *win) {
	XMapWindow(win->dpy, win->win);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_unmap ]=========================================[=]
MKWIN_API void mkwin_unmap(struct mkwin_window *win) {
	XUnmapWindow(win->dpy, win->win);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_destroy ]=======================================[=]
// Per-window teardown; when the last window on the connection goes, the shared
// input method and Display are closed too.
MKWIN_API void mkwin_destroy(struct mkwin_window *win) {
	for(uint32_t i = 0; i < win->incr_send_count; ++i) {
		free(win->incr_send[i].data);
	}
	win->incr_send_count = 0;
	drop_free(win);
	free(win->clip_text);

	mkwin_fb_destroy(win, &win->shm, win->img);
	win->img = NULL;
	win->shm_cap = 0;
	XFreeCursor(win->dpy, win->cursor_default);
	XFreeCursor(win->dpy, win->cursor_h_resize);
	XFreeCursor(win->dpy, win->cursor_v_resize);
	if(win->xic) {
		XDestroyIC(win->xic);
		win->xic = NULL;
	}
	XFreeGC(win->dpy, win->gc);
	XDestroyWindow(win->dpy, win->win);

	mkwin_unregister(win);

	if(g_window_count == 0) {
		if(win->xim) {
			XCloseIM(win->xim);
		}
		if(win->dpy) {
			XCloseDisplay(win->dpy);
		}
	}

	free(win);
}

// [=]===^=[ mkwin_set_cursor ]====================================[=]
MKWIN_API void mkwin_set_cursor(struct mkwin_window *win, uint32_t cursor) {
	if(win->cursor_active == cursor) {
		return;
	}
	win->cursor_active = cursor;
	Cursor c;
	switch(cursor) {
		case MKWIN_CURSOR_H_RESIZE: {
			c = win->cursor_h_resize;
		} break;

		case MKWIN_CURSOR_V_RESIZE: {
			c = win->cursor_v_resize;
		} break;

		default: {
			c = win->cursor_default;
		} break;
	}
	XDefineCursor(win->dpy, win->win, c);
}

// ---------------------------------------------------------------------------
// Geometry and window management
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_resize ]========================================[=]
MKWIN_API void mkwin_resize(struct mkwin_window *win, int32_t w, int32_t h) {
	XResizeWindow(win->dpy, win->win, (uint32_t)w, (uint32_t)h);
}

// [=]===^=[ mkwin_move ]==========================================[=]
// XMoveWindow is a hint that travels through the WM asynchronously; the server's
// reported position doesn't update until the WM has finished its reparenting /
// placement work. Poll briefly for the position to catch up so a callsite that
// follows with mkwin_position observes the move.
MKWIN_API void mkwin_move(struct mkwin_window *win, int32_t x, int32_t y) {
	XMoveWindow(win->dpy, win->win, x, y);
	XFlush(win->dpy);
	XSync(win->dpy, False);
	for(uint32_t i = 0; i < 40; ++i) {
		int32_t cx, cy;
		Window child;
		XTranslateCoordinates(win->dpy, win->win, win->root, 0, 0, &cx, &cy, &child);
		if(cx == x && cy == y) {
			break;
		}
		struct timespec ts = { 0, 5 * 1000 * 1000 };
		nanosleep(&ts, NULL);
		XSync(win->dpy, False);
	}
}

// [=]===^=[ mkwin_position ]======================================[=]
MKWIN_API void mkwin_position(struct mkwin_window *win, int32_t *out_x, int32_t *out_y) {
	Window child;
	int32_t x, y;
	XTranslateCoordinates(win->dpy, win->win, win->root, 0, 0, &x, &y, &child);
	*out_x = x;
	*out_y = y;
}

// [=]===^=[ mkwin_begin_drag ]====================================[=]
MKWIN_API void mkwin_begin_drag(struct mkwin_window *win) {
	Atom net_wm_moveresize = XInternAtom(win->dpy, "_NET_WM_MOVERESIZE", False);
	Window root_ret, child_ret;
	int32_t rx, ry, wx, wy;
	uint32_t mask;
	XQueryPointer(win->dpy, win->win, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
	XUngrabPointer(win->dpy, CurrentTime);
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = win->win;
	ev.xclient.message_type = net_wm_moveresize;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = rx;
	ev.xclient.data.l[1] = ry;
	ev.xclient.data.l[2] = 8;
	ev.xclient.data.l[3] = 1;
	ev.xclient.data.l[4] = 1;
	XSendEvent(win->dpy, win->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_set_shape ]=====================================[=]
MKWIN_API void mkwin_set_shape(struct mkwin_window *win, const int32_t *points_xy, uint32_t point_count) {
	XPoint *pts = (XPoint *)malloc(point_count * sizeof(XPoint));
	if(!pts) {
		return;
	}
	for(uint32_t i = 0; i < point_count; ++i) {
		pts[i].x = (short)points_xy[i * 2];
		pts[i].y = (short)points_xy[i * 2 + 1];
	}
	Region rgn = XPolygonRegion(pts, (int)point_count, WindingRule);
	XShapeCombineRegion(win->dpy, win->win, ShapeBounding, 0, 0, rgn, ShapeSet);
	XDestroyRegion(rgn);
	free(pts);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_set_shape_mask ]================================[=]
MKWIN_API void mkwin_set_shape_mask(struct mkwin_window *win, const uint32_t *argb, int32_t w, int32_t h, uint32_t alpha_threshold) {
	Pixmap mask = XCreatePixmap(win->dpy, win->root, (uint32_t)w, (uint32_t)h, 1);
	GC gc = XCreateGC(win->dpy, mask, 0, NULL);
	XSetForeground(win->dpy, gc, 0);
	XFillRectangle(win->dpy, mask, gc, 0, 0, (uint32_t)w, (uint32_t)h);
	XSetForeground(win->dpy, gc, 1);
	for(int32_t y = 0; y < h; ++y) {
		int32_t x = 0;
		while(x < w) {
			while(x < w && (argb[y * w + x] >> 24) < alpha_threshold) {
				++x;
			}
			int32_t x0 = x;
			while(x < w && (argb[y * w + x] >> 24) >= alpha_threshold) {
				++x;
			}
			if(x > x0) {
				XFillRectangle(win->dpy, mask, gc, x0, y, (uint32_t)(x - x0), 1);
			}
		}
	}
	XShapeCombineMask(win->dpy, win->win, ShapeBounding, 0, 0, mask, ShapeSet);
	XFreeGC(win->dpy, gc);
	XFreePixmap(win->dpy, mask);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_clear_shape ]===================================[=]
MKWIN_API void mkwin_clear_shape(struct mkwin_window *win) {
	XShapeCombineMask(win->dpy, win->win, ShapeBounding, 0, 0, None, ShapeSet);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_set_title ]=====================================[=]
MKWIN_API void mkwin_set_title(struct mkwin_window *win, const char *title) {
	XStoreName(win->dpy, win->win, title);
}

// [=]===^=[ mkwin_is_child ]======================================[=]
MKWIN_API uint32_t mkwin_is_child(struct mkwin_window *win) {
	return win->is_child;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_present ]=======================================[=]
MKWIN_API void mkwin_present(struct mkwin_window *win) {
	XShmPutImage(win->dpy, win->win, win->gc, win->img, 0, 0, 0, 0, (uint32_t)win->win_w, (uint32_t)win->win_h, False);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_present_region ]================================[=]
MKWIN_API void mkwin_present_region(struct mkwin_window *win, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(x < 0) {
		w += x;
		x = 0;
	}
	if(y < 0) {
		h += y;
		y = 0;
	}
	if(x + w > win->win_w) {
		w = win->win_w - x;
	}
	if(y + h > win->win_h) {
		h = win->win_h - y;
	}
	if(w <= 0 || h <= 0) {
		return;
	}
	XShmPutImage(win->dpy, win->win, win->gc, win->img, x, y, x, y, (uint32_t)w, (uint32_t)h, False);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_flush ]=========================================[=]
MKWIN_API void mkwin_flush(struct mkwin_window *win) {
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_sync ]==========================================[=]
MKWIN_API void mkwin_sync(struct mkwin_window *win) {
	XSync(win->dpy, False);
}

// ---------------------------------------------------------------------------
// Popup windows
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_popup_open ]====================================[=]
MKWIN_API struct mkwin_popup *mkwin_popup_open(struct mkwin_window *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(parent->popup_count >= MKWIN_MAX_POPUPS) {
		return NULL;
	}
	struct mkwin_popup *p = (struct mkwin_popup *)calloc(1, sizeof(*p));
	if(!p) {
		return NULL;
	}
	p->parent = parent;
	p->x = x;
	p->y = y;
	p->w = w;
	p->h = h;

	XSetWindowAttributes attrs;
	attrs.override_redirect = True;
	attrs.save_under = True;
	attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask;
	attrs.colormap = parent->colormap;
	attrs.border_pixel = 0;

	p->xwin = XCreateWindow(parent->dpy, parent->root, x, y, (uint32_t)w, (uint32_t)h, 0, (int)parent->depth, InputOutput, parent->visual, CWOverrideRedirect | CWSaveUnder | CWEventMask | CWColormap | CWBorderPixel, &attrs);
	if(!p->xwin) {
		free(p);
		return NULL;
	}

	mkwin_fb_create(parent, &p->shm, &p->img, &p->pixels, w, h, NULL);

	XMapRaised(parent->dpy, p->xwin);
	XFlush(parent->dpy);

	parent->popups[parent->popup_count++] = p;
	return p;
}

// [=]===^=[ mkwin_popup_framebuffer ]=============================[=]
MKWIN_API uint32_t *mkwin_popup_framebuffer(struct mkwin_popup *popup) {
	return popup->pixels;
}

// [=]===^=[ mkwin_popup_present ]=================================[=]
MKWIN_API void mkwin_popup_present(struct mkwin_popup *popup) {
	struct mkwin_window *parent = popup->parent;
	XShmPutImage(parent->dpy, popup->xwin, parent->gc, popup->img, 0, 0, 0, 0, (uint32_t)popup->w, (uint32_t)popup->h, False);
	XFlush(parent->dpy);
}

// [=]===^=[ mkwin_popup_close ]===================================[=]
MKWIN_API void mkwin_popup_close(struct mkwin_popup *popup) {
	struct mkwin_window *parent = popup->parent;
	if(popup->img) {
		mkwin_fb_destroy(parent, &popup->shm, popup->img);
		popup->img = NULL;
	}
	if(popup->xwin) {
		XDestroyWindow(parent->dpy, popup->xwin);
		popup->xwin = 0;
	}
	for(uint32_t i = 0; i < parent->popup_count; ++i) {
		if(parent->popups[i] == popup) {
			for(uint32_t j = i; j + 1 < parent->popup_count; ++j) {
				parent->popups[j] = parent->popups[j + 1];
			}
			--parent->popup_count;
			break;
		}
	}
	free(popup);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_framebuffer ]===================================[=]
MKWIN_API uint32_t *mkwin_framebuffer(struct mkwin_window *win) {
	return win->pixels;
}

// [=]===^=[ mkwin_size ]==========================================[=]
MKWIN_API void mkwin_size(struct mkwin_window *win, int32_t *out_w, int32_t *out_h) {
	if(out_w) {
		*out_w = win->win_w;
	}
	if(out_h) {
		*out_h = win->win_h;
	}
}

// [=]===^=[ mkwin_scale ]=========================================[=]
MKWIN_API float mkwin_scale(struct mkwin_window *win) {
	return win->scale;
}

// [=]===^=[ mkwin_display ]=======================================[=]
MKWIN_API void *mkwin_display(struct mkwin_window *win) {
	return win->dpy;
}

// [=]===^=[ mkwin_screen_size ]===================================[=]
MKWIN_API void mkwin_screen_size(struct mkwin_window *win, int32_t *out_w, int32_t *out_h) {
	*out_w = DisplayWidth(win->dpy, win->screen);
	*out_h = DisplayHeight(win->dpy, win->screen);
}

// [=]===^=[ mkwin_set_min_size ]==================================[=]
MKWIN_API void mkwin_set_min_size(struct mkwin_window *win, int32_t min_w, int32_t min_h) {
	XSizeHints hints = {0};
	hints.flags = PMinSize;
	hints.min_width = min_w;
	hints.min_height = min_h;
	XSetWMNormalHints(win->dpy, win->win, &hints);
}

// [=]===^=[ mkwin_translate_coords ]==============================[=]
MKWIN_API void mkwin_translate_coords(struct mkwin_window *win, int32_t local_x, int32_t local_y, int32_t *out_screen_x, int32_t *out_screen_y) {
	Window child;
	XTranslateCoordinates(win->dpy, win->win, win->root, local_x, local_y, out_screen_x, out_screen_y, &child);
}

// ---------------------------------------------------------------------------
// Event wait
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_wait ]==========================================[=]
// timeout_ns < 0: block indefinitely. == 0: return at once. > 0: wait up to ns.
MKWIN_API void mkwin_wait(struct mkwin_window *win, int64_t timeout_ns) {
	if(timeout_ns == 0) {
		return;
	}
	if(XEventsQueued(win->dpy, QueuedAlready) > 0) {
		return;
	}
	XFlush(win->dpy);
	struct pollfd pfds[1 + MKWIN_MAX_WAIT_FDS];
	uint32_t nfds = 0;
	pfds[nfds].fd = ConnectionNumber(win->dpy);
	pfds[nfds].events = POLLIN;
	++nfds;
	for(uint32_t i = 0; i < g_wait_fd_count; ++i) {
		pfds[nfds].fd = g_wait_fds[i];
		pfds[nfds].events = POLLIN;
		++nfds;
	}
	if(timeout_ns < 0) {
		ppoll(pfds, nfds, NULL, NULL);
	} else {
		struct timespec ts;
		ts.tv_sec = (time_t)(timeout_ns / 1000000000ll);
		ts.tv_nsec = (long)(timeout_ns % 1000000000ll);
		ppoll(pfds, nfds, &ts, NULL);
	}
	if(pfds[0].revents & POLLIN) {
		XEventsQueued(win->dpy, QueuedAfterReading);
	}
}

// [=]===^=[ mkwin_wait_fd_add ]===================================[=]
MKWIN_API void mkwin_wait_fd_add(int32_t fd) {
	for(uint32_t i = 0; i < g_wait_fd_count; ++i) {
		if(g_wait_fds[i] == fd) {
			return;
		}
	}
	if(g_wait_fd_count < MKWIN_MAX_WAIT_FDS) {
		g_wait_fds[g_wait_fd_count++] = fd;
	}
}

// [=]===^=[ mkwin_wait_fd_remove ]================================[=]
MKWIN_API void mkwin_wait_fd_remove(int32_t fd) {
	for(uint32_t i = 0; i < g_wait_fd_count; ++i) {
		if(g_wait_fds[i] == fd) {
			for(uint32_t j = i; j + 1 < g_wait_fd_count; ++j) {
				g_wait_fds[j] = g_wait_fds[j + 1];
			}
			--g_wait_fd_count;
			return;
		}
	}
}

// [=]===^=[ mkwin_set_modal_frame_cb ]============================[=]
// Stored but never fired on X11: there is no modal move/resize loop to animate
// through. Present so hosts register it identically on both backends.
MKWIN_API void mkwin_set_modal_frame_cb(struct mkwin_window *win, void (*cb)(struct mkwin_window *win, void *user), void *user) {
	win->modal_frame_cb = cb;
	win->modal_frame_user = user;
}

// ---------------------------------------------------------------------------
// Deferred event queue
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_deferred_push ]=================================[=]
static void mkwin_deferred_push(struct mkwin_window *win, struct mkwin_event *ev) {
	uint32_t next = (win->deferred_head + 1) % MKWIN_DEFERRED_SIZE;
	if(next == win->deferred_tail) {
		return;
	}
	win->deferred[win->deferred_head] = *ev;
	win->deferred_head = next;
}

// [=]===^=[ mkwin_deferred_pop ]==================================[=]
static uint32_t mkwin_deferred_pop(struct mkwin_window *win, struct mkwin_event *ev) {
	if(win->deferred_head == win->deferred_tail) {
		return 0;
	}
	*ev = win->deferred[win->deferred_tail];
	win->deferred_tail = (win->deferred_tail + 1) % MKWIN_DEFERRED_SIZE;
	return 1;
}

// [=]===^=[ mkwin_pending ]=======================================[=]
MKWIN_API uint32_t mkwin_pending(struct mkwin_window *win) {
	if(win->deferred_head != win->deferred_tail) {
		return 1;
	}
	if(XEventsQueued(win->dpy, QueuedAlready) > 0) {
		return 1;
	}
	return 0;
}

// [=]===^=[ mkwin_translate_keysym ]==============================[=]
static uint32_t mkwin_translate_keysym(KeySym ks) {
	switch(ks) {
		case 0xff8d: { return 0xff0d; } break; // KP_Enter  -> Return
		case 0xff89: { return 0xff09; } break; // KP_Tab    -> Tab
		case 0xff9f: { return 0xffff; } break; // KP_Delete -> Delete
		case 0xff95: { return 0xff50; } break; // KP_Home   -> Home
		case 0xff96: { return 0xff51; } break; // KP_Left   -> Left
		case 0xff97: { return 0xff52; } break; // KP_Up     -> Up
		case 0xff98: { return 0xff53; } break; // KP_Right  -> Right
		case 0xff99: { return 0xff54; } break; // KP_Down   -> Down
		case 0xff9a: { return 0xff55; } break; // KP_Prior  -> Prior
		case 0xff9b: { return 0xff56; } break; // KP_Next   -> Next
		case 0xff9c: { return 0xff57; } break; // KP_End    -> End
		default: break;
	}
	return (uint32_t)ks;
}

// ---------------------------------------------------------------------------
// File drag-and-drop (XDnd v5)
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_xdnd_handle ]===================================[=]
static uint32_t mkwin_xdnd_handle(struct mkwin_window *win, XClientMessageEvent *cm, struct mkwin_event *ev) {
	if(cm->message_type == win->atoms.xdnd_enter) {
		win->xdnd_source = (Window)cm->data.l[0];
		win->xdnd_uri_ok = 0;
		uint32_t has_type_list = (uint32_t)cm->data.l[1] & 1;
		if(has_type_list) {
			Atom type_ret;
			int32_t format;
			unsigned long count, remaining;
			unsigned char *data = NULL;
			XGetWindowProperty(win->dpy, win->xdnd_source, win->atoms.text_uri_list, 0, 256, False, XA_ATOM, &type_ret, &format, &count, &remaining, &data);
			if(!data) {
				Atom type_list_atom = XInternAtom(win->dpy, "XdndTypeList", False);
				XGetWindowProperty(win->dpy, win->xdnd_source, type_list_atom, 0, 256, False, XA_ATOM, &type_ret, &format, &count, &remaining, &data);
			}

			if(data) {
				Atom *types = (Atom *)data;
				for(unsigned long i = 0; i < count; ++i) {
					if(types[i] == win->atoms.text_uri_list) {
						win->xdnd_uri_ok = 1;
						break;
					}
				}
				XFree(data);
			}
		} else {
			for(uint32_t i = 0; i < 3; ++i) {
				if((Atom)cm->data.l[2 + i] == win->atoms.text_uri_list) {
					win->xdnd_uri_ok = 1;
					break;
				}
			}
		}
		return 0;
	}

	if(cm->message_type == win->atoms.xdnd_position) {
		XClientMessageEvent reply;
		memset(&reply, 0, sizeof(reply));
		reply.type = ClientMessage;
		reply.window = win->xdnd_source;
		reply.message_type = win->atoms.xdnd_status;
		reply.format = 32;
		reply.data.l[0] = (long)win->win;
		if(win->xdnd_uri_ok && win->drop_enabled) {
			reply.data.l[1] = 1;
			reply.data.l[4] = (long)win->atoms.xdnd_action_copy;
		}
		XSendEvent(win->dpy, win->xdnd_source, False, NoEventMask, (XEvent *)&reply);
		XFlush(win->dpy);
		return 0;
	}

	if(cm->message_type == win->atoms.xdnd_drop) {
		if(win->xdnd_uri_ok && win->drop_enabled) {
			XConvertSelection(win->dpy, win->atoms.xdnd_selection, win->atoms.text_uri_list, win->atoms.mkwin_clip_prop, win->win, (Time)cm->data.l[2]);
		} else {
			XClientMessageEvent fin;
			memset(&fin, 0, sizeof(fin));
			fin.type = ClientMessage;
			fin.window = win->xdnd_source;
			fin.message_type = win->atoms.xdnd_finished;
			fin.format = 32;
			fin.data.l[0] = (long)win->win;
			XSendEvent(win->dpy, win->xdnd_source, False, NoEventMask, (XEvent *)&fin);
			XFlush(win->dpy);
		}
		return 0;
	}

	if(cm->message_type == win->atoms.xdnd_leave) {
		win->xdnd_source = 0;
		win->xdnd_uri_ok = 0;
		return 0;
	}

	(void)ev;
	return 0;
}

// [=]===^=[ mkwin_xdnd_selection ]================================[=]
static uint32_t mkwin_xdnd_selection(struct mkwin_window *win, XSelectionEvent *se, struct mkwin_event *ev) {
	if(se->selection != win->atoms.xdnd_selection) {
		return 0;
	}

	uint32_t got_data = 0;
	if(se->property != None) {
		Atom type_ret;
		int32_t format;
		unsigned long count, remaining;
		unsigned char *data = NULL;
		XGetWindowProperty(win->dpy, win->win, se->property, 0, 65536, True, AnyPropertyType, &type_ret, &format, &count, &remaining, &data);
		if(data && count > 0) {
			drop_parse_uri_list(win, (char *)data, (uint32_t)count);
			got_data = (win->drop_count > 0);
		}

		if(data) {
			XFree(data);
		}
	}

	XClientMessageEvent fin;
	memset(&fin, 0, sizeof(fin));
	fin.type = ClientMessage;
	fin.window = win->xdnd_source;
	fin.message_type = win->atoms.xdnd_finished;
	fin.format = 32;
	fin.data.l[0] = (long)win->win;
	if(got_data) {
		fin.data.l[1] = 1;
		fin.data.l[2] = (long)win->atoms.xdnd_action_copy;
	}
	XSendEvent(win->dpy, win->xdnd_source, False, NoEventMask, (XEvent *)&fin);
	XFlush(win->dpy);

	win->xdnd_source = 0;
	win->xdnd_uri_ok = 0;

	if(got_data) {
		ev->type = MKWIN_EV_DROP;
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_clip_chunk_bytes ]==============================[=]
// Largest payload we put in a single XChangeProperty, derived from the server's
// max request size (with BIG-REQUESTS when available) minus header slack, and
// capped so a huge clipboard streams in bounded steps.
static uint32_t mkwin_clip_chunk_bytes(Display *dpy) {
	long units = XExtendedMaxRequestSize(dpy);
	if(units == 0) {
		units = XMaxRequestSize(dpy);
	}
	uint32_t bytes = (uint32_t)(units * 4);
	bytes = bytes > 4096 ? bytes - 4096 : bytes / 2;
	if(bytes > (1u << 20)) {
		bytes = 1u << 20;
	}
	return bytes;
}

// [=]===^=[ mkwin_clip_incr_begin ]===============================[=]
// Start an incremental selection transfer to `requestor`: snapshot the data,
// advertise the size via an INCR-typed property, and watch the requestor for the
// property deletions that pace the stream. Returns 0 if it cannot start (no free
// slot, alloc failure, or a same-process requestor whose event mask we must not
// clobber), leaving the caller to report transfer failure.
static uint32_t mkwin_clip_incr_begin(struct mkwin_window *win, Window requestor, Atom property, Atom target) {
	if(win->incr_send_count >= MKWIN_CLIP_INCR_MAX) {
		return 0;
	}
	for(uint32_t i = 0; i < g_window_count; ++i) {
		if(g_windows[i]->win == requestor) {
			return 0;
		}
	}
	char *copy = (char *)malloc(win->clip_len ? win->clip_len : 1);
	if(!copy) {
		return 0;
	}
	memcpy(copy, win->clip_text, win->clip_len);

	struct mkwin_clip_incr *e = &win->incr_send[win->incr_send_count++];
	e->requestor = requestor;
	e->property = property;
	e->type = target;
	e->data = copy;
	e->len = win->clip_len;
	e->offset = 0;

	XSelectInput(win->dpy, requestor, PropertyChangeMask);
	long lower = (long)e->len;
	XChangeProperty(win->dpy, requestor, property, win->atoms.incr, 32, PropModeReplace, (unsigned char *)&lower, 1);
	XFlush(win->dpy);
	return 1;
}

// [=]===^=[ mkwin_clip_incr_continue ]============================[=]
// A requestor deleted a property; if it matches an in-flight INCR send, push the
// next chunk (a final zero-length chunk ends the stream). Returns 1 if the event
// belonged to a transfer we are driving.
static uint32_t mkwin_clip_incr_continue(XPropertyEvent *pe) {
	for(uint32_t w = 0; w < g_window_count; ++w) {
		struct mkwin_window *win = g_windows[w];
		for(uint32_t i = 0; i < win->incr_send_count; ++i) {
			struct mkwin_clip_incr *e = &win->incr_send[i];
			if(e->requestor != pe->window || e->property != pe->atom) {
				continue;
			}
			uint32_t remaining = e->len - e->offset;
			uint32_t chunk = mkwin_clip_chunk_bytes(win->dpy);
			uint32_t n = remaining < chunk ? remaining : chunk;
			XChangeProperty(win->dpy, e->requestor, e->property, e->type, 8, PropModeReplace, (unsigned char *)(e->data + e->offset), (int)n);
			XFlush(win->dpy);
			e->offset += n;
			if(n == 0) {
				XSelectInput(win->dpy, e->requestor, NoEventMask);
				free(e->data);
				*e = win->incr_send[--win->incr_send_count];
			}
			return 1;
		}
	}
	return 0;
}

// [=]===^=[ mkwin_clip_drain_prop ]===============================[=]
// Read MKWIN_CLIP off our window in full, deleting as we go, and append it to
// *buf (grown as needed). A single property can exceed one request, so page
// through it; the delete that signals an INCR owner happens on the final read.
// Returns bytes appended (0 = empty property), or 0 with *failed set on OOM.
static uint32_t mkwin_clip_drain_prop(struct mkwin_window *win, char **buf, uint32_t *cap, uint32_t *len, uint32_t *failed) {
	uint32_t appended = 0;
	uint32_t offset = 0;
	for(;;) {
		Atom type;
		int format;
		unsigned long nitems, bytes_after;
		unsigned char *data = NULL;
		XGetWindowProperty(win->dpy, win->win, win->atoms.mkwin_clip_prop, offset, 1024 * 1024, True, AnyPropertyType, &type, &format, &nitems, &bytes_after, &data);
		if(!data) {
			break;
		}
		uint32_t n = (uint32_t)nitems * ((uint32_t)format / 8);
		if(n > 0) {
			if(*len + n + 1 > *cap) {
				uint32_t nc = *cap ? *cap : 4096;
				while(nc < *len + n + 1) {
					nc *= 2;
				}
				char *nb = (char *)realloc(*buf, nc);
				if(!nb) {
					XFree(data);
					*failed = 1;
					break;
				}
				*buf = nb;
				*cap = nc;
			}
			memcpy(*buf + *len, data, n);
			*len += n;
			appended += n;
		}
		XFree(data);
		if(bytes_after == 0) {
			break;
		}
		offset += n / 4;
	}
	return appended;
}

// [=]===^=[ mkwin_clip_recv_incr ]================================[=]
// Receive an incremental selection: the owner appends one chunk per property
// delete we issue, terminating with a zero-length chunk. Returns the malloc'd,
// NUL-terminated payload (NULL on timeout/OOM/empty).
static char *mkwin_clip_recv_incr(struct mkwin_window *win, uint32_t *out_len) {
	char *buf = NULL;
	uint32_t cap = 0;
	uint32_t len = 0;

	for(;;) {
		XEvent xev;
		uint32_t got = 0;
		for(uint32_t i = 0; i < 1000; ++i) {
			if(XCheckTypedWindowEvent(win->dpy, win->win, PropertyNotify, &xev)) {
				if(xev.xproperty.atom == win->atoms.mkwin_clip_prop && xev.xproperty.state == PropertyNewValue) {
					got = 1;
					break;
				}
				continue;
			}
			struct timespec ts = {0, 10000000};
			nanosleep(&ts, NULL);
		}
		if(!got) {
			free(buf);
			return NULL;
		}

		uint32_t failed = 0;
		uint32_t n = mkwin_clip_drain_prop(win, &buf, &cap, &len, &failed);
		if(failed) {
			free(buf);
			return NULL;
		}
		if(n == 0) {
			break;
		}
	}

	if(!buf) {
		return NULL;
	}
	buf[len] = '\0';
	*out_len = len;
	return buf;
}

// [=]===^=[ mkwin_clipboard_set ]=================================[=]
MKWIN_API void mkwin_clipboard_set(struct mkwin_window *win, const char *text, uint32_t len) {
	if(len + 1 > win->clip_cap) {
		uint32_t cap = win->clip_cap ? win->clip_cap : 256;
		while(cap < len + 1) {
			cap *= 2;
		}
		char *nt = (char *)realloc(win->clip_text, cap);
		if(!nt) {
			return;
		}
		win->clip_text = nt;
		win->clip_cap = cap;
	}
	memcpy(win->clip_text, text, len);
	win->clip_text[len] = '\0';
	win->clip_len = len;
	XSetSelectionOwner(win->dpy, win->atoms.clipboard, win->win, CurrentTime);
	XFlush(win->dpy);
}

// [=]===^=[ mkwin_clipboard_get ]=================================[=]
MKWIN_API char *mkwin_clipboard_get(struct mkwin_window *win, uint32_t *out_len) {
	*out_len = 0;

	if(XGetSelectionOwner(win->dpy, win->atoms.clipboard) == win->win) {
		uint32_t len = win->clip_len;
		char *buf = (char *)malloc(len + 1);
		if(!buf) {
			return NULL;
		}
		memcpy(buf, win->clip_text, len);
		buf[len] = '\0';
		*out_len = len;
		return buf;
	}

	XConvertSelection(win->dpy, win->atoms.clipboard, win->atoms.utf8_string, win->atoms.mkwin_clip_prop, win->win, CurrentTime);
	XFlush(win->dpy);

	XEvent xev;
	for(uint32_t i = 0; i < 50; ++i) {
		if(XCheckTypedWindowEvent(win->dpy, win->win, SelectionNotify, &xev)) {
			if(xev.xselection.property == None) {
				return NULL;
			}
			// Peek the type: an INCR property means the data arrives in chunks
			// paced by us deleting the property; anything else is inline.
			Atom type;
			int format;
			unsigned long nitems, bytes_after;
			unsigned char *probe = NULL;
			XGetWindowProperty(win->dpy, win->win, win->atoms.mkwin_clip_prop, 0, 0, False, AnyPropertyType, &type, &format, &nitems, &bytes_after, &probe);
			if(probe) {
				XFree(probe);
			}
			if(type == win->atoms.incr) {
				// The XChangeProperty that delivered the INCR atom already queued
				// a PropertyNotify(NewValue); discard any such pending events so
				// recv_incr does not mistake the (now-empty) INCR property for the
				// terminating zero-length chunk. Data chunks only start arriving
				// after we delete the property below.
				XEvent stale;
				while(XCheckTypedWindowEvent(win->dpy, win->win, PropertyNotify, &stale)) {
				}
				XDeleteProperty(win->dpy, win->win, win->atoms.mkwin_clip_prop);
				XFlush(win->dpy);
				return mkwin_clip_recv_incr(win, out_len);
			}

			char *buf = NULL;
			uint32_t cap = 0;
			uint32_t len = 0;
			uint32_t failed = 0;
			mkwin_clip_drain_prop(win, &buf, &cap, &len, &failed);
			if(failed || !buf) {
				free(buf);
				return NULL;
			}
			buf[len] = '\0';
			*out_len = len;
			return buf;
		}
		struct timespec ts = {0, 10000000};
		nanosleep(&ts, NULL);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Event translation and routing
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_translate_xevent ]==============================[=]
static void mkwin_translate_xevent(struct mkwin_window *owner, XEvent *xev, struct mkwin_event *ev) {
	switch(xev->type) {
		case Expose: {
			ev->type = MKWIN_EV_EXPOSE;
		} break;

		case ConfigureNotify: {
			if(xev->xconfigure.window == owner->win) {
				while(XEventsQueued(owner->dpy, QueuedAlready) > 0) {
					XEvent peek;
					XPeekEvent(owner->dpy, &peek);
					if(peek.type != ConfigureNotify || peek.xconfigure.window != xev->xconfigure.window) {
						break;
					}
					XNextEvent(owner->dpy, xev);
				}
				owner->win_w = xev->xconfigure.width;
				owner->win_h = xev->xconfigure.height;
				mkwin_fb_resize(owner);
				mkwin_publish(owner);
				ev->type = MKWIN_EV_RESIZE;
				ev->width = owner->win_w;
				ev->height = owner->win_h;
			}
		} break;

		case MotionNotify: {
			while(XEventsQueued(owner->dpy, QueuedAlready) > 0) {
				XEvent peek;
				XPeekEvent(owner->dpy, &peek);
				if(peek.type != MotionNotify || peek.xmotion.window != xev->xmotion.window) {
					break;
				}
				XNextEvent(owner->dpy, xev);
			}
			ev->type = MKWIN_EV_MOTION;
			ev->x = xev->xmotion.x;
			ev->y = xev->xmotion.y;
		} break;

		case ButtonPress: {
			ev->type = MKWIN_EV_BUTTON_PRESS;
			ev->x = xev->xbutton.x;
			ev->y = xev->xbutton.y;
			ev->button = xev->xbutton.button;
			ev->keymod = xev->xbutton.state;
		} break;

		case ButtonRelease: {
			ev->type = MKWIN_EV_BUTTON_RELEASE;
			ev->x = xev->xbutton.x;
			ev->y = xev->xbutton.y;
			ev->button = xev->xbutton.button;
		} break;

		case KeyPress: {
			char buf[32];
			KeySym ks;
			int32_t len;
			if(owner->xic) {
				Status status;
				len = Xutf8LookupString(owner->xic, &xev->xkey, buf, sizeof(buf) - 1, &ks, &status);
			} else {
				len = XLookupString(&xev->xkey, buf, sizeof(buf) - 1, &ks, NULL);
			}
			buf[len] = '\0';

			ev->type = MKWIN_EV_KEY;
			ev->keysym = mkwin_translate_keysym(ks);
			ev->keymod = xev->xkey.state;
			memcpy(ev->text, buf, (uint32_t)(len + 1));
			ev->text_len = len;
		} break;

		case ClientMessage: {
			if((Atom)xev->xclient.data.l[0] == owner->atoms.wm_delete) {
				ev->type = MKWIN_EV_CLOSE;
			} else {
				mkwin_xdnd_handle(owner, &xev->xclient, ev);
			}
		} break;

		case LeaveNotify: {
			ev->type = MKWIN_EV_LEAVE;
		} break;

		case FocusOut: {
			ev->type = MKWIN_EV_FOCUS_OUT;
		} break;

		case SelectionRequest: {
			XSelectionRequestEvent *req = &xev->xselectionrequest;
			XSelectionEvent resp;
			memset(&resp, 0, sizeof(resp));
			resp.type = SelectionNotify;
			resp.requestor = req->requestor;
			resp.selection = req->selection;
			resp.target = req->target;
			resp.time = req->time;
			resp.property = None;

			if(req->target == owner->atoms.targets) {
				Atom supported[] = { owner->atoms.utf8_string, XA_STRING };
				XChangeProperty(owner->dpy, req->requestor, req->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)supported, 2);
				resp.property = req->property;
			} else if(req->target == owner->atoms.utf8_string || req->target == XA_STRING) {
				if(owner->clip_len > mkwin_clip_chunk_bytes(owner->dpy)) {
					if(mkwin_clip_incr_begin(owner, req->requestor, req->property, req->target)) {
						resp.property = req->property;
					}
				} else {
					XChangeProperty(owner->dpy, req->requestor, req->property, req->target, 8, PropModeReplace, (unsigned char *)owner->clip_text, (int)owner->clip_len);
					resp.property = req->property;
				}
			}

			XSendEvent(owner->dpy, req->requestor, False, 0, (XEvent *)&resp);
			XFlush(owner->dpy);
			ev->type = MKWIN_EV_NONE;
		} break;

		case SelectionNotify: {
			ev->type = MKWIN_EV_NONE;
			mkwin_xdnd_selection(owner, &xev->xselection, ev);
		} break;

		default: {
			ev->type = MKWIN_EV_NONE;
		} break;
	}
}

// [=]===^=[ mkwin_find_window_owner ]=============================[=]
static struct mkwin_window *mkwin_find_window_owner(Window xwin, int32_t *popup_idx) {
	*popup_idx = -1;
	for(uint32_t i = 0; i < g_window_count; ++i) {
		struct mkwin_window *c = g_windows[i];
		if(c->win == xwin) {
			return c;
		}
		for(uint32_t p = 0; p < c->popup_count; ++p) {
			if(c->popups[p]->xwin == xwin) {
				*popup_idx = (int32_t)p;
				return c;
			}
		}
	}
	return NULL;
}

// [=]===^=[ mkwin_poll ]==========================================[=]
// Dequeue one platform event into `ev`. Returns 1 when an event was produced, 0
// when nothing is pending. Internal traffic (selection plumbing, foreign-window
// events routed to their owner's deferred queue) yields an MKWIN_EV_NONE event
// the host ignores.
MKWIN_API uint32_t mkwin_poll(struct mkwin_window *win, struct mkwin_event *ev) {
	memset(ev, 0, sizeof(*ev));
	ev->popup_idx = -1;

	if(mkwin_deferred_pop(win, ev)) {
		return 1;
	}

	if(XEventsQueued(win->dpy, QueuedAlready) == 0) {
		return 0;
	}

	XEvent xev;
	XNextEvent(win->dpy, &xev);

	// An in-flight INCR send is paced by the requestor deleting our property;
	// those PropertyNotify events carry the requestor's (foreign) window, so
	// handle them before the per-window routing below would drop them.
	if(xev.type == PropertyNotify && xev.xproperty.state == PropertyDelete) {
		if(mkwin_clip_incr_continue(&xev.xproperty)) {
			ev->type = MKWIN_EV_NONE;
			return 1;
		}
	}

	if(xev.xany.window == win->win) {
		mkwin_translate_xevent(win, &xev, ev);
		return 1;
	}

	for(uint32_t i = 0; i < win->popup_count; ++i) {
		if(win->popups[i]->xwin == xev.xany.window) {
			ev->popup_idx = (int32_t)i;
			mkwin_translate_xevent(win, &xev, ev);
			return 1;
		}
	}

	int32_t foreign_popup = -1;
	struct mkwin_window *owner = mkwin_find_window_owner(xev.xany.window, &foreign_popup);
	if(owner && owner != win) {
		struct mkwin_event foreign;
		memset(&foreign, 0, sizeof(foreign));
		foreign.popup_idx = foreign_popup;
		mkwin_translate_xevent(owner, &xev, &foreign);
		if(foreign.type != MKWIN_EV_NONE) {
			mkwin_deferred_push(owner, &foreign);
		}
		ev->type = MKWIN_EV_NONE;
		return 1;
	}

	ev->type = MKWIN_EV_NONE;
	return 1;
}

// ---------------------------------------------------------------------------
// Font rasterization (FreeType)
// ---------------------------------------------------------------------------

#ifdef MKWIN_FONT

static char mkwin_fc_font_path[4096];

// [=]===^=[ mkwin_find_font ]=====================================[=]
static const char *mkwin_find_font(void) {
	const char *env = getenv("MKWIN_FONT_FILE");
	if(env) {
		return env;
	}

	FcConfig *config = FcInitLoadConfigAndFonts();
	if(config) {
		FcPattern *pat = FcNameParse((FcChar8 *)"sans-serif");
		if(pat) {
			FcConfigSubstitute(config, pat, FcMatchPattern);
			FcDefaultSubstitute(pat);
			FcResult result;
			FcPattern *match = FcFontMatch(config, pat, &result);
			if(match) {
				FcChar8 *file = NULL;
				if(FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
					snprintf(mkwin_fc_font_path, sizeof(mkwin_fc_font_path), "%s", (char *)file);
				}
				FcPatternDestroy(match);
			}
			FcPatternDestroy(pat);
		}
		FcConfigDestroy(config);
		FcFini();
		if(mkwin_fc_font_path[0]) {
			return mkwin_fc_font_path;
		}
	}

	return NULL;
}

// [=]===^=[ mkwin_font_metrics ]==================================[=]
static void mkwin_font_metrics(FT_Face face, int32_t *out_ascent, int32_t *out_height) {
	if(out_ascent) {
		*out_ascent = (int32_t)(face->size->metrics.ascender >> 6);
	}
	if(out_height) {
		*out_height = (int32_t)((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
	}
}

// [=]===^=[ mkwin_font_open ]=====================================[=]
MKWIN_API struct mkwin_font *mkwin_font_open(const char *path, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height) {
	struct mkwin_font *font = (struct mkwin_font *)calloc(1, sizeof(*font));
	if(!font) {
		return NULL;
	}
	if(FT_Init_FreeType(&font->lib)) {
		free(font);
		return NULL;
	}
	const char *p = path ? path : mkwin_find_font();
	if(!p || FT_New_Face(font->lib, p, 0, &font->face)) {
		FT_Done_FreeType(font->lib);
		free(font);
		return NULL;
	}
	FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)pixel_size);
	mkwin_font_metrics(font->face, out_ascent, out_height);
	return font;
}

// [=]===^=[ mkwin_font_set_size ]=================================[=]
MKWIN_API void mkwin_font_set_size(struct mkwin_font *font, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height) {
	FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)pixel_size);
	mkwin_font_metrics(font->face, out_ascent, out_height);
}

// [=]===^=[ mkwin_font_glyph ]====================================[=]
MKWIN_API uint32_t mkwin_font_glyph(struct mkwin_font *font, uint32_t codepoint, struct mkwin_glyph_bitmap *out) {
	if(FT_Load_Char(font->face, codepoint, FT_LOAD_RENDER)) {
		return 0;
	}
	FT_GlyphSlot slot = font->face->glyph;
	out->width = (int32_t)slot->bitmap.width;
	out->height = (int32_t)slot->bitmap.rows;
	out->bearing_x = slot->bitmap_left;
	out->bearing_y = slot->bitmap_top;
	out->advance = (int32_t)(slot->advance.x >> 6);
	out->coverage = slot->bitmap.buffer;
	return 1;
}

// [=]===^=[ mkwin_font_close ]====================================[=]
MKWIN_API void mkwin_font_close(struct mkwin_font *font) {
	if(!font) {
		return;
	}
	if(font->face) {
		FT_Done_Face(font->face);
	}
	if(font->lib) {
		FT_Done_FreeType(font->lib);
	}
	free(font);
}

#endif // MKWIN_FONT

// ---------------------------------------------------------------------------
// GL view child window
// ---------------------------------------------------------------------------

#ifdef MKWIN_GLVIEW

// [=]===^=[ mkwin_glview_create ]=================================[=]
MKWIN_API struct mkwin_glview *mkwin_glview_create(struct mkwin_window *win, int32_t x, int32_t y, int32_t w, int32_t h) {
	struct mkwin_glview *view = (struct mkwin_glview *)calloc(1, sizeof(*view));
	if(!view) {
		return NULL;
	}
	view->win = win;

	// The main window is ARGB32 (for themed transparency on popups etc.), but
	// inheriting that visual for the glview child is wrong: the GL context
	// typically has no alpha channel, so after SwapBuffers the drawable's alpha
	// stays zero and a compositor renders the child as see-through. Force a
	// 24-bit RGB visual here so the compositor treats the child as fully opaque
	// regardless of what the app's GL back buffer contains.
	XVisualInfo vinfo;
	Visual *gl_visual;
	int32_t gl_depth;
	Colormap gl_colormap;
	if(XMatchVisualInfo(win->dpy, win->screen, 24, TrueColor, &vinfo)) {
		gl_visual = vinfo.visual;
		gl_depth = 24;
		gl_colormap = XCreateColormap(win->dpy, win->root, gl_visual, AllocNone);
	} else {
		gl_visual = CopyFromParent;
		gl_depth = CopyFromParent;
		gl_colormap = win->colormap;
	}

	XSetWindowAttributes wa;
	wa.background_pixmap = None;
	wa.event_mask = 0;
	wa.colormap = gl_colormap;
	wa.border_pixel = 0;
	unsigned long mask = CWBackPixmap | CWEventMask;
	if(gl_visual != CopyFromParent) {
		mask |= CWColormap | CWBorderPixel;
	}
	view->xwin = XCreateWindow(win->dpy, win->win, x, y, (uint32_t)w, (uint32_t)h, 0, gl_depth, InputOutput, gl_visual, mask, &wa);

	XMapWindow(win->dpy, view->xwin);
	XFlush(win->dpy);
	return view;
}

// [=]===^=[ mkwin_glview_handle ]=================================[=]
MKWIN_API void *mkwin_glview_handle(struct mkwin_glview *view) {
	return (void *)(uintptr_t)view->xwin;
}

// [=]===^=[ mkwin_glview_reposition ]=============================[=]
MKWIN_API void mkwin_glview_reposition(struct mkwin_glview *view, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(!view->xwin) {
		return;
	}
	XMoveResizeWindow(view->win->dpy, view->xwin, x, y, (uint32_t)w, (uint32_t)h);
}

// [=]===^=[ mkwin_glview_show ]===================================[=]
MKWIN_API void mkwin_glview_show(struct mkwin_glview *view, uint32_t visible) {
	if(!view->xwin) {
		return;
	}
	if(visible) {
		XMapWindow(view->win->dpy, view->xwin);
	} else {
		XUnmapWindow(view->win->dpy, view->xwin);
	}
}

// [=]===^=[ mkwin_glview_destroy ]================================[=]
MKWIN_API void mkwin_glview_destroy(struct mkwin_glview *view) {
	if(view->xwin) {
		XDestroyWindow(view->win->dpy, view->xwin);
		view->xwin = 0;
	}
	free(view);
}

#endif // MKWIN_GLVIEW
