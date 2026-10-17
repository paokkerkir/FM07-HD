// High-resolution rendering: a physical-size shadow of the game's logical back buffer.
// Drawing operations we understand (text) are re-rendered natively at physical size;
// everything else is upscaled from the logical buffer when its lock_rect is released.
#pragma once
#include <windows.h>

// Install vtable patches and text hooks. Returns FALSE if any signature check fails.
BOOL hires_install(void (*log)(const char *fmt, ...), BOOL smooth);

// Called after the game (re)created its logical back buffer. win = SI_WINDOW_PLATFORM.
void hires_on_back_buffer(BYTE *win, int pw, int ph, int lw, int lh);

// True when the shadow buffer is in use (zoomed and enabled).
BOOL hires_active(void);

// Copy the window's dirty area (or everything) from the shadow buffer to hdc.
void hires_present(HDC hdc, BYTE *win, BOOL full);

// Debug: write the frame the game is showing (shadow buffer, or the logical buffer when
// unzoomed) to a 32bpp BMP. Captures only the game, never other windows.
BOOL hires_dump(const WCHAR *path, BYTE *win);
