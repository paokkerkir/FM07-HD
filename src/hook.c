#include "hook.h"
#include <string.h>

BOOL hook_verify(DWORD addr, const BYTE *sig, size_t len)
{
	return memcmp((const void *)addr, sig, len) == 0;
}

static void write_jmp(BYTE *at, const void *to)
{
	at[0] = 0xE9;
	*(DWORD *)(at + 1) = (DWORD)((const BYTE *)to - (at + 5));
}

BOOL hook_install(DWORD addr, void *to, size_t prologue_len, void **trampoline)
{
	BYTE *target = (BYTE *)addr;
	DWORD old;

	if (prologue_len < 5)
		return FALSE;

	if (trampoline) {
		BYTE *t = (BYTE *)VirtualAlloc(NULL, prologue_len + 5, MEM_COMMIT | MEM_RESERVE,
			PAGE_EXECUTE_READWRITE);
		if (!t)
			return FALSE;
		memcpy(t, target, prologue_len);
		write_jmp(t + prologue_len, target + prologue_len);
		*trampoline = t;
	}

	if (!VirtualProtect(target, prologue_len, PAGE_EXECUTE_READWRITE, &old))
		return FALSE;
	write_jmp(target, to);
	// pad the rest of the overwritten prologue so disassembly stays sane
	memset(target + 5, 0xCC, prologue_len - 5);
	VirtualProtect(target, prologue_len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), target, prologue_len);
	return TRUE;
}
