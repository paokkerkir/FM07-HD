// dsound.dll proxy exports. fm.exe imports DSOUND.dll ordinal 1 (DirectSoundCreate) only, so a
// copy of fm07mod.dll named dsound.dll in the game folder is loaded automatically at startup.
// Every export forwards to the real dsound.dll in the system directory, loaded on first use
// (never from DllMain). See fm07mod.def for the ordinals.

#include <windows.h>

static HMODULE g_real;

static FARPROC __stdcall resolve(const char *name)
{
	FARPROC p;
	if (!g_real) {
		WCHAR path[MAX_PATH];
		// for a 32-bit process WOW64 redirects System32 to SysWOW64
		GetSystemDirectoryW(path, MAX_PATH);
		lstrcatW(path, L"\\dsound.dll");
		g_real = LoadLibraryW(path);
	}
	p = g_real ? GetProcAddress(g_real, name) : NULL;
	if (!p) {
		MessageBoxA(NULL, name, "fm07mod: system dsound.dll function missing", MB_ICONERROR);
		ExitProcess(1);
	}
	return p;
}

// Each stub resolves the real function and jumps to it with the caller's stack untouched.
#define FORWARD(fn)                                     \
	static const char name_##fn[] = #fn;                \
	__declspec(naked) void fwd_##fn(void)               \
	{                                                   \
		__asm push offset name_##fn                     \
		__asm call resolve                              \
		__asm jmp eax                                   \
	}

FORWARD(DirectSoundCreate)
FORWARD(DirectSoundEnumerateA)
FORWARD(DirectSoundEnumerateW)
FORWARD(DllCanUnloadNow)
FORWARD(DllGetClassObject)
FORWARD(DirectSoundCaptureCreate)
FORWARD(DirectSoundCaptureEnumerateA)
FORWARD(DirectSoundCaptureEnumerateW)
FORWARD(GetDeviceID)
FORWARD(DirectSoundFullDuplexCreate)
FORWARD(DirectSoundCreate8)
FORWARD(DirectSoundCaptureCreate8)
