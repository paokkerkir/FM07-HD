// fm07mod.dll - display mod for Football Manager 2007 (fm.exe 7.0.2).
//
// The game's UI (SI_WINDOW_PLATFORM) renders into a 32bpp DIB back buffer the size of the
// window's client area and BitBlts dirty rectangles to the window. We decouple the two:
//   - create_back_buffer is asked for client/zoom, so the UI lays out in logical pixels
//   - present StretchBlts the logical buffer to the physical client area
//   - the WndProc wrapper maps mouse coordinates back to logical pixels, frees window
//     resizing (WM_GETMINMAXINFO) and handles zoom hotkeys
//   - the platform open_window is given the configured size, mode and an unlocked max size
//
// Deployment: either copy this DLL into the game folder as dsound.dll (fm.exe loads it at
// startup; see proxy.c) or inject it as fm07mod.dll with fm07launch.exe. fm07mod.ini and
// fm07mod.log live next to the DLL; the current window/zoom state is written back to the ini
// as it changes so the next launch restores it.
//
// Addresses/offsets come from decomp/fm (see README "Display" section).

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <math.h>
#include "hook.h"
#include "hires.h"

// ---- fm.exe 7.0.2 addresses -------------------------------------------------------------

// short SI_WINDOW_PLATFORM::open_window(title,idx,w,h,flags,minH,maxH,minW,maxW); called by
// SI_WINDOW::open (0x00E014E0) after it applies the game's resolution preference to w/h
#define FM_TIMESTAMP   0x45DB2A8B // fm.exe 7.0.2 PE header, checked before touching any address
#define FM_IMAGE_SIZE  0x0151F000

#define ADDR_OPEN_WINDOW        0x00EAC820
#define ADDR_CREATE_BACK_BUFFER 0x00EAC440 // int SI_WINDOW_PLATFORM::create_back_buffer(w,h)
#define ADDR_MAIN_WNDPROC       0x00EAE350 // LRESULT CALLBACK main_wndproc(hwnd,msg,wp,lp)
#define ADDR_PRESENT            0x00EFC160 // void WINDOWS_RENDER_THREAD::present(HDC)

static const BYTE SIG_OPEN_WINDOW[] = { 0x6A, 0xFF, 0x68, 0x59, 0xBF, 0xFE, 0x00, 0x64, 0xA1 };
static const BYTE SIG_CREATE_BACK_BUFFER[] = { 0x83, 0xEC, 0x2C, 0x53, 0x55, 0x56, 0x8B, 0xF1 };
static const BYTE SIG_MAIN_WNDPROC[] = { 0xA1, 0x68, 0x63, 0x8E, 0x01, 0x85, 0xC0 };
static const BYTE SIG_PRESENT[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x9C, 0x00, 0x00, 0x00 };

// SI_WINDOW_PLATFORM field offsets
#define WIN_BB_WIDTH   0x80 // short, back buffer (logical) size
#define WIN_BB_HEIGHT  0x82
#define WIN_DIRTY_X0   0x8E // short, inclusive dirty rect in logical pixels
#define WIN_DIRTY_X1   0x90
#define WIN_DIRTY_Y0   0x92
#define WIN_DIRTY_Y1   0x94
#define WIN_HWND       0xAC
#define WIN_MEM_DC     0xB0
#define WIN_VF_FULL_REDRAW 0x4C // vtable offset: char is_full_redraw()

// WINDOWS_RENDER_THREAD field offsets
#define RT_WIDTH   0x98 // short
#define RT_HEIGHT  0x9A
#define RT_WINDOW  0x9C // SI_WINDOW_PLATFORM *

// ---- config ---------------------------------------------------------------------------

static struct {
	double zoom;       // requested UI scale; <= 0 means "auto" (monitor DPI / 96)
	double zoom_step;
	BOOL smooth;       // HALFTONE filtering instead of nearest neighbour
	int window_w, window_h; // initial client size; 0 = game default
	int window_x, window_y; // last normal position (workspace coords); POS_UNSET = centre
	BOOL resizable;
	BOOL start_maximized;
	BOOL dpi_aware;
	BOOL hires;         // render text natively at physical resolution
	int min_lw, min_lh; // smallest logical size the UI is allowed to get; 0 = game's own minimum
	int mode;           // MODE_* applied to the first window open
	BOOL exclusive_fullscreen; // 0 = fullscreen is a borderless window at desktop resolution
} cfg;

enum { MODE_GAME, MODE_WINDOWED, MODE_FULLSCREEN };
#define POS_UNSET (-32768)

#define ADDR_FULLSCREEN_ACTIVE 0x018E20F0 // char: game believes it owns an exclusive display mode
#define ADDR_PTR_CHANGE_DISPLAY_SETTINGS_W 0x0160DFDC // unicows thunk slot, called as (DEVMODEW*, DWORD)

static double g_zoom = 1.0; // current requested zoom (hotkeys change it)
static volatile LONG g_phys_w, g_phys_h, g_log_w, g_log_h;
static HWND g_hwnd;

static WCHAR g_dir[MAX_PATH];
static WCHAR g_ini[MAX_PATH];
static FILE *g_log;
static BOOL g_first_open_done;

static void modlog(const char *fmt, ...)
{
	va_list ap;
	if (!g_log)
		return;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

static double ini_double(const WCHAR *sec, const WCHAR *key, double def, const WCHAR *ini)
{
	WCHAR buf[64];
	GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
	if (!buf[0])
		return def;
	if (!lstrcmpiW(buf, L"auto"))
		return 0.0;
	return _wtof(buf);
}

static void load_config(void)
{
	const WCHAR *ini = g_ini;
	cfg.zoom = ini_double(L"display", L"zoom", 1.0, ini);
	cfg.zoom_step = ini_double(L"display", L"zoom_step", 0.25, ini);
	cfg.smooth = GetPrivateProfileIntW(L"display", L"smooth", 1, ini);
	cfg.window_w = GetPrivateProfileIntW(L"display", L"window_width", 0, ini);
	cfg.window_h = GetPrivateProfileIntW(L"display", L"window_height", 0, ini);
	cfg.window_x = GetPrivateProfileIntW(L"display", L"window_x", POS_UNSET, ini);
	cfg.window_y = GetPrivateProfileIntW(L"display", L"window_y", POS_UNSET, ini);
	cfg.resizable = GetPrivateProfileIntW(L"display", L"resizable", 1, ini);
	cfg.start_maximized = GetPrivateProfileIntW(L"display", L"start_maximized", 0, ini);
	cfg.dpi_aware = GetPrivateProfileIntW(L"display", L"dpi_aware", 1, ini);
	cfg.hires = GetPrivateProfileIntW(L"display", L"hires_text", 1, ini);
	cfg.min_lw = GetPrivateProfileIntW(L"display", L"min_logical_width", 0, ini);
	cfg.min_lh = GetPrivateProfileIntW(L"display", L"min_logical_height", 0, ini);
	cfg.exclusive_fullscreen = GetPrivateProfileIntW(L"display", L"exclusive_fullscreen", 0, ini);
	{
		WCHAR mode[32];
		GetPrivateProfileStringW(L"display", L"mode", L"game", mode, 32, ini);
		cfg.mode = !lstrcmpiW(mode, L"windowed") ? MODE_WINDOWED
			: !lstrcmpiW(mode, L"fullscreen") ? MODE_FULLSCREEN : MODE_GAME;
	}
	if (cfg.zoom_step <= 0)
		cfg.zoom_step = 0.25;
}

// ---- remembered state -------------------------------------------------------------------

static void ini_put(const WCHAR *key, const WCHAR *fmt, ...)
{
	WCHAR v[64];
	va_list ap;
	va_start(ap, fmt);
	wvsprintfW(v, fmt, ap);
	va_end(ap);
	if (!WritePrivateProfileStringW(L"display", key, v, g_ini))
		modlog("cannot write %ls (error %lu)", g_ini, GetLastError());
}

static void save_zoom(void)
{
	char buf[32];
	WCHAR w[32];
	sprintf(buf, "%.2f", g_zoom);
	MultiByteToWideChar(CP_ACP, 0, buf, -1, w, 32);
	ini_put(L"zoom", L"%s", w);
}

static BOOL is_fullscreen_window(HWND hwnd)
{
	return (GetWindowLongW(hwnd, GWL_STYLE) & WS_POPUP) != 0;
}

// Remember mode, and (when windowed) the normal position/client size and maximised state.
static void save_window_state(HWND hwnd, const char *why)
{
	WINDOWPLACEMENT wpl = { sizeof wpl };
	RECT frame = { 0, 0, 0, 0 };
	int cw, ch;
	BOOL maxed;

	if (!hwnd || !IsWindow(hwnd) || !g_first_open_done)
		return;
	if (is_fullscreen_window(hwnd)) {
		ini_put(L"mode", L"fullscreen");
		modlog("saved state (%s): fullscreen", why);
		return;
	}
	if (!GetWindowPlacement(hwnd, &wpl))
		return;
	AdjustWindowRectEx(&frame, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), FALSE,
		(DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE));
	cw = (wpl.rcNormalPosition.right - wpl.rcNormalPosition.left) - (frame.right - frame.left);
	ch = (wpl.rcNormalPosition.bottom - wpl.rcNormalPosition.top) - (frame.bottom - frame.top);
	maxed = wpl.showCmd == SW_SHOWMAXIMIZED ||
		(wpl.showCmd == SW_SHOWMINIMIZED && (wpl.flags & WPF_RESTORETOMAXIMIZED));
	ini_put(L"mode", L"windowed");
	if (cw > 0 && ch > 0) {
		ini_put(L"window_width", L"%d", cw);
		ini_put(L"window_height", L"%d", ch);
		ini_put(L"window_x", L"%d", wpl.rcNormalPosition.left);
		ini_put(L"window_y", L"%d", wpl.rcNormalPosition.top);
	}
	ini_put(L"start_maximized", L"%d", maxed ? 1 : 0);
	modlog("saved state (%s): windowed %dx%d at %ld,%ld maximized=%d", why, cw, ch,
		wpl.rcNormalPosition.left, wpl.rcNormalPosition.top, maxed);
}

// Put the window back where it was last time, if that spot is on a monitor of this PC.
static void restore_placement(HWND hwnd)
{
	WINDOWPLACEMENT wpl = { sizeof wpl };
	RECT r;
	if (!hwnd || cfg.window_x == POS_UNSET || cfg.window_y == POS_UNSET || is_fullscreen_window(hwnd))
		return;
	if (!GetWindowPlacement(hwnd, &wpl))
		return;
	r = wpl.rcNormalPosition;
	OffsetRect(&r, cfg.window_x - r.left, cfg.window_y - r.top);
	// rcNormalPosition is in workspace coordinates; close enough to screen ones for this test
	if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
		modlog("saved position %d,%d is off-screen here; centring instead", cfg.window_x, cfg.window_y);
		return;
	}
	wpl.rcNormalPosition = r;
	wpl.showCmd = cfg.start_maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
	wpl.flags = 0;
	SetWindowPlacement(hwnd, &wpl);
}

// Zoom actually used for a physical client size: never lets the logical UI drop below the
// configured minimum, never below 1.
static double effective_zoom(int pw, int ph)
{
	double z = g_zoom;
	double zmax_w = cfg.min_lw > 0 ? (double)pw / cfg.min_lw : z;
	double zmax_h = cfg.min_lh > 0 ? (double)ph / cfg.min_lh : z;
	if (z > zmax_w)
		z = zmax_w;
	if (z > zmax_h)
		z = zmax_h;
	return z < 1.0 ? 1.0 : z;
}

static BOOL zoomed(void)
{
	return g_log_w > 0 && (g_log_w != g_phys_w || g_log_h != g_phys_h);
}

// ---- hooks ----------------------------------------------------------------------------

typedef short(__fastcall *open_window_t)(void *self, void *edx, void *title, int idx, int w, int h, int flags,
	int min_h, int max_h, int min_w, int max_w);
typedef int(__fastcall *create_back_buffer_t)(void *self, void *edx, int w, int h);
typedef char(__fastcall *vf_bool_t)(void *self, void *edx);

static open_window_t orig_open_window;
static create_back_buffer_t orig_create_back_buffer;
static WNDPROC orig_main_wndproc;

static short __fastcall hk_open_window(void *self, void *edx, void *title, int idx, int w, int h, int flags,
	int min_h, int max_h, int min_w, int max_w)
{
	static BOOL once;
	short r;
	modlog("open_window in  w=%d h=%d flags=%#x minW=%d minH=%d maxW=%d maxH=%d",
		(short)w, (short)h, flags & 0xFF, (short)min_w, (short)min_h, (short)max_w, (short)max_h);
	if (!once) {
		once = TRUE;
		if (cfg.dpi_aware)
			SetProcessDPIAware();
		if (cfg.zoom <= 0) {
			HDC dc = GetDC(NULL);
			g_zoom = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
			ReleaseDC(NULL, dc);
		}
		// The game re-opens fullscreen whenever its window is smaller than this minimum
		// (WM_ACTIVATE), so the logical UI size must never go below it.
		if (cfg.min_lw <= 0)
			cfg.min_lw = (short)min_w > 0 ? (short)min_w : 1024;
		if (cfg.min_lh <= 0)
			cfg.min_lh = (short)min_h > 0 ? (short)min_h : 768;
		// Window mode is only forced on the first open so the in-game toggle keeps working.
		// Flag 8 = restore saved placement (which also restores the saved fullscreen state).
		if (cfg.mode == MODE_WINDOWED)
			flags &= ~(4 | 8);
		else if (cfg.mode == MODE_FULLSCREEN)
			flags = (flags & ~8) | 4;
		if (cfg.start_maximized && !(flags & 4) && cfg.window_x == POS_UNSET)
			flags |= 2; // with a saved position, restore_placement maximises instead
		if (cfg.window_w > 0 && cfg.window_h > 0) {
			w = cfg.window_w;
			h = cfg.window_h;
		}
		modlog("zoom %.2f, min logical %dx%d", g_zoom, cfg.min_lw, cfg.min_lh);
	}
	// Unlock the window size: the game's spec caps it at 1600x1200. Any max >= 0 also makes
	// the platform open choose the resizable WS_OVERLAPPEDWINDOW style.
	min_w = cfg.min_lw;
	min_h = cfg.min_lh;
	if (cfg.resizable) {
		max_w = 0x7FFF;
		max_h = 0x7FFF;
	}
	r = orig_open_window(self, edx, title, idx, w, h, flags, min_h, max_h, min_w, max_w);
	modlog("open_window out w=%d h=%d -> %d", (short)w, (short)h, r);
	if (r == 0) {
		HWND hwnd = *(HWND *)((BYTE *)self + WIN_HWND);
		if (!g_first_open_done) {
			restore_placement(hwnd);
			g_first_open_done = TRUE;
		}
		else {
			// in-game windowed/fullscreen toggle (or the game's own fallback)
			save_window_state(hwnd, "mode change");
		}
	}
	return r;
}

static int __fastcall hk_create_back_buffer(void *self, void *edx, int w, int h)
{
	int pw = (short)w, ph = (short)h, lw, lh;
	double z;
	HWND hwnd = *(HWND *)((BYTE *)self + WIN_HWND);

	// Borderless fullscreen: the game made a WS_POPUP of its "fullscreen" size at (0,0) but the
	// display mode was not changed (see fake_change_display_settings). Cover the monitor.
	if (!cfg.exclusive_fullscreen && *(volatile char *)ADDR_FULLSCREEN_ACTIVE && hwnd) {
		MONITORINFO mi = { sizeof mi };
		GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
		pw = mi.rcMonitor.right - mi.rcMonitor.left;
		ph = mi.rcMonitor.bottom - mi.rcMonitor.top;
		// the game ignores WM_SIZE while fullscreen, so this does not recurse
		SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, pw, ph,
			SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}
	z = effective_zoom(pw, ph);
	lw = (int)floor(pw / z + 0.5);
	lh = (int)floor(ph / z + 0.5);
	if (lw < 1)
		lw = 1;
	if (lh < 1)
		lh = 1;
	InterlockedExchange(&g_phys_w, pw);
	InterlockedExchange(&g_phys_h, ph);
	InterlockedExchange(&g_log_w, lw);
	InterlockedExchange(&g_log_h, lh);
	g_hwnd = hwnd;
	modlog("back buffer: client %dx%d -> logical %dx%d (zoom %.3f)", pw, ph, lw, lh, z);
	{
		int r = orig_create_back_buffer(self, edx, lw, lh);
		if (r == 0)
			hires_on_back_buffer((BYTE *)self, pw, ph, lw, lh);
		return r;
	}
}

// Replaces WINDOWS_RENDER_THREAD::present entirely (only two BitBlt sites in the exe).
static void __fastcall hk_present(BYTE *self, void *edx, HDC hdc)
{
	BYTE *win = *(BYTE **)(self + RT_WINDOW);
	vf_bool_t is_full = (vf_bool_t)(*(void ***)win)[WIN_VF_FULL_REDRAW / 4];
	BOOL full = is_full(win, NULL) != 0;
	HDC src = *(HDC *)(win + WIN_MEM_DC);
	int lw = *(short *)(win + WIN_BB_WIDTH), lh = *(short *)(win + WIN_BB_HEIGHT);
	int x0 = *(short *)(win + WIN_DIRTY_X0), x1 = *(short *)(win + WIN_DIRTY_X1);
	int y0 = *(short *)(win + WIN_DIRTY_Y0), y1 = *(short *)(win + WIN_DIRTY_Y1);
	RECT cr;
	int pw, ph, saved;

	GetClientRect(*(HWND *)(win + WIN_HWND), &cr);
	pw = cr.right;
	ph = cr.bottom;

	// Debug capture: creating <dll dir>\shot.req makes the next frame land in shot.bmp.
	{
		static DWORD last_check;
		DWORD now = GetTickCount();
		if (now - last_check > 250) {
			WCHAR req[MAX_PATH], out[MAX_PATH];
			last_check = now;
			wsprintfW(req, L"%s\\shot.req", g_dir);
			if (GetFileAttributesW(req) != INVALID_FILE_ATTRIBUTES) {
				DeleteFileW(req);
				wsprintfW(out, L"%s\\shot.bmp", g_dir);
				modlog("capture -> shot.bmp: %s", hires_dump(out, win) ? "ok" : "failed");
			}
		}
	}

	if (hires_active()) {
		hires_present(hdc, win, full);
		return;
	}

	if (pw == lw && ph == lh) { // unzoomed: original behaviour
		if (full)
			BitBlt(hdc, 0, 0, *(short *)(self + RT_WIDTH), *(short *)(self + RT_HEIGHT), src, 0, 0, SRCCOPY);
		else
			BitBlt(hdc, x0, y0, x1 - x0 + 1, y1 - y0 + 1, src, x0, y0, SRCCOPY);
		return;
	}

	// Stretch the whole frame every time so the src->dst mapping is identical for every
	// update (no seams); a clip rect limits the work to the dirty area.
	saved = SaveDC(hdc);
	if (!full && lw > 0 && lh > 0) {
		IntersectClipRect(hdc, (x0 - 1) * pw / lw, (y0 - 1) * ph / lh,
			((x1 + 2) * pw + lw - 1) / lw, ((y1 + 2) * ph + lh - 1) / lh);
	}
	if (cfg.smooth) {
		SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, NULL);
	}
	else {
		SetStretchBltMode(hdc, COLORONCOLOR);
	}
	StretchBlt(hdc, 0, 0, pw, ph, src, 0, 0, lw, lh, SRCCOPY);
	RestoreDC(hdc, saved);
}

static void apply_zoom(HWND hwnd, double z)
{
	RECT cr;
	double zmax = 4.0;
	GetClientRect(hwnd, &cr);
	// Beyond what the window can show (UI must stay >= min logical size) further steps would
	// change nothing visible and make zooming back out feel stuck, so clamp here.
	if (cfg.min_lw > 0 && cr.right / (double)cfg.min_lw < zmax)
		zmax = cr.right / (double)cfg.min_lw;
	if (cfg.min_lh > 0 && cr.bottom / (double)cfg.min_lh < zmax)
		zmax = cr.bottom / (double)cfg.min_lh;
	if (z > zmax)
		z = zmax;
	if (z < 1.0)
		z = 1.0;
	g_zoom = z;
	modlog("hotkey zoom %.2f", z);
	save_zoom();
	// The game's WM_SIZE handler rebuilds the back buffer (via our hook) and re-lays out.
	SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(cr.right, cr.bottom));
	InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK hk_main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		if (zoomed() && g_phys_w > 0 && g_phys_h > 0) {
			int x = GET_X_LPARAM(lp) * g_log_w / g_phys_w;
			int y = GET_Y_LPARAM(lp) * g_log_h / g_phys_h;
			lp = MAKELPARAM(x, y);
		}
		break;

	case WM_GETMINMAXINFO:
		// The game pins the track size to its (now logical) window size; size it ourselves.
		if (cfg.resizable || zoomed()) {
			MINMAXINFO *mmi = (MINMAXINFO *)lp;
			RECT r = { 0, 0, cfg.min_lw, cfg.min_lh };
			AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), FALSE,
				(DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE));
			mmi->ptMinTrackSize.x = r.right - r.left;
			mmi->ptMinTrackSize.y = r.bottom - r.top;
			if (!cfg.resizable && g_phys_w > 0) {
				RECT f = { 0, 0, g_phys_w, g_phys_h };
				AdjustWindowRectEx(&f, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), FALSE,
					(DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE));
				mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = f.right - f.left;
				mmi->ptMinTrackSize.y = mmi->ptMaxTrackSize.y = f.bottom - f.top;
			}
			return 0;
		}
		break;

	case WM_EXITSIZEMOVE:
		save_window_state(hwnd, "move/resize");
		break;

	case WM_SYSCOMMAND:
		switch (wp & 0xFFF0) {
		case SC_MAXIMIZE:
		case SC_RESTORE: {
			LRESULT r = orig_main_wndproc(hwnd, msg, wp, lp);
			save_window_state(hwnd, "maximize/restore");
			return r;
		}
		}
		break;

	case WM_CLOSE:
	case WM_DESTROY:
	case WM_ENDSESSION:
		save_window_state(hwnd, "close");
		break;

	case WM_PAINT:
		// The game marks BeginPaint's rcPaint (physical pixels) dirty; repaint everything.
		if (zoomed())
			InvalidateRect(hwnd, NULL, FALSE);
		break;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000)) {
			if (wp == VK_OEM_PLUS || wp == VK_ADD) {
				apply_zoom(hwnd, g_zoom + cfg.zoom_step);
				return 0;
			}
			if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) {
				apply_zoom(hwnd, g_zoom - cfg.zoom_step);
				return 0;
			}
			if (wp == '0' || wp == VK_NUMPAD0) {
				apply_zoom(hwnd, 1.0);
				return 0;
			}
		}
		break;
	}
	return orig_main_wndproc(hwnd, msg, wp, lp);
}

// Borderless fullscreen: report success for every mode change without touching the display.
// The game then keeps its fullscreen logic (WS_POPUP window, minimise on deactivate) at the
// desktop resolution.
static LONG WINAPI fake_change_display_settings(DEVMODEW *dm, DWORD flags)
{
	modlog("ChangeDisplaySettingsW(%s%lux%lu, %#lx) faked", dm ? "" : "reset ",
		dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, flags);
	return DISP_CHANGE_SUCCESSFUL;
}

// ---- install ------------------------------------------------------------------------------

static BOOL install(const char *name, DWORD addr, const BYTE *sig, size_t sig_len, void *hook,
	size_t prologue_len, void **tramp)
{
	if (!hook_verify(addr, sig, sig_len)) {
		modlog("%s: signature mismatch at %08lx - not fm.exe 7.0.2? hook skipped", name, addr);
		return FALSE;
	}
	if (!hook_install(addr, hook, prologue_len, tramp)) {
		modlog("%s: install failed (%lu)", name, GetLastError());
		return FALSE;
	}
	modlog("%s: hooked %08lx", name, addr);
	return TRUE;
}

static void install_all(void)
{
	BOOL ok = TRUE;
	// verify everything first so we never leave a half-patched exe
	ok &= hook_verify(ADDR_OPEN_WINDOW, SIG_OPEN_WINDOW, sizeof SIG_OPEN_WINDOW);
	ok &= hook_verify(ADDR_CREATE_BACK_BUFFER, SIG_CREATE_BACK_BUFFER, sizeof SIG_CREATE_BACK_BUFFER);
	ok &= hook_verify(ADDR_MAIN_WNDPROC, SIG_MAIN_WNDPROC, sizeof SIG_MAIN_WNDPROC);
	ok &= hook_verify(ADDR_PRESENT, SIG_PRESENT, sizeof SIG_PRESENT);
	if (!ok) {
		modlog("signature check failed; no hooks installed");
		return;
	}
	install("open_window", ADDR_OPEN_WINDOW, SIG_OPEN_WINDOW, sizeof SIG_OPEN_WINDOW,
		(void *)hk_open_window, 7, (void **)&orig_open_window);
	install("create_back_buffer", ADDR_CREATE_BACK_BUFFER, SIG_CREATE_BACK_BUFFER,
		sizeof SIG_CREATE_BACK_BUFFER, (void *)hk_create_back_buffer, 5, (void **)&orig_create_back_buffer);
	install("main_wndproc", ADDR_MAIN_WNDPROC, SIG_MAIN_WNDPROC, sizeof SIG_MAIN_WNDPROC,
		(void *)hk_main_wndproc, 5, (void **)&orig_main_wndproc);
	install("present", ADDR_PRESENT, SIG_PRESENT, sizeof SIG_PRESENT, (void *)hk_present, 5, NULL);
	if (cfg.hires)
		hires_install(modlog, cfg.smooth);
	if (!cfg.exclusive_fullscreen) {
		// the slot is in .data (writable); it normally holds the lazy unicows resolver stub
		*(void **)ADDR_PTR_CHANGE_DISPLAY_SETTINGS_W = (void *)fake_change_display_settings;
		modlog("ChangeDisplaySettingsW: borderless fullscreen enabled");
	}
}

// Marker export (see fm07mod.def): lets fm07launch.exe recognise a dsound.dll that is this mod.
DWORD fm07mod_version(void)
{
	return 0x00010000;
}

// True only inside the exact fm.exe build all addresses were taken from.
static BOOL host_is_fm_702(void)
{
	BYTE *base = (BYTE *)GetModuleHandleW(NULL);
	IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	return nt->FileHeader.TimeDateStamp == FM_TIMESTAMP && nt->OptionalHeader.SizeOfImage == FM_IMAGE_SIZE &&
		(DWORD)base == 0x00400000;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		WCHAR logpath[MAX_PATH], name[64], *slash;
		DisableThreadLibraryCalls(inst);
		// As dsound.dll we may be loaded by something other than fm.exe: then just forward sound.
		if (!host_is_fm_702())
			return TRUE;
		// dsound.dll proxy and launcher-injected fm07mod.dll can both be present: hook once.
		wsprintfW(name, L"Local\\fm07mod_%lu", GetCurrentProcessId());
		if (CreateMutexW(NULL, FALSE, name) && GetLastError() == ERROR_ALREADY_EXISTS)
			return TRUE;
		GetModuleFileNameW(inst, g_dir, MAX_PATH);
		slash = wcsrchr(g_dir, L'\\');
		if (slash)
			*slash = 0;
		wsprintfW(logpath, L"%s\\fm07mod.log", g_dir);
		wsprintfW(g_ini, L"%s\\fm07mod.ini", g_dir);
		g_log = _wfopen(logpath, L"w");
		load_config();
		g_zoom = cfg.zoom > 0 ? cfg.zoom : 1.0;
		modlog("fm07mod loaded; zoom=%.2f window=%dx%d resizable=%d smooth=%d", cfg.zoom,
			cfg.window_w, cfg.window_h, cfg.resizable, cfg.smooth);
		install_all();
	}
	return TRUE;
}
