// Copyright (c) 2026 Peter Fors
// SPDX-License-Identifier: MIT

#include "mkwin.h"
#include "mkwin_types_win32.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Library state
// ---------------------------------------------------------------------------

static struct mkwin_window *g_windows[MKWIN_MAX_WINDOWS];
static uint32_t g_window_count;
static uint32_t g_class_registered;
static uint32_t g_glview_class_registered;

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_sleep_ns ]======================================[=]
// ms-granular at the kernel level (Sleep ceils to ms; sub-ms would need a
// waitable timer).
MKWIN_API void mkwin_sleep_ns(uint64_t ns) {
	DWORD ms = (DWORD)((ns + 999999ull) / 1000000ull);
	Sleep(ms);
}

// [=]===^=[ mkwin_now_ns ]========================================[=]
// Monotonic clock in nanoseconds.
MKWIN_API uint64_t mkwin_now_ns(void) {
	static int64_t freq;
	if(!freq) {
		QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
	}
	int64_t now;
	QueryPerformanceCounter((LARGE_INTEGER *)&now);
	uint64_t q = (uint64_t)now / (uint64_t)freq;
	uint64_t r = (uint64_t)now % (uint64_t)freq;
	return q * 1000000000ull + (r * 1000000000ull) / (uint64_t)freq;
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

// [=]===^=[ mkwin_drop_enable ]===================================[=]
MKWIN_API void mkwin_drop_enable(struct mkwin_window *win) {
	win->drop_enabled = 1;
	DragAcceptFiles(win->hwnd, TRUE);
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
// Per-window event queue
// ---------------------------------------------------------------------------

// [=]===^=[ evq_push ]============================================[=]
static void evq_push(struct mkwin_window *win, struct mkwin_event *ev) {
	uint32_t next = (win->evq_head + 1) % MKWIN_EVQ_SIZE;
	if(next == win->evq_tail) {
		return;
	}
	win->evq_buf[win->evq_head] = *ev;
	win->evq_head = next;
}

// [=]===^=[ evq_pop ]=============================================[=]
static uint32_t evq_pop(struct mkwin_window *win, struct mkwin_event *ev) {
	if(win->evq_head == win->evq_tail) {
		return 0;
	}
	*ev = win->evq_buf[win->evq_tail];
	win->evq_tail = (win->evq_tail + 1) % MKWIN_EVQ_SIZE;
	return 1;
}

// ---------------------------------------------------------------------------
// Framebuffer helpers
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_fb_create_dib ]=================================[=]
static uint32_t mkwin_fb_create_dib(HDC hdc_ref, HDC *hdc_mem, HBITMAP *hbmp, HBITMAP *hbmp_old, uint32_t **pixels, int32_t w, int32_t h) {
	BITMAPINFO bmi;
	memset(&bmi, 0, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = -h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	*hdc_mem = CreateCompatibleDC(hdc_ref);
	*hbmp = CreateDIBSection(*hdc_mem, &bmi, DIB_RGB_COLORS, (void **)pixels, NULL, 0);
	memset(*pixels, 0, (size_t)w * (size_t)h * 4);
	*hbmp_old = (HBITMAP)SelectObject(*hdc_mem, *hbmp);
	return 1;
}

// [=]===^=[ mkwin_fb_destroy_dib ]================================[=]
static void mkwin_fb_destroy_dib(HDC *hdc_mem, HBITMAP *hbmp, HBITMAP *hbmp_old) {
	if(*hdc_mem) {
		SelectObject(*hdc_mem, *hbmp_old);
		DeleteObject(*hbmp);
		DeleteDC(*hdc_mem);
		*hdc_mem = NULL;
		*hbmp = NULL;
	}
}

// [=]===^=[ mkwin_fb_resize ]=====================================[=]
static void mkwin_fb_resize(struct mkwin_window *win) {
	if(win->win_w <= 0 || win->win_h <= 0) {
		return;
	}
	mkwin_fb_destroy_dib(&win->hdc_mem, &win->hbmp, &win->hbmp_old);
	HDC hdc = GetDC(win->hwnd);
	mkwin_fb_create_dib(hdc, &win->hdc_mem, &win->hbmp, &win->hbmp_old, &win->pixels, win->win_w, win->win_h);
	ReleaseDC(win->hwnd, hdc);
}

// ---------------------------------------------------------------------------
// Key translation
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_translate_vk ]==================================[=]
static uint32_t mkwin_translate_vk(WPARAM vk) {
	switch(vk) {
		case VK_BACK:   { return 0xff08; } break;
		case VK_TAB:    { return 0xff09; } break;
		case VK_RETURN: { return 0xff0d; } break;
		case VK_ESCAPE: { return 0xff1b; } break;
		case VK_DELETE: { return 0xffff; } break;
		case VK_HOME:   { return 0xff50; } break;
		case VK_LEFT:   { return 0xff51; } break;
		case VK_UP:     { return 0xff52; } break;
		case VK_RIGHT:  { return 0xff53; } break;
		case VK_DOWN:   { return 0xff54; } break;
		case VK_PRIOR:  { return 0xff55; } break;
		case VK_NEXT:   { return 0xff56; } break;
		case VK_END:    { return 0xff57; } break;
		default: break;
	}
	return (uint32_t)vk;
}

// [=]===^=[ mkwin_get_keymod ]====================================[=]
static uint32_t mkwin_get_keymod(void) {
	uint32_t mod = 0;
	if(GetKeyState(VK_SHIFT) & 0x8000) {
		mod |= MKWIN_MOD_SHIFT;
	}
	if(GetKeyState(VK_CONTROL) & 0x8000) {
		mod |= MKWIN_MOD_CONTROL;
	}
	if(GetKeyState(VK_MENU) & 0x8000) {
		mod |= MKWIN_MOD_ALT;
	}
	return mod;
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_find_owner ]====================================[=]
static struct mkwin_window *mkwin_find_owner(HWND hwnd, int32_t *popup_idx) {
	*popup_idx = -1;
	for(uint32_t i = 0; i < g_window_count; ++i) {
		struct mkwin_window *c = g_windows[i];
		if(c->hwnd == hwnd) {
			return c;
		}
		for(uint32_t p = 0; p < c->popup_count; ++p) {
			if(c->popups[p]->hwnd == hwnd) {
				*popup_idx = (int32_t)p;
				return c;
			}
		}
	}
	return NULL;
}

// [=]===^=[ mkwin_wndproc ]=======================================[=]
static LRESULT CALLBACK mkwin_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	int32_t popup_idx = -1;
	struct mkwin_window *owner = mkwin_find_owner(hwnd, &popup_idx);

	if(!owner) {
		return DefWindowProcA(hwnd, msg, wp, lp);
	}

	struct mkwin_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.popup_idx = popup_idx;

	switch(msg) {
		case WM_GETMINMAXINFO: {
			if(ev.popup_idx < 0 && (owner->min_w > 0 || owner->min_h > 0)) {
				MINMAXINFO *mmi = (MINMAXINFO *)lp;
				if(owner->min_w > 0) {
					mmi->ptMinTrackSize.x = owner->min_w;
				}
				if(owner->min_h > 0) {
					mmi->ptMinTrackSize.y = owner->min_h;
				}
			}
			return 0;
		} break;

		case WM_PAINT: {
			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			EndPaint(hwnd, &ps);
			ev.type = MKWIN_EV_EXPOSE;
			evq_push(owner, &ev);
			return 0;
		} break;

		case WM_SIZE: {
			if(ev.popup_idx < 0) {
				owner->win_w = LOWORD(lp);
				owner->win_h = HIWORD(lp);
				mkwin_fb_resize(owner);
				mkwin_publish(owner);
				ev.type = MKWIN_EV_RESIZE;
				ev.width = owner->win_w;
				ev.height = owner->win_h;
				uint32_t prev = (owner->evq_head + MKWIN_EVQ_SIZE - 1) % MKWIN_EVQ_SIZE;
				if(owner->evq_head != owner->evq_tail && owner->evq_buf[prev].type == MKWIN_EV_RESIZE) {
					owner->evq_buf[prev] = ev;
				} else {
					evq_push(owner, &ev);
				}
			}
			return 0;
		} break;

		case WM_MOUSEMOVE: {
			ev.type = MKWIN_EV_MOTION;
			ev.x = (int16_t)LOWORD(lp);
			ev.y = (int16_t)HIWORD(lp);
			uint32_t prev = (owner->evq_head + MKWIN_EVQ_SIZE - 1) % MKWIN_EVQ_SIZE;
			if(owner->evq_head != owner->evq_tail && owner->evq_buf[prev].type == MKWIN_EV_MOTION) {
				owner->evq_buf[prev] = ev;
			} else {
				evq_push(owner, &ev);
			}
			return 0;
		} break;

		case WM_LBUTTONDOWN: {
			ev.type = MKWIN_EV_BUTTON_PRESS;
			ev.x = (int16_t)LOWORD(lp);
			ev.y = (int16_t)HIWORD(lp);
			ev.button = 1;
			ev.keymod = mkwin_get_keymod();
			evq_push(owner, &ev);
			SetCapture(hwnd);
			return 0;
		} break;

		case WM_LBUTTONUP: {
			ev.type = MKWIN_EV_BUTTON_RELEASE;
			ev.x = (int16_t)LOWORD(lp);
			ev.y = (int16_t)HIWORD(lp);
			ev.button = 1;
			evq_push(owner, &ev);
			ReleaseCapture();
			return 0;
		} break;

		case WM_RBUTTONDOWN: {
			ev.type = MKWIN_EV_BUTTON_PRESS;
			ev.x = (int16_t)LOWORD(lp);
			ev.y = (int16_t)HIWORD(lp);
			ev.button = 3;
			evq_push(owner, &ev);
			return 0;
		} break;

		case WM_RBUTTONUP: {
			ev.type = MKWIN_EV_BUTTON_RELEASE;
			ev.x = (int16_t)LOWORD(lp);
			ev.y = (int16_t)HIWORD(lp);
			ev.button = 3;
			evq_push(owner, &ev);
			return 0;
		} break;

		case WM_MOUSEWHEEL: {
			int16_t delta = (int16_t)HIWORD(wp);
			ev.type = MKWIN_EV_BUTTON_PRESS;
			POINT pt = { (int16_t)LOWORD(lp), (int16_t)HIWORD(lp) };
			ScreenToClient(hwnd, &pt);
			ev.x = pt.x;
			ev.y = pt.y;
			ev.button = delta > 0 ? 4 : 5;
			evq_push(owner, &ev);
			ev.type = MKWIN_EV_BUTTON_RELEASE;
			evq_push(owner, &ev);
			return 0;
		} break;

		case WM_MOUSEHWHEEL: {
			int16_t delta = (int16_t)HIWORD(wp);
			ev.type = MKWIN_EV_BUTTON_PRESS;
			POINT pt = { (int16_t)LOWORD(lp), (int16_t)HIWORD(lp) };
			ScreenToClient(hwnd, &pt);
			ev.x = pt.x;
			ev.y = pt.y;
			ev.button = delta > 0 ? 7 : 6;
			evq_push(owner, &ev);
			ev.type = MKWIN_EV_BUTTON_RELEASE;
			evq_push(owner, &ev);
			return 0;
		} break;

		case WM_KEYDOWN: {
			ev.type = MKWIN_EV_KEY;
			ev.keysym = mkwin_translate_vk(wp);
			ev.keymod = mkwin_get_keymod();
			ev.text[0] = '\0';
			ev.text_len = 0;
			evq_push(owner, &ev);
			return 0;
		} break;

		case WM_CHAR: {
			uint32_t cp = (uint32_t)wp;
			if(cp >= 32) {
				ev.type = MKWIN_EV_KEY;
				ev.keysym = cp < 128 ? cp : 0;
				ev.keymod = mkwin_get_keymod();
				if(cp < 0x80) {
					ev.text[0] = (char)cp;
					ev.text[1] = '\0';
					ev.text_len = 1;
				} else if(cp < 0x800) {
					ev.text[0] = (char)(0xc0 | (cp >> 6));
					ev.text[1] = (char)(0x80 | (cp & 0x3f));
					ev.text[2] = '\0';
					ev.text_len = 2;
				} else {
					ev.text[0] = (char)(0xe0 | (cp >> 12));
					ev.text[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
					ev.text[2] = (char)(0x80 | (cp & 0x3f));
					ev.text[3] = '\0';
					ev.text_len = 3;
				}
				evq_push(owner, &ev);
			}
			return 0;
		} break;

		case WM_DROPFILES: {
			HDROP hdrop = (HDROP)wp;
			UINT count = DragQueryFileA(hdrop, 0xFFFFFFFF, NULL, 0);
			drop_free(owner);
			for(UINT i = 0; i < count && i < MKWIN_DROP_MAX; ++i) {
				char path[4096];
				UINT len = DragQueryFileA(hdrop, i, path, sizeof(path));
				if(len > 0) {
					drop_add_path(owner, path);
				}
			}
			DragFinish(hdrop);
			if(owner->drop_count > 0) {
				ev.type = MKWIN_EV_DROP;
				evq_push(owner, &ev);
			}
			return 0;
		} break;

		case WM_ENTERSIZEMOVE: {
			SetTimer(hwnd, 1, 16, NULL);
			return 0;
		} break;

		case WM_EXITSIZEMOVE: {
			KillTimer(hwnd, 1);
			return 0;
		} break;

		case WM_TIMER: {
			// The modal move/resize loop runs its own message pump, so the host's
			// event loop is suspended; hand it a per-frame tick to keep animating.
			if(wp == 1 && ev.popup_idx < 0 && owner->modal_frame_cb) {
				owner->modal_frame_cb(owner, owner->modal_frame_user);
			}
			return 0;
		} break;

		case WM_CLOSE: {
			ev.type = MKWIN_EV_CLOSE;
			evq_push(owner, &ev);
			return 0;
		} break;

		case WM_MOUSELEAVE: {
			ev.type = MKWIN_EV_LEAVE;
			evq_push(owner, &ev);
			return 0;
		} break;

		default: break;
	}

	return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Window class registration
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_set_dpi_aware ]=================================[=]
static void mkwin_set_dpi_aware(void) {
	typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(HANDLE);
	typedef HRESULT (WINAPI *SetProcessDpiAwareness_t)(int);

	HMODULE user32 = GetModuleHandleA("user32.dll");
	if(user32) {
		SetProcessDpiAwarenessContext_t fn = (SetProcessDpiAwarenessContext_t)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
		if(fn) {
			fn((HANDLE)-4);
			return;
		}
	}

	HMODULE shcore = LoadLibraryA("shcore.dll");
	if(shcore) {
		SetProcessDpiAwareness_t fn = (SetProcessDpiAwareness_t)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
		if(fn) {
			fn(2);
		}
		FreeLibrary(shcore);
	}
}

// [=]===^=[ mkwin_register_class ]================================[=]
static void mkwin_register_class(void) {
	if(g_class_registered) {
		return;
	}
	mkwin_set_dpi_aware();
	WNDCLASSEXA wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = mkwin_wndproc;
	wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
	wc.lpszClassName = "mkwin";
	RegisterClassExA(&wc);
	g_class_registered = 1;
}

// ---------------------------------------------------------------------------
// WM_CLASS and window icon
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_set_class_hint ]================================[=]
// Win32 has no WM_CLASS equivalent; class identity is the registered class name.
MKWIN_API void mkwin_set_class_hint(struct mkwin_window *win, const char *instance, const char *cls) {
	(void)win;
	(void)instance;
	(void)cls;
}

// [=]===^=[ mkwin_make_hicon ]====================================[=]
static HICON mkwin_make_hicon(uint32_t *pixels, int32_t w, int32_t h) {
	BITMAPV5HEADER bi;
	memset(&bi, 0, sizeof(bi));
	bi.bV5Size = sizeof(bi);
	bi.bV5Width = w;
	bi.bV5Height = -h;
	bi.bV5Planes = 1;
	bi.bV5BitCount = 32;
	bi.bV5Compression = BI_BITFIELDS;
	bi.bV5RedMask   = 0x00ff0000;
	bi.bV5GreenMask = 0x0000ff00;
	bi.bV5BlueMask  = 0x000000ff;
	bi.bV5AlphaMask = 0xff000000;

	uint32_t *dib_bits = NULL;
	HDC hdc = GetDC(NULL);
	HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&dib_bits, NULL, 0);
	ReleaseDC(NULL, hdc);
	if(!color) {
		return NULL;
	}
	memcpy(dib_bits, pixels, (size_t)(w * h) * 4);

	HBITMAP mask = CreateBitmap(w, h, 1, 1, NULL);
	if(!mask) {
		DeleteObject(color);
		return NULL;
	}

	ICONINFO ii;
	ii.fIcon = TRUE;
	ii.xHotspot = 0;
	ii.yHotspot = 0;
	ii.hbmMask = mask;
	ii.hbmColor = color;
	HICON icon = CreateIconIndirect(&ii);
	DeleteObject(color);
	DeleteObject(mask);
	return icon;
}

// [=]===^=[ mkwin_set_icon ]======================================[=]
MKWIN_API void mkwin_set_icon(struct mkwin_window *win, const struct mkwin_icon_size *sizes, uint32_t count) {
	int32_t sm_big = GetSystemMetrics(SM_CXICON);
	int32_t sm_small = GetSystemMetrics(SM_CXSMICON);
	uint32_t best_big = 0;
	uint32_t best_small = 0;
	int32_t dist_big = 0x7fffffff;
	int32_t dist_small = 0x7fffffff;
	for(uint32_t i = 0; i < count; ++i) {
		int32_t d = sizes[i].w - sm_big;
		if(d < 0) {
			d = -d;
		}
		if(d < dist_big) {
			dist_big = d;
			best_big = i;
		}
		d = sizes[i].w - sm_small;
		if(d < 0) {
			d = -d;
		}
		if(d < dist_small) {
			dist_small = d;
			best_small = i;
		}
	}
	HICON big = mkwin_make_hicon(sizes[best_big].pixels, sizes[best_big].w, sizes[best_big].h);
	HICON small_icon = mkwin_make_hicon(sizes[best_small].pixels, sizes[best_small].w, sizes[best_small].h);
	if(big) {
		SendMessageA(win->hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
	}
	if(small_icon) {
		SendMessageA(win->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
	}
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

	typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
	HMODULE user32 = GetModuleHandleA("user32.dll");
	if(user32) {
		GetDpiForWindow_t fn = (GetDpiForWindow_t)(void *)GetProcAddress(user32, "GetDpiForWindow");
		if(fn) {
			UINT dpi = fn(win->hwnd);
			if(dpi > 0) {
				return (float)dpi / 96.0f;
			}
		}
	}

	HDC hdc = GetDC(NULL);
	if(hdc) {
		int32_t dpi = GetDeviceCaps(hdc, LOGPIXELSX);
		ReleaseDC(NULL, hdc);
		if(dpi > 0) {
			return (float)dpi / 96.0f;
		}
	}

	return 1.0f;
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_open ]==========================================[=]
// parent != NULL makes a transient/dialog owned by that window.
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
	win->undecorated = (flags & MKWIN_UNDECORATED) ? 1 : 0;
	if(app_class && app_class[0]) {
		strncpy(win->app_class, app_class, sizeof(win->app_class) - 1);
	}

	mkwin_register_class();

	DWORD style = (flags & MKWIN_UNDECORATED) ? WS_POPUP : WS_OVERLAPPEDWINDOW;
	DWORD exstyle = (flags & MKWIN_UNDECORATED) ? WS_EX_APPWINDOW : 0;
	HWND owner_hwnd = (parent && !(flags & MKWIN_UNDECORATED)) ? parent->hwnd : NULL;
	RECT rc = { 0, 0, w, h };
	AdjustWindowRect(&rc, style, FALSE);

	win->hwnd = CreateWindowExA(exstyle, "mkwin", title, style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, owner_hwnd, NULL, GetModuleHandleA(NULL), NULL);
	if(!win->hwnd) {
		free(win);
		return NULL;
	}
	win->parent_hwnd = parent ? parent->hwnd : NULL;

	win->win_w = w;
	win->win_h = h;

	HDC hdc = GetDC(win->hwnd);
	mkwin_fb_create_dib(hdc, &win->hdc_mem, &win->hbmp, &win->hbmp_old, &win->pixels, w, h);
	ReleaseDC(win->hwnd, hdc);

	win->cursor_default = LoadCursorA(NULL, IDC_ARROW);
	win->cursor_h_resize = LoadCursorA(NULL, IDC_SIZEWE);
	win->cursor_v_resize = LoadCursorA(NULL, IDC_SIZENS);
	win->cursor_active = MKWIN_CURSOR_DEFAULT;

	win->scale = mkwin_detect_scale(win);

	mkwin_register(win);
	mkwin_publish(win);
	return win;
}

// [=]===^=[ mkwin_map ]===========================================[=]
MKWIN_API void mkwin_map(struct mkwin_window *win) {
	ShowWindow(win->hwnd, SW_SHOW);
	UpdateWindow(win->hwnd);
}

// [=]===^=[ mkwin_unmap ]=========================================[=]
MKWIN_API void mkwin_unmap(struct mkwin_window *win) {
	ShowWindow(win->hwnd, SW_HIDE);
}

// [=]===^=[ mkwin_destroy ]=======================================[=]
MKWIN_API void mkwin_destroy(struct mkwin_window *win) {
	drop_free(win);
	mkwin_fb_destroy_dib(&win->hdc_mem, &win->hbmp, &win->hbmp_old);
	if(win->hwnd) {
		DestroyWindow(win->hwnd);
		win->hwnd = NULL;
	}
	mkwin_unregister(win);
	free(win);
}

// [=]===^=[ mkwin_set_cursor ]====================================[=]
MKWIN_API void mkwin_set_cursor(struct mkwin_window *win, uint32_t cursor) {
	if(win->cursor_active == cursor) {
		return;
	}
	win->cursor_active = cursor;
	HCURSOR c;
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
	SetCursor(c);
}

// ---------------------------------------------------------------------------
// Geometry and window management
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_resize ]========================================[=]
MKWIN_API void mkwin_resize(struct mkwin_window *win, int32_t w, int32_t h) {
	RECT rc = { 0, 0, w, h };
	DWORD style = win->undecorated ? WS_POPUP : WS_OVERLAPPEDWINDOW;
	AdjustWindowRect(&rc, style, FALSE);
	SetWindowPos(win->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// [=]===^=[ mkwin_move ]==========================================[=]
MKWIN_API void mkwin_move(struct mkwin_window *win, int32_t x, int32_t y) {
	SetWindowPos(win->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// [=]===^=[ mkwin_position ]======================================[=]
MKWIN_API void mkwin_position(struct mkwin_window *win, int32_t *out_x, int32_t *out_y) {
	RECT rc;
	GetWindowRect(win->hwnd, &rc);
	*out_x = rc.left;
	*out_y = rc.top;
}

// [=]===^=[ mkwin_begin_drag ]====================================[=]
MKWIN_API void mkwin_begin_drag(struct mkwin_window *win) {
	ReleaseCapture();
	SendMessageA(win->hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

// [=]===^=[ mkwin_set_shape ]=====================================[=]
MKWIN_API void mkwin_set_shape(struct mkwin_window *win, const int32_t *points_xy, uint32_t point_count) {
	POINT *pts = (POINT *)malloc(point_count * sizeof(POINT));
	if(!pts) {
		return;
	}
	for(uint32_t i = 0; i < point_count; ++i) {
		pts[i].x = points_xy[i * 2];
		pts[i].y = points_xy[i * 2 + 1];
	}
	HRGN rgn = CreatePolygonRgn(pts, (int)point_count, WINDING);
	SetWindowRgn(win->hwnd, rgn, TRUE);
	free(pts);
}

// [=]===^=[ mkwin_set_shape_mask ]================================[=]
MKWIN_API void mkwin_set_shape_mask(struct mkwin_window *win, const uint32_t *argb, int32_t w, int32_t h, uint32_t alpha_threshold) {
	HRGN combined = CreateRectRgn(0, 0, 0, 0);
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
				HRGN span = CreateRectRgn(x0, y, x, y + 1);
				CombineRgn(combined, combined, span, RGN_OR);
				DeleteObject(span);
			}
		}
	}
	SetWindowRgn(win->hwnd, combined, TRUE);
}

// [=]===^=[ mkwin_clear_shape ]===================================[=]
MKWIN_API void mkwin_clear_shape(struct mkwin_window *win) {
	SetWindowRgn(win->hwnd, NULL, TRUE);
}

// [=]===^=[ mkwin_set_title ]=====================================[=]
MKWIN_API void mkwin_set_title(struct mkwin_window *win, const char *title) {
	SetWindowTextA(win->hwnd, title);
}

// [=]===^=[ mkwin_is_child ]======================================[=]
MKWIN_API uint32_t mkwin_is_child(struct mkwin_window *win) {
	return win->is_child;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_blit_exclude_glviews ]==========================[=]
// GDI would paint over the GL child windows, so cut their rectangles out of the
// blit clip region first.
static void mkwin_blit_exclude_glviews(struct mkwin_window *win, HDC hdc) {
	for(uint32_t i = 0; i < win->glview_count; ++i) {
		struct mkwin_glview *gv = win->glviews[i];
		if(gv->hwnd && gv->visible) {
			ExcludeClipRect(hdc, gv->last_x, gv->last_y, gv->last_x + gv->last_w, gv->last_y + gv->last_h);
		}
	}
}

// [=]===^=[ mkwin_present ]=======================================[=]
MKWIN_API void mkwin_present(struct mkwin_window *win) {
	HDC hdc = GetDC(win->hwnd);
	SelectClipRgn(hdc, NULL);
	mkwin_blit_exclude_glviews(win, hdc);
	BitBlt(hdc, 0, 0, win->win_w, win->win_h, win->hdc_mem, 0, 0, SRCCOPY);
	ReleaseDC(win->hwnd, hdc);
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
	HDC hdc = GetDC(win->hwnd);
	SelectClipRgn(hdc, NULL);
	mkwin_blit_exclude_glviews(win, hdc);
	BitBlt(hdc, x, y, w, h, win->hdc_mem, x, y, SRCCOPY);
	ReleaseDC(win->hwnd, hdc);
}

// [=]===^=[ mkwin_flush ]=========================================[=]
MKWIN_API void mkwin_flush(struct mkwin_window *win) {
	(void)win;
	GdiFlush();
}

// [=]===^=[ mkwin_sync ]==========================================[=]
// Win32 presentation is synchronous; nothing to wait for.
MKWIN_API void mkwin_sync(struct mkwin_window *win) {
	(void)win;
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

	p->hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "mkwin", "", WS_POPUP | WS_VISIBLE, x, y, w, h, parent->hwnd, NULL, GetModuleHandleA(NULL), NULL);
	if(!p->hwnd) {
		free(p);
		return NULL;
	}

	HDC hdc = GetDC(p->hwnd);
	mkwin_fb_create_dib(hdc, &p->hdc_mem, &p->hbmp, &p->hbmp_old, &p->pixels, w, h);
	ReleaseDC(p->hwnd, hdc);

	parent->popups[parent->popup_count++] = p;
	return p;
}

// [=]===^=[ mkwin_popup_framebuffer ]=============================[=]
MKWIN_API uint32_t *mkwin_popup_framebuffer(struct mkwin_popup *popup) {
	return popup->pixels;
}

// [=]===^=[ mkwin_popup_present ]=================================[=]
MKWIN_API void mkwin_popup_present(struct mkwin_popup *popup) {
	HDC hdc = GetDC(popup->hwnd);
	BitBlt(hdc, 0, 0, popup->w, popup->h, popup->hdc_mem, 0, 0, SRCCOPY);
	ReleaseDC(popup->hwnd, hdc);
}

// [=]===^=[ mkwin_popup_close ]===================================[=]
MKWIN_API void mkwin_popup_close(struct mkwin_popup *popup) {
	struct mkwin_window *parent = popup->parent;
	mkwin_fb_destroy_dib(&popup->hdc_mem, &popup->hbmp, &popup->hbmp_old);
	if(popup->hwnd) {
		DestroyWindow(popup->hwnd);
		popup->hwnd = NULL;
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
// No shared connection handle on Win32; GL hosts bind via mkwin_glview_handle.
MKWIN_API void *mkwin_display(struct mkwin_window *win) {
	(void)win;
	return NULL;
}

// [=]===^=[ mkwin_screen_size ]===================================[=]
MKWIN_API void mkwin_screen_size(struct mkwin_window *win, int32_t *out_w, int32_t *out_h) {
	(void)win;
	*out_w = GetSystemMetrics(SM_CXSCREEN);
	*out_h = GetSystemMetrics(SM_CYSCREEN);
}

// [=]===^=[ mkwin_set_min_size ]==================================[=]
MKWIN_API void mkwin_set_min_size(struct mkwin_window *win, int32_t min_w, int32_t min_h) {
	win->min_w = min_w;
	win->min_h = min_h;
}

// [=]===^=[ mkwin_translate_coords ]==============================[=]
MKWIN_API void mkwin_translate_coords(struct mkwin_window *win, int32_t local_x, int32_t local_y, int32_t *out_screen_x, int32_t *out_screen_y) {
	POINT pt = { local_x, local_y };
	ClientToScreen(win->hwnd, &pt);
	*out_screen_x = pt.x;
	*out_screen_y = pt.y;
}

// ---------------------------------------------------------------------------
// Event pump
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_pump_messages ]=================================[=]
static void mkwin_pump_messages(void) {
	MSG msg;
	while(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
}

// [=]===^=[ mkwin_wait ]==========================================[=]
// timeout_ns < 0: block indefinitely. == 0: return at once. > 0: wait up to ns.
// MsgWaitForMultipleObjects is ms-granular, so ceil-round the conversion. With
// no pollable descriptors the host must bound timeout_ns by its own next
// deadline (there is no fd registry to wake this on a host timer).
MKWIN_API void mkwin_wait(struct mkwin_window *win, int64_t timeout_ns) {
	if(timeout_ns == 0 || win->evq_head != win->evq_tail) {
		return;
	}
	mkwin_pump_messages();
	if(win->evq_head != win->evq_tail) {
		return;
	}
	DWORD wait_ms;
	if(timeout_ns < 0) {
		wait_ms = INFINITE;
	} else {
		uint64_t ms = ((uint64_t)timeout_ns + 999999ull) / 1000000ull;
		if(ms > 0xfffffffeull) {
			ms = 0xfffffffeull;
		}
		wait_ms = (DWORD)ms;
	}
	MsgWaitForMultipleObjects(0, NULL, FALSE, wait_ms, QS_ALLINPUT);
}

// [=]===^=[ mkwin_wait_fd_add ]===================================[=]
// Inert on Win32: there are no pollable descriptors to fold into the wait.
MKWIN_API void mkwin_wait_fd_add(int32_t fd) {
	(void)fd;
}

// [=]===^=[ mkwin_wait_fd_remove ]================================[=]
MKWIN_API void mkwin_wait_fd_remove(int32_t fd) {
	(void)fd;
}

// [=]===^=[ mkwin_set_modal_frame_cb ]============================[=]
MKWIN_API void mkwin_set_modal_frame_cb(struct mkwin_window *win, void (*cb)(struct mkwin_window *win, void *user), void *user) {
	win->modal_frame_cb = cb;
	win->modal_frame_user = user;
}

// [=]===^=[ mkwin_pending ]=======================================[=]
MKWIN_API uint32_t mkwin_pending(struct mkwin_window *win) {
	mkwin_pump_messages();
	return win->evq_head != win->evq_tail;
}

// [=]===^=[ mkwin_poll ]==========================================[=]
MKWIN_API uint32_t mkwin_poll(struct mkwin_window *win, struct mkwin_event *ev) {
	mkwin_pump_messages();
	if(evq_pop(win, ev)) {
		return 1;
	}
	memset(ev, 0, sizeof(*ev));
	ev->popup_idx = -1;
	ev->type = MKWIN_EV_NONE;
	return 0;
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

// [=]===^=[ mkwin_clipboard_set ]=================================[=]
MKWIN_API void mkwin_clipboard_set(struct mkwin_window *win, const char *text, uint32_t len) {
	if(!OpenClipboard(win->hwnd)) {
		return;
	}
	EmptyClipboard();
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
	if(hg) {
		char *p = (char *)GlobalLock(hg);
		memcpy(p, text, len);
		p[len] = '\0';
		GlobalUnlock(hg);
		SetClipboardData(CF_TEXT, hg);
	}
	CloseClipboard();
}

// [=]===^=[ mkwin_clipboard_get ]=================================[=]
MKWIN_API char *mkwin_clipboard_get(struct mkwin_window *win, uint32_t *out_len) {
	*out_len = 0;
	if(!OpenClipboard(win->hwnd)) {
		return NULL;
	}
	HANDLE h = GetClipboardData(CF_TEXT);
	if(!h) {
		CloseClipboard();
		return NULL;
	}
	const char *p = (const char *)GlobalLock(h);
	if(!p) {
		CloseClipboard();
		return NULL;
	}
	uint32_t len = (uint32_t)strlen(p);
	char *buf = (char *)malloc(len + 1);
	if(!buf) {
		GlobalUnlock(h);
		CloseClipboard();
		return NULL;
	}
	memcpy(buf, p, len);
	buf[len] = '\0';
	GlobalUnlock(h);
	CloseClipboard();
	*out_len = len;
	return buf;
}

// ---------------------------------------------------------------------------
// Font rasterization (GDI)
// ---------------------------------------------------------------------------

#ifdef MKWIN_FONT

// [=]===^=[ mkwin_create_font ]===================================[=]
static HFONT mkwin_create_font(int32_t pixel_size) {
	HFONT h = CreateFontA(-pixel_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
	if(!h) {
		h = CreateFontA(-pixel_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Tahoma");
	}
	return h;
}

// [=]===^=[ mkwin_font_apply_metrics ]============================[=]
static void mkwin_font_apply_metrics(struct mkwin_font *font, int32_t *out_ascent, int32_t *out_height) {
	SelectObject(font->dc, font->handle);
	TEXTMETRICA tm;
	GetTextMetricsA(font->dc, &tm);
	if(out_ascent) {
		*out_ascent = tm.tmAscent;
	}
	if(out_height) {
		*out_height = tm.tmAscent + tm.tmDescent;
	}
}

// [=]===^=[ mkwin_font_open ]=====================================[=]
// path is ignored on Win32 (fonts are selected by family, not file).
MKWIN_API struct mkwin_font *mkwin_font_open(const char *path, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height) {
	(void)path;
	struct mkwin_font *font = (struct mkwin_font *)calloc(1, sizeof(*font));
	if(!font) {
		return NULL;
	}
	font->dc = CreateCompatibleDC(NULL);
	if(!font->dc) {
		free(font);
		return NULL;
	}
	font->handle = mkwin_create_font(pixel_size);
	if(!font->handle) {
		DeleteDC(font->dc);
		free(font);
		return NULL;
	}
	mkwin_font_apply_metrics(font, out_ascent, out_height);
	return font;
}

// [=]===^=[ mkwin_font_set_size ]=================================[=]
MKWIN_API void mkwin_font_set_size(struct mkwin_font *font, int32_t pixel_size, int32_t *out_ascent, int32_t *out_height) {
	if(font->handle) {
		DeleteObject(font->handle);
	}
	font->handle = mkwin_create_font(pixel_size);
	if(!font->handle) {
		return;
	}
	mkwin_font_apply_metrics(font, out_ascent, out_height);
}

// [=]===^=[ mkwin_font_glyph ]====================================[=]
MKWIN_API uint32_t mkwin_font_glyph(struct mkwin_font *font, uint32_t codepoint, struct mkwin_glyph_bitmap *out) {
	memset(out, 0, sizeof(*out));
	MAT2 mat = { {0, 1}, {0, 0}, {0, 0}, {0, 1} };
	GLYPHMETRICS gm;
	DWORD sz = GetGlyphOutlineA(font->dc, codepoint, GGO_GRAY8_BITMAP, &gm, 0, NULL, &mat);

	if(sz == GDI_ERROR || sz == 0) {
		// No bitmap (whitespace or unrenderable): report advance only.
		ABC abc;
		if(GetCharABCWidthsA(font->dc, codepoint, codepoint, &abc)) {
			out->advance = abc.abcA + (int32_t)abc.abcB + abc.abcC;
		} else if(sz == 0) {
			out->advance = gm.gmCellIncX;
		}
		return 1;
	}

	out->width = (int32_t)gm.gmBlackBoxX;
	out->height = (int32_t)gm.gmBlackBoxY;
	out->bearing_x = gm.gmptGlyphOrigin.x;
	out->bearing_y = gm.gmptGlyphOrigin.y;
	out->advance = gm.gmCellIncX;

	uint8_t *tmp = (uint8_t *)malloc(sz);
	if(!tmp) {
		return 1;
	}
	GetGlyphOutlineA(font->dc, codepoint, GGO_GRAY8_BITMAP, &gm, sz, tmp, &mat);

	uint32_t gw = gm.gmBlackBoxX;
	uint32_t gh = gm.gmBlackBoxY;
	uint32_t pitch = (gw + 3) & ~3u;
	uint32_t area = gw * gh;
	uint32_t need = area ? area : 1;
	if(need > font->scratch_cap) {
		uint8_t *ns = (uint8_t *)realloc(font->scratch, need);
		if(!ns) {
			free(tmp);
			return 1;
		}
		font->scratch = ns;
		font->scratch_cap = need;
	}
	// GGO_GRAY8_BITMAP coverage is 0..64; expand to 0..255.
	for(uint32_t y = 0; y < gh; ++y) {
		for(uint32_t x = 0; x < gw; ++x) {
			uint8_t v = tmp[y * pitch + x];
			font->scratch[y * gw + x] = (uint8_t)(v * 255 / 64);
		}
	}
	free(tmp);
	out->coverage = font->scratch;
	return 1;
}

// [=]===^=[ mkwin_font_close ]====================================[=]
MKWIN_API void mkwin_font_close(struct mkwin_font *font) {
	if(!font) {
		return;
	}
	if(font->handle) {
		DeleteObject(font->handle);
	}
	if(font->dc) {
		DeleteDC(font->dc);
	}
	free(font->scratch);
	free(font);
}

#endif // MKWIN_FONT

// ---------------------------------------------------------------------------
// GL view child window
// ---------------------------------------------------------------------------

#ifdef MKWIN_GLVIEW

// [=]===^=[ mkwin_glview_wndproc ]================================[=]
static LRESULT CALLBACK mkwin_glview_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if(msg == WM_ERASEBKGND) {
		return 1;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

// [=]===^=[ mkwin_glview_create ]=================================[=]
MKWIN_API struct mkwin_glview *mkwin_glview_create(struct mkwin_window *win, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(win->glview_count >= MKWIN_MAX_GLVIEWS) {
		return NULL;
	}
	if(!g_glview_class_registered) {
		WNDCLASSEXA wc;
		memset(&wc, 0, sizeof(wc));
		wc.cbSize = sizeof(wc);
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = mkwin_glview_wndproc;
		wc.lpszClassName = "mkwin_glview";
		RegisterClassExA(&wc);
		g_glview_class_registered = 1;
	}
	struct mkwin_glview *view = (struct mkwin_glview *)calloc(1, sizeof(*view));
	if(!view) {
		return NULL;
	}
	view->win = win;
	view->last_x = x;
	view->last_y = y;
	view->last_w = w;
	view->last_h = h;
	view->visible = 1;
	view->hwnd = CreateWindowExA(0, "mkwin_glview", "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, x, y, w, h, win->hwnd, NULL, GetModuleHandleA(NULL), NULL);
	if(!view->hwnd) {
		free(view);
		return NULL;
	}
	win->glviews[win->glview_count++] = view;
	return view;
}

// [=]===^=[ mkwin_glview_handle ]=================================[=]
MKWIN_API void *mkwin_glview_handle(struct mkwin_glview *view) {
	return (void *)view->hwnd;
}

// [=]===^=[ mkwin_glview_reposition ]=============================[=]
MKWIN_API void mkwin_glview_reposition(struct mkwin_glview *view, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(!view->hwnd) {
		return;
	}
	view->last_x = x;
	view->last_y = y;
	view->last_w = w;
	view->last_h = h;
	MoveWindow(view->hwnd, x, y, w, h, TRUE);
}

// [=]===^=[ mkwin_glview_show ]===================================[=]
MKWIN_API void mkwin_glview_show(struct mkwin_glview *view, uint32_t visible) {
	if(!view->hwnd) {
		return;
	}
	view->visible = visible;
	ShowWindow(view->hwnd, visible ? SW_SHOW : SW_HIDE);
}

// [=]===^=[ mkwin_glview_destroy ]================================[=]
MKWIN_API void mkwin_glview_destroy(struct mkwin_glview *view) {
	struct mkwin_window *win = view->win;
	if(view->hwnd) {
		DestroyWindow(view->hwnd);
		view->hwnd = NULL;
	}
	for(uint32_t i = 0; i < win->glview_count; ++i) {
		if(win->glviews[i] == view) {
			for(uint32_t j = i; j + 1 < win->glview_count; ++j) {
				win->glviews[j] = win->glviews[j + 1];
			}
			--win->glview_count;
			break;
		}
	}
	free(view);
}

#endif // MKWIN_GLVIEW
