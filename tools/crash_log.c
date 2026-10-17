#include "crash_log.h"

#ifdef _WIN32
#include <windows.h>
#include <signal.h>
#include <stdio.h>

static void write_crash(const char *reason, unsigned long code, void *address) {
	FILE *file = fopen("kmat_crash.txt", "a");
	if(file == NULL) return;
	char *base = (char *)GetModuleHandle(NULL);
	fprintf(file, "%s code=0x%lx thread=%lu address_offset=0x%llx\n", reason, code, GetCurrentThreadId(),
		address != NULL ? (unsigned long long)((char *)address - base) : 0ULL);
	void *frames[32];
	USHORT count = CaptureStackBackTrace(0, 32, frames, NULL);
	for(USHORT i = 0; i < count; i++) fprintf(file, "  frame %u offset=0x%llx\n", i, (unsigned long long)((char *)frames[i] - base));
	fclose(file);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
	write_crash("exception", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
	return EXCEPTION_CONTINUE_SEARCH;
}

static void abort_handler(int signal_number) {
	write_crash("abort", (unsigned long)signal_number, NULL);
}

void install_crash_log(void) {
	SetUnhandledExceptionFilter(crash_filter);
	signal(SIGABRT, abort_handler);
}
#else
void install_crash_log(void) {}
#endif
