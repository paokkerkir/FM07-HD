// Performance fixes (file I/O fast path). See perf.c.
#pragma once
#include <windows.h>

BOOL perf_install(void (*log)(const char *fmt, ...));
