// Minimal x86 inline hooking for fixed-address targets in fm.exe 7.0.2.
#pragma once
#include <windows.h>

// True if the bytes at addr equal sig[0..len).
BOOL hook_verify(DWORD addr, const BYTE *sig, size_t len);

// Overwrite addr with "jmp to". If trampoline is non-NULL, first copy the
// prologue_len bytes at addr into executable memory followed by a jmp back to
// addr+prologue_len, and store it there. The copied prologue must not contain
// relative branches. prologue_len must be >= 5.
BOOL hook_install(DWORD addr, void *to, size_t prologue_len, void **trampoline);
