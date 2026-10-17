// High-resolution shadow buffer for fm.exe 7.0.2. See hires.h and README "Hi-res rendering".
//
// Surfaces (IMAGE_BASE) in fm.exe: vtable +0x14 width, +0x18 height, +0x2c pixel_ptr(pt,&pitch),
// +0x30 lock_rect(rect,&pitch) -> uint32*, +0x34 unlock(); clip rect (local coords) at +0x08,
// origin at +0x10. All drawing writes raw pixels between lock_rect and unlock.
//
// We patch lock_rect/unlock in the SI_WINDOW_PLATFORM vtable: every locked rect is upscaled from
// the logical buffer into the shadow buffer at unlock ("fallback"). Text drawing
// (FONT draw_string) is instead replayed onto a proxy surface backed by the shadow buffer with
// the T2K transform scaled, and its fallback is suppressed - so text is rasterised natively.

#include "hires.h"
#include "hook.h"
#include <math.h>
#include <string.h>

// ---- fm.exe 7.0.2 addresses -----------------------------------------------------------------

#define VT_WINDOW_PLATFORM      0x013F1E4C // SI_WINDOW_PLATFORM::vftable (.rdata)
#define VT_SLOT_LOCK_RECT       0x30
#define VT_SLOT_UNLOCK          0x34
#define ADDR_WIN_LOCK_RECT      0x00E01450 // SI_WINDOW::lock_rect(rect, &pitch)
#define ADDR_SHARED_RET         0x00E50ED0 // folded "ret" used for unlock

#define ADDR_DRAW_STRING        0x00EE1E90 // FONT::draw_string(surf,x,y,&col,&outline,str,hit_cb)
#define ADDR_SET_FONT_SIZE      0x00EE1270 // FONT::select(style{.,flags}, size_x, size_y)
#define ADDR_T2K_SET_TRANSFORM  0x00F05090 // cdecl (scaler, 0, xdpi, ydpi, int matrix[4], 1, &err)
#define ADDR_FILL_RECT          0x00DFDB50 // IMAGE_BASE::fill_rect(rect, colour, alpha)
#define ADDR_BLIT_ALPHA         0x00DFCB00 // IMAGE_BASE::blit_alpha(x, y, src, alpha)
#define ADDR_BLIT_COPY          0x00DFCC30 // IMAGE_BASE::blit_copy(x, y, src)

static const BYTE SIG_DRAW_STRING[] = { 0x83, 0xEC, 0x68, 0x55, 0x8B, 0x29, 0x8A, 0x45, 0x00 };
static const BYTE SIG_SET_FONT_SIZE[] = { 0x83, 0xEC, 0x38, 0x56, 0x8B, 0x31, 0x8A, 0x06 };
static const BYTE SIG_T2K_SET_TRANSFORM[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48, 0x8B, 0x45, 0x20 };
static const BYTE SIG_FILL_RECT[] = { 0x83, 0xEC, 0x20, 0x8B, 0x44, 0x24, 0x24, 0x66, 0x8B, 0x50, 0x02 };
static const BYTE SIG_BLIT_ALPHA[] = { 0x83, 0xEC, 0x1C, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x30 };
static const BYTE SIG_BLIT_COPY[] = { 0x83, 0xEC, 0x1C, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x30 };

// SI_WINDOW_PLATFORM back buffer fields
#define WIN_BITS     0x7C
#define WIN_BB_W     0x80
#define WIN_BB_H     0x82
#define WIN_PITCH    0x88 // short, in pixels
#define WIN_DIRTY_X0 0x8E
#define WIN_DIRTY_X1 0x90
#define WIN_DIRTY_Y0 0x92
#define WIN_DIRTY_Y1 0x94
// IMAGE_BASE fields
#define SURF_CLIP    0x08 // short x, y, w, h (local coordinates)
#define SURF_ORIGIN  0x10 // short x, y

// FONT object (*font_handle)
#define FONT_LOADED  0x00
#define FONT_FLAGS   0x1C // style flags; part of the glyph cache key
#define FONT_SIZE_X  0x20
#define FONT_SIZE_Y  0x21

// Style flag bits unused by the renderer; set during the hi-res pass so its glyphs get their
// own cache entries (key = flags<<16 | size_y<<8 | size_x). Bits 10..14 = zoom generation.
#define HIRES_TAG    0x8000

typedef DWORD *(__fastcall *lock_rect_t)(BYTE *self, void *edx, short *rect, short *pitch);
typedef void(__fastcall *draw_string_t)(void **font, void *edx, BYTE *surf, int x, int y,
	DWORD *colour, DWORD *outline, void *str, void *hit_cb);
typedef void(__fastcall *fill_rect_t)(BYTE *surf, void *edx, short *rect, DWORD colour, int alpha);
typedef void(__fastcall *blit_alpha_t)(BYTE *dst, void *edx, int x, int y, BYTE *src, int alpha);
typedef void(__fastcall *blit_copy_t)(BYTE *dst, void *edx, int x, int y, BYTE *src);
typedef short(__fastcall *vf_short_t)(void *self, void *edx);
typedef char(__fastcall *vf_char_t)(void *self, void *edx);
typedef void(__fastcall *vf_get_colour_t)(void *self, void *edx, DWORD *colour);
typedef DWORD *(__fastcall *vf_pixel_ptr_t)(void *self, void *edx, short *pt, short *pitch);
typedef void(__fastcall *set_font_size_t)(void **font, void *edx, void *style, int sx, int sy);
typedef int(__cdecl *t2k_set_transform_t)(void *scaler, int a, int xdpi, int ydpi, int *matrix,
	int b, void *err);

static void (*logf_)(const char *fmt, ...);
static BOOL g_smooth;
static BOOL g_installed;

static draw_string_t orig_draw_string;
static t2k_set_transform_t orig_t2k_set_transform;
static fill_rect_t orig_fill_rect;
static blit_alpha_t orig_blit_alpha;
static blit_copy_t orig_blit_copy;
static const lock_rect_t win_lock_rect = (lock_rect_t)ADDR_WIN_LOCK_RECT;
static const set_font_size_t set_font_size = (set_font_size_t)ADDR_SET_FONT_SIZE;

// ---- shadow buffer state --------------------------------------------------------------------

static BYTE *g_win;
static int g_pw, g_ph, g_lw, g_lh;
static double g_zx = 1.0, g_zy = 1.0;
static HDC g_hdc;
static HBITMAP g_bmp;
static DWORD *g_bits;
static BOOL g_on;

static int g_suppress;      // >0: window unlocks do not upscale (op is replayed natively)
static int g_text_depth;    // draw_string recursion (shadow/outline styles call themselves)
static double g_tscale = 1.0; // applied to the next T2K transform
static int g_hx0, g_hy0;    // position of the current hi-res draw_string (shadow offsets)
static DWORD g_tag = HIRES_TAG;
static int g_gen;

#define MAX_PENDING 64
static short g_pending[MAX_PENDING][4];
static int g_npending;

static int sx_(int v) { return (int)floor(v * g_zx + 0.5); }
static int sy_(int v) { return (int)floor(v * g_zy + 0.5); }

BOOL hires_active(void) { return g_on; }

static void upscale(int x, int y, int w, int h);

void hires_on_back_buffer(BYTE *win, int pw, int ph, int lw, int lh)
{
	BITMAPINFO bi;
	double old_zx = g_zx;

	// Same geometry: the game kept its buffer (and will not redraw), so keep ours too.
	if (g_bmp && win == g_win && pw == g_pw && ph == g_ph && lw == g_lw && lh == g_lh) {
		g_on = TRUE;
		return;
	}
	g_on = FALSE;
	if (g_bmp) {
		DeleteObject(g_bmp);
		g_bmp = NULL;
		g_bits = NULL;
	}
	if (!g_hdc)
		g_hdc = CreateCompatibleDC(NULL);
	g_win = win;
	g_npending = 0;
	if (!g_installed || (pw == lw && ph == lh) || lw <= 0 || lh <= 0)
		return;

	memset(&bi, 0, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = pw;
	bi.bmiHeader.biHeight = -ph; // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	g_bmp = CreateDIBSection(g_hdc, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
	if (!g_bmp) {
		logf_("hires: CreateDIBSection %dx%d failed", pw, ph);
		return;
	}
	SelectObject(g_hdc, g_bmp);
	g_pw = pw;
	g_ph = ph;
	g_lw = lw;
	g_lh = lh;
	g_zx = (double)pw / lw;
	g_zy = (double)ph / lh;
	if (g_zx != old_zx) {
		g_gen = (g_gen + 1) & 0x1F;
		g_tag = HIRES_TAG | (DWORD)g_gen << 10;
	}
	g_on = TRUE;
	upscale(0, 0, lw, lh); // never start blank
	logf_("hires: shadow %dx%d for logical %dx%d", pw, ph, lw, lh);
}

// ---- fallback: upscale a logical rect into the shadow buffer ---------------------------------

static void upscale(int x, int y, int w, int h)
{
	const DWORD *src = *(DWORD **)(g_win + WIN_BITS);
	int pitch = *(short *)(g_win + WIN_PITCH);
	int lw = *(short *)(g_win + WIN_BB_W), lh = *(short *)(g_win + WIN_BB_H);
	int x0, x1, y0, y1, px, py;
	static int col_i[8192], col_w[8192];

	if (!src || lw != g_lw || lh != g_lh)
		return;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > lw) w = lw - x;
	if (y + h > lh) h = lh - y;
	if (w <= 0 || h <= 0)
		return;
	x0 = sx_(x); x1 = sx_(x + w);
	y0 = sy_(y); y1 = sy_(y + h);
	if (x1 > g_pw) x1 = g_pw;
	if (y1 > g_ph) y1 = g_ph;
	if (x1 - x0 > 8192)
		x1 = x0 + 8192;

	// per-column source index and 8-bit weight (pixel centres aligned)
	for (px = x0; px < x1; px++) {
		double f = (px + 0.5) / g_zx - 0.5;
		int i = (int)floor(f);
		int wt = (int)((f - i) * 256.0);
		if (i < 0) { i = 0; wt = 0; }
		if (i >= lw - 1) { i = lw - 1; wt = 0; }
		col_i[px - x0] = i;
		col_w[px - x0] = g_smooth ? wt : (wt >= 128 ? 256 : 0);
	}
	for (py = y0; py < y1; py++) {
		double f = (py + 0.5) / g_zy - 0.5;
		int j = (int)floor(f);
		int wy = (int)((f - j) * 256.0);
		const DWORD *r0, *r1;
		DWORD *dst = g_bits + (size_t)py * g_pw;
		if (j < 0) { j = 0; wy = 0; }
		if (j >= lh - 1) { j = lh - 1; wy = 0; }
		if (!g_smooth)
			wy = wy >= 128 ? 256 : 0;
		r0 = src + (size_t)j * pitch;
		r1 = wy ? r0 + pitch : r0;
		for (px = x0; px < x1; px++) {
			int i = col_i[px - x0], wx = col_w[px - x0];
			DWORD a = r0[i], b = wx ? r0[i + 1] : a, c = r1[i], d = wx ? r1[i + 1] : c;
			DWORD ix = 256 - wx, iy = 256 - wy;
			// weighted sums two channels at a time (lanes 16 bits apart; max 255*256 fits)
			DWORD rb0 = ((a & 0xFF00FF) * ix + (b & 0xFF00FF) * wx) >> 8 & 0xFF00FF;
			DWORD ga0 = ((a >> 8 & 0xFF00FF) * ix + (b >> 8 & 0xFF00FF) * wx) >> 8 & 0xFF00FF;
			DWORD rb1 = ((c & 0xFF00FF) * ix + (d & 0xFF00FF) * wx) >> 8 & 0xFF00FF;
			DWORD ga1 = ((c >> 8 & 0xFF00FF) * ix + (d >> 8 & 0xFF00FF) * wx) >> 8 & 0xFF00FF;
			DWORD rb = (rb0 * iy + rb1 * wy) >> 8 & 0xFF00FF;
			DWORD ga = (ga0 * iy + ga1 * wy) >> 8 & 0xFF00FF;
			dst[px] = rb | ga << 8;
		}
	}
}

static void flush_pending(void)
{
	int i;
	for (i = 0; i < g_npending; i++)
		upscale(g_pending[i][0], g_pending[i][1], g_pending[i][2], g_pending[i][3]);
	g_npending = 0;
}

// ---- window vtable patches -------------------------------------------------------------------

static DWORD *__fastcall hk_win_lock_rect(BYTE *self, void *edx, short *rect, short *pitch)
{
	DWORD *p = win_lock_rect(self, edx, rect, pitch);
	if (g_on && self == g_win && !g_suppress) {
		if (g_npending == MAX_PENDING)
			flush_pending();
		memcpy(g_pending[g_npending++], rect, 4 * sizeof(short));
	}
	return p;
}

static void __fastcall hk_win_unlock(BYTE *self, void *edx)
{
	(void)edx;
	if (g_on && self == g_win)
		flush_pending();
}

// ---- proxy surface backed by the shadow buffer -----------------------------------------------

static short __fastcall px_width(void *self, void *edx) { (void)self; (void)edx; return (short)g_pw; }
static short __fastcall px_height(void *self, void *edx) { (void)self; (void)edx; return (short)g_ph; }
static DWORD *__fastcall px_lock(void *self, void *edx, short *rect, short *pitch)
{
	(void)self; (void)edx;
	*pitch = (short)g_pw;
	return g_bits + (size_t)rect[1] * g_pw + rect[0];
}
static void __fastcall px_unlock(void *self, void *edx) { (void)self; (void)edx; }
static void __fastcall px_unexpected(void *self, void *edx)
{
	(void)self; (void)edx;
	logf_("hires: unexpected proxy surface vcall");
}

static void *g_proxy_vt[64];
static struct {
	void **vt;
	void **vt2;
	short clip[4];
	short origin[2];
	BYTE pad[0x100];
} g_proxy;

// Map the window's clip/origin (logical, local) to the shadow buffer.
static void proxy_from_window(BYTE *win)
{
	short *clip = (short *)(win + SURF_CLIP);
	short ox = *(short *)(win + SURF_ORIGIN), oy = *(short *)(win + SURF_ORIGIN + 2);
	int sox = sx_(ox), soy = sy_(oy);
	g_proxy.origin[0] = (short)sox;
	g_proxy.origin[1] = (short)soy;
	if (clip[2] == -1) {
		memcpy(g_proxy.clip, clip, sizeof g_proxy.clip);
	}
	else {
		int x0 = sx_(clip[0] + ox) - sox, x1 = sx_(clip[0] + clip[2] + ox) - sox;
		int y0 = sy_(clip[1] + oy) - soy, y1 = sy_(clip[1] + clip[3] + oy) - soy;
		g_proxy.clip[0] = (short)x0;
		g_proxy.clip[1] = (short)y0;
		g_proxy.clip[2] = (short)(x1 - x0);
		g_proxy.clip[3] = (short)(y1 - y0);
	}
}

// ---- text ------------------------------------------------------------------------------------

static int __cdecl hk_t2k_set_transform(void *scaler, int a, int xdpi, int ydpi, int *m, int b,
	void *err)
{
	int saved[4], i, r;
	if (g_tscale == 1.0 || !m)
		return orig_t2k_set_transform(scaler, a, xdpi, ydpi, m, b, err);
	for (i = 0; i < 4; i++) {
		saved[i] = m[i];
		m[i] = (int)floor(m[i] * g_tscale + 0.5);
	}
	r = orig_t2k_set_transform(scaler, a, xdpi, ydpi, m, b, err);
	memcpy(m, saved, sizeof saved);
	return r;
}

static void __fastcall hk_draw_string(void **font, void *edx, BYTE *surf, int x, int y,
	DWORD *colour, DWORD *outline, void *str, void *hit_cb)
{
	BYTE *f = font ? (BYTE *)*font : NULL;
	DWORD flags;
	BYTE sx, sy;
	struct { void *vt; DWORD flags; } style;
	short ox, oy;

	if (g_text_depth && surf == (BYTE *)&g_proxy && !hit_cb) {
		// shadow/outline/emboss styles recurse with +-1px offsets; scale them
		x = g_hx0 + (int)floor(((short)x - g_hx0) * g_zx + 0.5);
		y = g_hy0 + (int)floor(((short)y - g_hy0) * g_zy + 0.5);
	}
	if (g_text_depth || hit_cb || !g_on || surf != g_win || !f || !f[FONT_LOADED]) {
		orig_draw_string(font, edx, surf, x, y, colour, outline, str, hit_cb);
		return;
	}
	g_text_depth++;

	// 1. logical text into the game's buffer (kept consistent), without the blurry fallback
	g_suppress++;
	orig_draw_string(font, edx, surf, x, y, colour, outline, str, hit_cb);
	g_suppress--;

	// 2. the same text rasterised at physical size into the shadow buffer
	flags = *(DWORD *)(f + FONT_FLAGS);
	sx = f[FONT_SIZE_X];
	sy = f[FONT_SIZE_Y];
	style.vt = NULL;
	style.flags = flags;
	f[FONT_SIZE_X] = 0; // force the transform to be rebuilt
	g_tscale = (g_zx + g_zy) / 2;
	set_font_size(font, NULL, &style, sx, sy);
	g_tscale = 1.0;
	*(DWORD *)(f + FONT_FLAGS) = flags | g_tag;

	proxy_from_window(surf);
	ox = *(short *)(surf + SURF_ORIGIN);
	oy = *(short *)(surf + SURF_ORIGIN + 2);
	// x/y are shorts in 32-bit stack slots: the game only reads the low 16 bits, and callers
	// leave junk in the high half
	g_hx0 = sx_((short)x + ox) - g_proxy.origin[0];
	g_hy0 = sy_((short)y + oy) - g_proxy.origin[1];
	orig_draw_string(font, edx, (BYTE *)&g_proxy, g_hx0, g_hy0, colour, outline, str, NULL);

	// 3. restore the logical font transform
	*(DWORD *)(f + FONT_FLAGS) = flags;
	f[FONT_SIZE_X] = 0;
	set_font_size(font, NULL, &style, sx, sy);

	g_text_depth--;
}

// ---- fills -----------------------------------------------------------------------------------

// Clip a local rect against a surface's clip rect and convert it to physical shadow pixels.
static BOOL map_rect(BYTE *surf, const short *rect, int *x0, int *y0, int *x1, int *y1)
{
	const short *clip = (const short *)(surf + SURF_CLIP);
	int ox = *(short *)(surf + SURF_ORIGIN), oy = *(short *)(surf + SURF_ORIGIN + 2);
	int l = rect[0], t = rect[1], r = rect[0] + rect[2], b = rect[1] + rect[3];
	if (clip[2] == -1)
		return FALSE;
	if (l < clip[0]) l = clip[0];
	if (t < clip[1]) t = clip[1];
	if (r > clip[0] + clip[2]) r = clip[0] + clip[2];
	if (b > clip[1] + clip[3]) b = clip[1] + clip[3];
	if (l >= r || t >= b)
		return FALSE;
	*x0 = sx_(l + ox); *x1 = sx_(r + ox);
	*y0 = sy_(t + oy); *y1 = sy_(b + oy);
	if (*x0 < 0) *x0 = 0;
	if (*y0 < 0) *y0 = 0;
	if (*x1 > g_pw) *x1 = g_pw;
	if (*y1 > g_ph) *y1 = g_ph;
	return *x0 < *x1 && *y0 < *y1;
}

static void shadow_fill(int x0, int y0, int x1, int y1, DWORD colour, DWORD a);

static void __fastcall hk_fill_rect(BYTE *surf, void *edx, short *rect, DWORD colour, int alpha)
{
	int x0, y0, x1, y1;

	if (!g_on || surf != g_win) {
		orig_fill_rect(surf, edx, rect, colour, alpha);
		return;
	}
	g_suppress++;
	orig_fill_rect(surf, edx, rect, colour, alpha);
	g_suppress--;
	if (map_rect(surf, rect, &x0, &y0, &x1, &y1))
		shadow_fill(x0, y0, x1, y1, colour, alpha & 0xFF);
}

static void shadow_fill(int x0, int y0, int x1, int y1, DWORD colour, DWORD a)
{
	int x, y;
	DWORD ia, crb, cga;
	if (a == 0xFF) {
		for (y = y0; y < y1; y++) {
			DWORD *d = g_bits + (size_t)y * g_pw;
			for (x = x0; x < x1; x++)
				d[x] = colour;
		}
		return;
	}
	// same formula as the game's MMX fill (0x00EAC040): (d*(255-a) + c*a) >> 8 per byte
	ia = 255 - a;
	crb = (colour & 0xFF00FF) * a;
	cga = (colour >> 8 & 0xFF00FF) * a;
	for (y = y0; y < y1; y++) {
		DWORD *d = g_bits + (size_t)y * g_pw;
		for (x = x0; x < x1; x++) {
			DWORD p = d[x];
			DWORD rb = ((p & 0xFF00FF) * ia + crb) >> 8 & 0xFF00FF;
			DWORD ga = ((p >> 8 & 0xFF00FF) * ia + cga) >> 8 & 0xFF00FF;
			d[x] = rb | ga << 8;
		}
	}
}

// ---- images ----------------------------------------------------------------------------------

// Redraw src (an IMAGE_BASE) at logical local (x,y) of the window into the shadow buffer,
// sampling it bilinearly at physical resolution. blend: per-pixel alpha "over" like the game's
// MMX blit (0x00EABC40), with premultiplied interpolation to avoid dark fringes.
static void shadow_image(BYTE *dst, int x, int y, BYTE *src, DWORD galpha, BOOL blend)
{
	void **vt = *(void ***)src;
	int sw = ((vf_short_t)vt[0x14 / 4])(src, NULL), sh = ((vf_short_t)vt[0x18 / 4])(src, NULL);
	short rect[4], pt[4], pitch;
	int x0, y0, x1, y1, px, py, ax, ay;
	const DWORD *base;
	static int col_i[8192], col_w[8192];

	if (sw <= 0 || sh <= 0)
		return;
	rect[0] = (short)x; rect[1] = (short)y; rect[2] = (short)sw; rect[3] = (short)sh;
	if (!map_rect(dst, rect, &x0, &y0, &x1, &y1))
		return;

	if (blend && ((vf_char_t)vt[0x20 / 4])(src, NULL)) {
		// solid image: the game fills with its colour (alpha modulated unless galpha == 0xff)
		DWORD c;
		((vf_get_colour_t)vt[0x24 / 4])(src, NULL, &c);
		shadow_fill(x0, y0, x1, y1, c, galpha == 0xFF ? 0xFF : (c >> 24) * galpha >> 8);
		return;
	}

	pt[0] = *(short *)(src + SURF_ORIGIN);
	pt[1] = *(short *)(src + SURF_ORIGIN + 2);
	pt[2] = (short)sw;
	pt[3] = (short)sh;
	base = ((vf_pixel_ptr_t)vt[0x2C / 4])(src, NULL, pt, &pitch);
	if (!base || x1 - x0 > 8192)
		return;

	ax = x + *(short *)(dst + SURF_ORIGIN);
	ay = y + *(short *)(dst + SURF_ORIGIN + 2);
	for (px = x0; px < x1; px++) {
		double f = (px + 0.5) / g_zx - 0.5 - ax;
		int i = (int)floor(f), wt = (int)((f - i) * 256.0);
		if (i < 0) { i = 0; wt = 0; }
		if (i >= sw - 1) { i = sw - 1; wt = 0; }
		col_i[px - x0] = i;
		col_w[px - x0] = wt;
	}
	for (py = y0; py < y1; py++) {
		double f = (py + 0.5) / g_zy - 0.5 - ay;
		int j = (int)floor(f), wy = (int)((f - j) * 256.0);
		const DWORD *r0, *r1;
		DWORD *d = g_bits + (size_t)py * g_pw;
		if (j < 0) { j = 0; wy = 0; }
		if (j >= sh - 1) { j = sh - 1; wy = 0; }
		r0 = base + (size_t)j * pitch;
		r1 = wy ? r0 + pitch : r0;
		for (px = x0; px < x1; px++) {
			int i = col_i[px - x0], wx = col_w[px - x0];
			DWORD s[4];
			int k, ch;
			unsigned wts[4];
			unsigned acc[4] = { 0, 0, 0, 0 }; // premultiplied b, g, r and alpha, scaled by 65536
			s[0] = r0[i]; s[1] = wx ? r0[i + 1] : s[0];
			s[2] = r1[i]; s[3] = wx ? r1[i + 1] : s[2];
			wts[0] = (256 - wx) * (256 - wy); wts[1] = wx * (256 - wy);
			wts[2] = (256 - wx) * wy; wts[3] = wx * wy;
			for (k = 0; k < 4; k++) {
				unsigned a = blend ? s[k] >> 24 : 255;
				unsigned wa = wts[k] * a / 255; // weight * alpha
				acc[3] += wts[k] * a;
				for (ch = 0; ch < 3; ch++)
					acc[ch] += wa * (s[k] >> (8 * ch) & 0xFF);
			}
			if (!blend) {
				d[px] = (acc[0] >> 16) | (acc[1] >> 16) << 8 | (acc[2] >> 16) << 16 | 0xFF000000;
			}
			else {
				unsigned a = (acc[3] >> 16) * galpha / 255; // coverage 0..255
				DWORD p = d[px], out = 0;
				unsigned da, na;
				if (!a)
					continue;
				for (ch = 0; ch < 3; ch++) {
					unsigned dc = p >> (8 * ch) & 0xFF;
					unsigned sc = (acc[ch] >> 16) * galpha / 255; // premultiplied source
					unsigned v = sc + dc * (255 - a) / 255;
					out |= (v > 255 ? 255 : v) << (8 * ch);
				}
				da = p >> 24;
				na = da > a ? da : a;
				d[px] = out | na << 24;
			}
		}
	}
}

static void __fastcall hk_blit_alpha(BYTE *dst, void *edx, int x, int y, BYTE *src, int alpha)
{
	if (!g_on || dst != g_win || !src) {
		orig_blit_alpha(dst, edx, x, y, src, alpha);
		return;
	}
	g_suppress++;
	orig_blit_alpha(dst, edx, x, y, src, alpha);
	g_suppress--;
	shadow_image(dst, (short)x, (short)y, src, alpha & 0xFF, TRUE);
}

static void __fastcall hk_blit_copy(BYTE *dst, void *edx, int x, int y, BYTE *src)
{
	if (!g_on || dst != g_win || !src) {
		orig_blit_copy(dst, edx, x, y, src);
		return;
	}
	g_suppress++;
	orig_blit_copy(dst, edx, x, y, src);
	g_suppress--;
	shadow_image(dst, (short)x, (short)y, src, 0xFF, FALSE);
}

// ---- present ---------------------------------------------------------------------------------

void hires_present(HDC hdc, BYTE *win, BOOL full)
{
	if (full) {
		BitBlt(hdc, 0, 0, g_pw, g_ph, g_hdc, 0, 0, SRCCOPY);
	}
	else {
		int x0 = sx_(*(short *)(win + WIN_DIRTY_X0)) - 1, x1 = sx_(*(short *)(win + WIN_DIRTY_X1) + 1) + 1;
		int y0 = sy_(*(short *)(win + WIN_DIRTY_Y0)) - 1, y1 = sy_(*(short *)(win + WIN_DIRTY_Y1) + 1) + 1;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		BitBlt(hdc, x0, y0, x1 - x0, y1 - y0, g_hdc, x0, y0, SRCCOPY);
	}
}

// ---- install ---------------------------------------------------------------------------------

static BOOL patch_vtable_slot(DWORD vt, int slot, DWORD expect, void *fn)
{
	DWORD *p = (DWORD *)(vt + slot), old;
	if (*p != expect)
		return FALSE;
	if (!VirtualProtect(p, 4, PAGE_READWRITE, &old))
		return FALSE;
	*p = (DWORD)fn;
	VirtualProtect(p, 4, old, &old);
	return TRUE;
}

BOOL hires_install(void (*log)(const char *fmt, ...), BOOL smooth)
{
	int i;
	logf_ = log;
	g_smooth = smooth;
	if (!hook_verify(ADDR_DRAW_STRING, SIG_DRAW_STRING, sizeof SIG_DRAW_STRING) ||
		!hook_verify(ADDR_SET_FONT_SIZE, SIG_SET_FONT_SIZE, sizeof SIG_SET_FONT_SIZE) ||
		!hook_verify(ADDR_T2K_SET_TRANSFORM, SIG_T2K_SET_TRANSFORM, sizeof SIG_T2K_SET_TRANSFORM) ||
		!hook_verify(ADDR_FILL_RECT, SIG_FILL_RECT, sizeof SIG_FILL_RECT) ||
		!hook_verify(ADDR_BLIT_ALPHA, SIG_BLIT_ALPHA, sizeof SIG_BLIT_ALPHA) ||
		!hook_verify(ADDR_BLIT_COPY, SIG_BLIT_COPY, sizeof SIG_BLIT_COPY) ||
		*(DWORD *)(VT_WINDOW_PLATFORM + VT_SLOT_LOCK_RECT) != ADDR_WIN_LOCK_RECT ||
		*(DWORD *)(VT_WINDOW_PLATFORM + VT_SLOT_UNLOCK) != ADDR_SHARED_RET) {
		log("hires: signature mismatch; hi-res rendering disabled");
		return FALSE;
	}

	for (i = 0; i < 64; i++)
		g_proxy_vt[i] = (void *)px_unexpected;
	g_proxy_vt[0x14 / 4] = (void *)px_width;
	g_proxy_vt[0x18 / 4] = (void *)px_height;
	g_proxy_vt[0x2C / 4] = (void *)px_lock;
	g_proxy_vt[0x30 / 4] = (void *)px_lock;
	g_proxy_vt[0x34 / 4] = (void *)px_unlock;
	g_proxy.vt = g_proxy_vt;
	g_proxy.vt2 = g_proxy_vt;

	if (!hook_install(ADDR_DRAW_STRING, (void *)hk_draw_string, 6, (void **)&orig_draw_string) ||
		!hook_install(ADDR_T2K_SET_TRANSFORM, (void *)hk_t2k_set_transform, 6,
			(void **)&orig_t2k_set_transform) ||
		!hook_install(ADDR_FILL_RECT, (void *)hk_fill_rect, 7, (void **)&orig_fill_rect) ||
		!hook_install(ADDR_BLIT_ALPHA, (void *)hk_blit_alpha, 5, (void **)&orig_blit_alpha) ||
		!hook_install(ADDR_BLIT_COPY, (void *)hk_blit_copy, 5, (void **)&orig_blit_copy) ||
		!patch_vtable_slot(VT_WINDOW_PLATFORM, VT_SLOT_LOCK_RECT, ADDR_WIN_LOCK_RECT, (void *)hk_win_lock_rect) ||
		!patch_vtable_slot(VT_WINDOW_PLATFORM, VT_SLOT_UNLOCK, ADDR_SHARED_RET, (void *)hk_win_unlock)) {
		log("hires: install failed");
		return FALSE;
	}
	g_installed = TRUE;
	log("hires: installed (text, fills, images native; rest upscaled %s)", smooth ? "bilinear" : "nearest");
	return TRUE;
}

// ---- debug capture ---------------------------------------------------------------------------

static BOOL write_bmp(const WCHAR *path, const DWORD *bits, int w, int h, int pitch)
{
	BITMAPFILEHEADER fh;
	BITMAPINFOHEADER ih;
	DWORD n;
	int y;
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return FALSE;
	memset(&fh, 0, sizeof fh);
	memset(&ih, 0, sizeof ih);
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof fh + sizeof ih;
	fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
	ih.biSize = sizeof ih;
	ih.biWidth = w;
	ih.biHeight = -h;
	ih.biPlanes = 1;
	ih.biBitCount = 32;
	WriteFile(f, &fh, sizeof fh, &n, NULL);
	WriteFile(f, &ih, sizeof ih, &n, NULL);
	for (y = 0; y < h; y++)
		WriteFile(f, bits + (size_t)y * pitch, (DWORD)w * 4, &n, NULL);
	CloseHandle(f);
	return TRUE;
}

BOOL hires_dump(const WCHAR *path, BYTE *win)
{
	if (g_on)
		return write_bmp(path, g_bits, g_pw, g_ph, g_pw);
	if (!win || !*(DWORD **)(win + WIN_BITS))
		return FALSE;
	return write_bmp(path, *(DWORD **)(win + WIN_BITS), *(short *)(win + WIN_BB_W),
		*(short *)(win + WIN_BB_H), *(short *)(win + WIN_PITCH));
}
