// Performance fixes for fm.exe 7.0.2 (see README "Performance").
//
// File I/O: WINDOWS_UTILITIES::FILE::read/write (used by FILE_STREAM for every transfer, even
// single bytes) dynamic_casts the record to WINDOWS_FILE_RECORD and calls ftell before and after
// fread/fwrite to compute the byte count - two lseek syscalls, CRT locks and three RTTI casts per
// call. We call the game's own fread/fwrite with element size 1, which returns the byte count.

#include "perf.h"
#include "hook.h"
#include <stdio.h>

#define ADDR_FILE_READ   0x00EEA630 // int FILE::read(record, buf, size), thiscall, ret 0xc
#define ADDR_FILE_WRITE  0x00EEA6A0 // int FILE::write(record, buf, size)
#define ADDR_GAME_FREAD  0x00F4371C // statically linked CRT (the FILE* belongs to it)
#define ADDR_GAME_FWRITE 0x00F441AF
#define VT_WINDOWS_FILE_RECORD 0x013FF998
#define REC_FILE 0x0C // FILE* in WINDOWS_FILE_RECORD

static const BYTE SIG_FILE_READ[] = { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x85, 0xFF, 0x8B, 0xF1, 0x74, 0x24,
	0x6A, 0x00, 0x68, 0x98, 0xBE, 0x60, 0x01 };
static const BYTE SIG_FILE_WRITE_TAIL[] = { 0xE8, 0xB5, 0x9A, 0x05, 0x00 }; // call fwrite at +0x55

typedef int(__fastcall *file_xfer_t)(void *self, void *edx, BYTE *rec, void *buf, int size);
typedef size_t(__cdecl *crt_xfer_t)(void *buf, size_t size, size_t count, FILE *f);

static file_xfer_t orig_file_read, orig_file_write;
static const crt_xfer_t game_fread = (crt_xfer_t)ADDR_GAME_FREAD;
static const crt_xfer_t game_fwrite = (crt_xfer_t)ADDR_GAME_FWRITE;

static FILE *record_file(BYTE *rec)
{
	if (!rec || *(DWORD *)rec != VT_WINDOWS_FILE_RECORD)
		return NULL;
	return *(FILE **)(rec + REC_FILE);
}

static int __fastcall hk_file_read(void *self, void *edx, BYTE *rec, void *buf, int size)
{
	FILE *f = record_file(rec);
	if (!f || size < 0)
		return orig_file_read(self, edx, rec, buf, size);
	return (int)game_fread(buf, 1, (size_t)size, f);
}

static int __fastcall hk_file_write(void *self, void *edx, BYTE *rec, void *buf, int size)
{
	FILE *f = record_file(rec);
	if (!f || size < 0)
		return orig_file_write(self, edx, rec, buf, size);
	return (int)game_fwrite(buf, 1, (size_t)size, f);
}

BOOL perf_install(void (*log)(const char *fmt, ...))
{
	// read and write share the same prologue; the call targets identify them
	if (!hook_verify(ADDR_FILE_READ, SIG_FILE_READ, sizeof SIG_FILE_READ) ||
		!hook_verify(ADDR_FILE_WRITE, SIG_FILE_READ, sizeof SIG_FILE_READ) ||
		!hook_verify(ADDR_FILE_WRITE + 0x55, SIG_FILE_WRITE_TAIL, sizeof SIG_FILE_WRITE_TAIL)) {
		log("perf: file I/O signature mismatch; not installed");
		return FALSE;
	}
	if (!hook_install(ADDR_FILE_READ, (void *)hk_file_read, 7, (void **)&orig_file_read) ||
		!hook_install(ADDR_FILE_WRITE, (void *)hk_file_write, 7, (void **)&orig_file_write)) {
		log("perf: file I/O install failed");
		return FALSE;
	}
	log("perf: direct file read/write installed");
	return TRUE;
}
