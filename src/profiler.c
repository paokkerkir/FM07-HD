// fm07prof.exe - external sampling profiler for fm.exe (x86 build, attaches to a running game).
//
// Usage: fm07prof <out.bin> [seconds=0 (until exit)] [interval_ms=1]
// Every interval each fm.exe thread is suspended briefly; we record its EIP and the stack words
// that point into fm.exe's .text (return-address candidates, filtered offline). Nothing is
// written into the game process.
//
// Record format (little endian): u32 t_ms, u32 tid, u32 eip, u8 active, u8 n, u32 cand[n]
// active = thread used CPU time since the previous round (else it is waiting)

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#define TEXT_LO 0x00401000u
#define TEXT_HI 0x0100D000u
#define STACK_SCAN_BYTES 16384
#define MAX_CAND 64
#define IDLE_EVERY 25 // idle threads (no CPU time since last round) are sampled every Nth round
#define MAX_THREADS 128

static DWORD find_pid(void)
{
	PROCESSENTRY32W pe = { sizeof pe };
	HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	DWORD pid = 0;
	if (Process32FirstW(s, &pe)) {
		do {
			if (!lstrcmpiW(pe.szExeFile, L"fm.exe")) {
				pid = pe.th32ProcessID;
				break;
			}
		} while (Process32NextW(s, &pe));
	}
	CloseHandle(s);
	return pid;
}

typedef struct {
	DWORD tid;
	HANDLE h;
	ULONGLONG cpu; // kernel+user 100ns units at the previous round
} THREAD;

static THREAD g_threads[MAX_THREADS];
static int g_nthreads;

static void refresh_threads(DWORD pid)
{
	THREADENTRY32 te = { sizeof te };
	HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	int i;
	for (i = 0; i < g_nthreads; i++)
		CloseHandle(g_threads[i].h);
	g_nthreads = 0;
	if (Thread32First(s, &te)) {
		do {
			if (te.th32OwnerProcessID == pid && g_nthreads < MAX_THREADS) {
				HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
					FALSE, te.th32ThreadID);
				if (h) {
					g_threads[g_nthreads].tid = te.th32ThreadID;
					g_threads[g_nthreads].h = h;
					g_threads[g_nthreads].cpu = 0;
					g_nthreads++;
				}
			}
		} while (Thread32Next(s, &te));
	}
	CloseHandle(s);
}

int main(int argc, char **argv)
{
	FILE *out;
	DWORD pid, start, last_refresh = 0, seconds = 0, interval = 1, samples = 0, round = 0;
	HANDLE proc;
	static DWORD stack[STACK_SCAN_BYTES / 4];

	if (argc < 2) {
		fprintf(stderr, "usage: fm07prof <out.bin> [seconds] [interval_ms]\n");
		return 2;
	}
	if (argc > 2)
		seconds = (DWORD)atoi(argv[2]);
	if (argc > 3)
		interval = (DWORD)atoi(argv[3]);
	pid = find_pid();
	if (!pid) {
		fprintf(stderr, "fm.exe not running\n");
		return 1;
	}
	proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
	out = fopen(argv[1], "wb");
	if (!proc || !out) {
		fprintf(stderr, "open failed (%lu)\n", GetLastError());
		return 1;
	}
	timeBeginPeriod(1);
	start = GetTickCount();
	printf("profiling pid %lu -> %s\n", pid, argv[1]);
	fflush(stdout);

	while (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) {
		DWORD now = GetTickCount(), t = now - start;
		int i;
		if (seconds && t >= seconds * 1000)
			break;
		if (now - last_refresh > 1000) {
			refresh_threads(pid);
			last_refresh = now;
		}
		round++;
		for (i = 0; i < g_nthreads; i++) {
			CONTEXT ctx;
			FILETIME c, e, k, u;
			ULONGLONG cpu;
			BYTE active = 1;
			if (GetThreadTimes(g_threads[i].h, &c, &e, &k, &u)) {
				cpu = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
					((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
				active = cpu != g_threads[i].cpu;
				if (!active && round % IDLE_EVERY)
					continue;
				g_threads[i].cpu = cpu;
			}
			SIZE_T got = 0;
			BYTE n = 0;
			DWORD cand[MAX_CAND], j;
			if (SuspendThread(g_threads[i].h) == (DWORD)-1)
				continue;
			ctx.ContextFlags = CONTEXT_CONTROL;
			if (!GetThreadContext(g_threads[i].h, &ctx)) {
				ResumeThread(g_threads[i].h);
				continue;
			}
			ReadProcessMemory(proc, (void *)ctx.Esp, stack, sizeof stack, &got);
			ResumeThread(g_threads[i].h);
			for (j = 0; j < got / 4 && n < MAX_CAND; j++)
				if (stack[j] >= TEXT_LO && stack[j] < TEXT_HI)
					cand[n++] = stack[j];
			fwrite(&t, 4, 1, out);
			fwrite(&g_threads[i].tid, 4, 1, out);
			fwrite(&ctx.Eip, 4, 1, out);
			fwrite(&active, 1, 1, out);
			fwrite(&n, 1, 1, out);
			fwrite(cand, 4, n, out);
			samples++;
		}
		Sleep(interval);
	}
	fclose(out);
	timeEndPeriod(1);
	printf("done: %lu samples, %lu s\n", samples, (GetTickCount() - start) / 1000);
	return 0;
}
