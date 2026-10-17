// fm07launch.exe - optional launcher for fm07mod.
//
// Not needed when the mod is installed as dsound.dll in the game folder (fm.exe then loads it
// by itself). The launcher is for running the mod without changing the game folder: it starts
// fm.exe suspended and injects fm07mod.dll from the launcher's own folder.
//
// Game folder: the launcher's folder if it contains fm.exe, else fm07mod.ini [launcher] game_dir.
// If the game folder already has the mod as dsound.dll, fm.exe is simply started.
// Extra command line arguments are passed to fm.exe. Must be built as x86 (the LoadLibraryW
// address of the 32-bit kernel32 is shared with the 32-bit game process).

#include <windows.h>

static void fail(const WCHAR *what)
{
	WCHAR msg[1024];
	wsprintfW(msg, L"%s (error %lu)", what, GetLastError());
	MessageBoxW(NULL, msg, L"fm07launch", MB_ICONERROR);
}

static BOOL exists(const WCHAR *path)
{
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// True if dir\dsound.dll is fm07mod (exports fm07mod_version).
static BOOL proxy_installed(const WCHAR *dir)
{
	WCHAR path[MAX_PATH];
	HMODULE h;
	BOOL ours;
	wsprintfW(path, L"%s\\dsound.dll", dir);
	if (!exists(path))
		return FALSE;
	// maps the image for GetProcAddress without running its DllMain
	h = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
	if (!h)
		return FALSE;
	ours = GetProcAddress(h, "fm07mod_version") != NULL;
	FreeLibrary(h);
	return ours;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR args, int show)
{
	WCHAR dir[MAX_PATH], ini[MAX_PATH], dll[MAX_PATH], game_dir[MAX_PATH], exe[MAX_PATH];
	WCHAR cmd[2048], *slash;
	STARTUPINFOW si = { sizeof si };
	PROCESS_INFORMATION pi;
	void *remote;
	SIZE_T dll_bytes;
	HANDLE th;
	DWORD code = 0;
	BOOL inject;
	(void)inst; (void)prev; (void)show;

	GetModuleFileNameW(NULL, dir, MAX_PATH);
	slash = wcsrchr(dir, L'\\');
	if (slash)
		*slash = 0;
	wsprintfW(ini, L"%s\\fm07mod.ini", dir);
	wsprintfW(dll, L"%s\\fm07mod.dll", dir);

	wsprintfW(exe, L"%s\\fm.exe", dir);
	if (exists(exe)) {
		lstrcpyW(game_dir, dir);
	}
	else {
		GetPrivateProfileStringW(L"launcher", L"game_dir", L"", game_dir, MAX_PATH, ini);
		wsprintfW(exe, L"%s\\fm.exe", game_dir);
		if (!game_dir[0] || !exists(exe)) {
			SetLastError(ERROR_FILE_NOT_FOUND);
			fail(L"fm.exe not found. Put fm07launch.exe in the Football Manager 2007 folder, "
				L"or set [launcher] game_dir in fm07mod.ini");
			return 1;
		}
	}
	// optional: run a different fm.exe (e.g. a Large Address Aware copy) with game_dir as cwd
	{
		WCHAR alt[MAX_PATH];
		GetPrivateProfileStringW(L"launcher", L"exe", L"", alt, MAX_PATH, ini);
		if (alt[0] && exists(alt))
			lstrcpyW(exe, alt);
	}
	wsprintfW(cmd, L"\"%s\" %s", exe, args ? args : L"");

	inject = !proxy_installed(game_dir);
	if (inject && !exists(dll)) {
		SetLastError(ERROR_FILE_NOT_FOUND);
		fail(L"fm07mod.dll not found next to fm07launch.exe");
		return 1;
	}
	if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, inject ? CREATE_SUSPENDED : 0, NULL, game_dir,
			&si, &pi)) {
		fail(L"Could not start fm.exe");
		return 1;
	}
	if (!inject) // dsound.dll proxy in the game folder does the work
		goto done;

	dll_bytes = (lstrlenW(dll) + 1) * sizeof(WCHAR);
	remote = VirtualAllocEx(pi.hProcess, NULL, dll_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remote || !WriteProcessMemory(pi.hProcess, remote, dll, dll_bytes, NULL)) {
		fail(L"Could not write the DLL path into fm.exe");
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	th = CreateRemoteThread(pi.hProcess, NULL, 0,
		(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
		remote, 0, NULL);
	if (!th) {
		fail(L"Could not inject fm07mod.dll");
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	WaitForSingleObject(th, 30000);
	GetExitCodeThread(th, &code); // HMODULE of the loaded DLL, 0 on failure
	CloseHandle(th);
	VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
	if (!code) {
		SetLastError(0);
		fail(L"LoadLibrary of fm07mod.dll inside fm.exe failed");
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	ResumeThread(pi.hThread);
done:
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}
