/*
Cuckoo Sandbox - Automated Malware Analysis
Copyright (C) 2010-2014 Cuckoo Sandbox Developers

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>

#include "ntapi.h"
#include "misc.h"
#include "hooking.h"
#include "hooks.h"
#include "log.h"
#include "pipe.h"
#include "ignore.h"
#include "hook_file.h"
#include "hook_sleep.h"
#include "config.h"
#include "unhook.h"
#include "bson.h"

// Allow debug mode to be turned on at compilation time.
#ifdef CUCKOODBG
#undef CUCKOODBG
#define CUCKOODBG 1
#else
#define CUCKOODBG 0
#endif

#define HOOK(library, funcname) {L###library, #funcname, NULL, \
    &New_##funcname, (void **) &Old_##funcname}

#define HOOK2(library, funcname, recursion) {L###library, #funcname, NULL, \
    &New2_##funcname, (void **) &Old2_##funcname, recursion}

static hook_t g_hooks[] = {

    //
    // Special Hooks
    //
    // NOTE: due to the fact that the "special" hooks don't use a hook count
    // (whereas the "normal" hooks, those with allow_hook_recursion set to
    // zero, do) we have to hook the "special" hooks first. Otherwise the
    // execution flow will end up in an infinite loop, because of hook count
    // and whatnot.
    //
    // In other words, do *NOT* place "special" hooks behind "normal" hooks.
    //

	HOOK2(ntdll, LdrLoadDll, TRUE),
    HOOK2(kernel32, CreateProcessInternalW, TRUE),

	// COM object creation hook
	HOOK2(ole32, CoCreateInstance, TRUE),
	//
    // File Hooks
    //
	HOOK(ntdll, NtQueryAttributesFile),
	HOOK(ntdll, NtQueryFullAttributesFile),
	HOOK(ntdll, NtCreateFile),
    HOOK(ntdll, NtOpenFile),
    HOOK(ntdll, NtReadFile),
    HOOK(ntdll, NtWriteFile),
    HOOK(ntdll, NtDeleteFile),
    HOOK(ntdll, NtDeviceIoControlFile),
    HOOK(ntdll, NtQueryDirectoryFile),
    HOOK(ntdll, NtQueryInformationFile),
    HOOK(ntdll, NtSetInformationFile),
    HOOK(ntdll, NtOpenDirectoryObject),
    HOOK(ntdll, NtCreateDirectoryObject),

    // CreateDirectoryExA calls CreateDirectoryExW
    // CreateDirectoryW does not call CreateDirectoryExW
    HOOK(kernel32, CreateDirectoryW),
    HOOK(kernel32, CreateDirectoryExW),

    HOOK(kernel32, RemoveDirectoryA),
    HOOK(kernel32, RemoveDirectoryW),

    // lowest variant of MoveFile()
    HOOK(kernel32, MoveFileWithProgressW),

    HOOK(kernel32, FindFirstFileExA),
    HOOK(kernel32, FindFirstFileExW),

    // Covered by NtCreateFile() but still grap this information
    HOOK(kernel32, CopyFileA),
    HOOK(kernel32, CopyFileW),
    HOOK(kernel32, CopyFileExW),

    // Covered by NtSetInformationFile() but still grab this information
    HOOK(kernel32, DeleteFileA),
    HOOK(kernel32, DeleteFileW),

    HOOK(kernel32, GetDiskFreeSpaceExA),
    HOOK(kernel32, GetDiskFreeSpaceExW),
    HOOK(kernel32, GetDiskFreeSpaceA),
    HOOK(kernel32, GetDiskFreeSpaceW),

	HOOK(kernel32, GetVolumeNameForVolumeMountPointW),

	HOOK(shell32, SHGetFolderPathW),

	HOOK(version, GetFileVersionInfoW),
	HOOK(version, GetFileVersionInfoSizeW),

    //
    // Registry Hooks
    //
    // Note: Most, if not all, of the Registry API go natively from both the
    // A as well as the W versions. In other words, we have to hook all the
    // ascii *and* unicode APIs of those functions.
    //

    HOOK(advapi32, RegOpenKeyExA),
    HOOK(advapi32, RegOpenKeyExW),

    HOOK(advapi32, RegCreateKeyExA),
    HOOK(advapi32, RegCreateKeyExW),

    // Note that RegDeleteKeyEx() is available for 64bit XP/Vista+
    HOOK(advapi32, RegDeleteKeyA),
    HOOK(advapi32, RegDeleteKeyW),

    // RegEnumKeyA() calls RegEnumKeyExA(), but RegEnumKeyW() does *not*
    // call RegEnumKeyExW()
    HOOK(advapi32, RegEnumKeyW),
    HOOK(advapi32, RegEnumKeyExA),
    HOOK(advapi32, RegEnumKeyExW),

    HOOK(advapi32, RegEnumValueA),
    HOOK(advapi32, RegEnumValueW),

    HOOK(advapi32, RegSetValueExA),
    HOOK(advapi32, RegSetValueExW),

    HOOK(advapi32, RegQueryValueExA),
    HOOK(advapi32, RegQueryValueExW),

    HOOK(advapi32, RegDeleteValueA),
    HOOK(advapi32, RegDeleteValueW),

    HOOK(advapi32, RegQueryInfoKeyA),
    HOOK(advapi32, RegQueryInfoKeyW),

    HOOK(advapi32, RegCloseKey),

    //
    // Native Registry Hooks
    //

	HOOK(ntdll, NtCreateKey),
    HOOK(ntdll, NtOpenKey),
    HOOK(ntdll, NtOpenKeyEx),
	HOOK(ntdll, NtRenameKey),
    HOOK(ntdll, NtReplaceKey),
    HOOK(ntdll, NtEnumerateKey),
    HOOK(ntdll, NtEnumerateValueKey),
	HOOK(ntdll, NtSetValueKey),
	HOOK(ntdll, NtQueryValueKey),
    HOOK(ntdll, NtQueryMultipleValueKey),
    HOOK(ntdll, NtDeleteKey),
	HOOK(ntdll, NtDeleteValueKey),
	HOOK(ntdll, NtLoadKey),
    HOOK(ntdll, NtLoadKey2),
    HOOK(ntdll, NtLoadKeyEx),
	HOOK(ntdll, NtQueryKey),
    HOOK(ntdll, NtSaveKey),
    HOOK(ntdll, NtSaveKeyEx),

    //
    // Window Hooks
    //

	// can't use these until we come up with a fool-proof method of logging them,
	// as they might not return as in the upatre downloader

	//HOOK(user32, CreateWindowExA, TRUE),
	//HOOK(user32, CreateWindowExW, TRUE),
    HOOK(user32, FindWindowA),
    HOOK(user32, FindWindowW),
    HOOK(user32, FindWindowExA),
    HOOK(user32, FindWindowExW),
    HOOK(user32, EnumWindows),
	HOOK(user32, SendNotifyMessageA),
	HOOK(user32, SendNotifyMessageW),

    //
    // Sync Hooks
    //

    HOOK(ntdll, NtCreateMutant),
    HOOK(ntdll, NtOpenMutant),
	HOOK(ntdll, NtCreateEvent),
	HOOK(ntdll, NtOpenEvent),
	HOOK(ntdll, NtCreateNamedPipeFile),
    //
    // Process Hooks
    //

	HOOK(kernel32, CreateToolhelp32Snapshot),
	HOOK(kernel32, Process32FirstW),
	HOOK(kernel32, Process32NextW),
	HOOK(ntdll, NtCreateProcess),
    HOOK(ntdll, NtCreateProcessEx),
    HOOK(ntdll, NtCreateUserProcess),
    HOOK(ntdll, RtlCreateUserProcess),
    HOOK(ntdll, NtOpenProcess),
    HOOK(ntdll, NtTerminateProcess),
	HOOK(ntdll, NtResumeProcess),
	HOOK(ntdll, NtCreateSection),
	HOOK(ntdll, NtDuplicateObject),
    HOOK(ntdll, NtMakeTemporaryObject),
    HOOK(ntdll, NtMakePermanentObject),
    HOOK(ntdll, NtOpenSection),
    HOOK(ntdll, NtMapViewOfSection),
	HOOK(kernel32, WaitForDebugEvent),
	HOOK(ntdll, DbgUiWaitStateChange),

    // all variants of ShellExecute end up in ShellExecuteExW
    HOOK(shell32, ShellExecuteExW),
    HOOK(ntdll, NtUnmapViewOfSection),
    HOOK(ntdll, NtAllocateVirtualMemory),
    HOOK(ntdll, NtReadVirtualMemory),
    HOOK(kernel32, ReadProcessMemory),
    HOOK(ntdll, NtWriteVirtualMemory),
    HOOK(kernel32, WriteProcessMemory),
    HOOK(ntdll, NtProtectVirtualMemory),
    HOOK(kernel32, VirtualProtectEx),
    HOOK(ntdll, NtFreeVirtualMemory),
    //HOOK(kernel32, VirtualFreeEx),
	
	HOOK(msvcrt, system),
    //
    // Thread Hooks
    //
	HOOK(ntdll, NtQueueApcThread),
    HOOK(ntdll, NtCreateThread),
    HOOK(ntdll, NtCreateThreadEx),
    HOOK(ntdll, NtOpenThread),
    HOOK(ntdll, NtGetContextThread),
    HOOK(ntdll, NtSetContextThread),
    HOOK(ntdll, NtSuspendThread),
    HOOK(ntdll, NtResumeThread),
    HOOK(ntdll, NtTerminateThread),
    HOOK(kernel32, CreateThread),
    HOOK(kernel32, CreateRemoteThread),
    HOOK(ntdll, RtlCreateUserThread),

	//
    // Misc Hooks
    //

	// for debugging only
	//HOOK(kernel32, GetLastError),

	HOOK(user32, SetWindowsHookExA),
    HOOK(user32, SetWindowsHookExW),
    HOOK(user32, UnhookWindowsHookEx),
    HOOK(kernel32, SetUnhandledExceptionFilter),
	HOOK(kernel32, SetErrorMode),
    HOOK(ntdll, LdrGetDllHandle),
    HOOK(ntdll, LdrGetProcedureAddress),
    HOOK(kernel32, DeviceIoControl),
    HOOK(user32, ExitWindowsEx),
    HOOK(kernel32, IsDebuggerPresent),
    HOOK(advapi32, LookupPrivilegeValueW),
    HOOK(ntdll, NtClose),
    HOOK(kernel32, WriteConsoleA),
    HOOK(kernel32, WriteConsoleW),
    HOOK(user32, GetSystemMetrics),
    HOOK(user32, GetCursorPos),
    HOOK(kernel32, GetComputerNameA),
    HOOK(kernel32, GetComputerNameW),
    HOOK(advapi32, GetUserNameA),
    HOOK(advapi32, GetUserNameW),
	HOOK(user32, GetAsyncKeyState),
	HOOK(ntdll, NtLoadDriver),
	
	//
    // Network Hooks
    //
	HOOK(netapi32, NetUserGetInfo),
    HOOK(urlmon, URLDownloadToFileW),
	HOOK(urlmon, ObtainUserAgentString),
	HOOK(wininet, InternetGetConnectedState),
    HOOK(wininet, InternetOpenA),
    HOOK(wininet, InternetOpenW),
    HOOK(wininet, InternetConnectA),
    HOOK(wininet, InternetConnectW),
    HOOK(wininet, InternetOpenUrlA),
    HOOK(wininet, InternetOpenUrlW),
    HOOK(wininet, HttpOpenRequestA),
    HOOK(wininet, HttpOpenRequestW),
    HOOK(wininet, HttpSendRequestA),
    HOOK(wininet, HttpSendRequestW),
    HOOK(wininet, InternetReadFile),
    HOOK(wininet, InternetWriteFile),
    HOOK(wininet, InternetCloseHandle),
	HOOK(wininet, InternetCrackUrlA),
	HOOK(wininet, InternetCrackUrlW),
	HOOK(wininet, InternetSetOptionA),

	HOOK(winhttp, WinHttpOpen),
	HOOK(winhttp, WinHttpGetIEProxyConfigForCurrentUser),
	HOOK(winhttp, WinHttpGetProxyForUrl),
	HOOK(winhttp, WinHttpSetOption),
	HOOK(winhttp, WinHttpConnect),
	HOOK(winhttp, WinHttpOpenRequest),
	HOOK(winhttp, WinHttpSetTimeouts),
	HOOK(winhttp, WinHttpQueryHeaders),
	HOOK(winhttp, WinHttpSendRequest),
	HOOK(winhttp, WinHttpReceiveResponse),

    HOOK(dnsapi, DnsQuery_A),
    HOOK(dnsapi, DnsQuery_UTF8),
    HOOK(dnsapi, DnsQuery_W),
    HOOK(ws2_32, getaddrinfo),
    HOOK(ws2_32, GetAddrInfoW),

    //
    // Service Hooks
    //

    HOOK(advapi32, OpenSCManagerA),
    HOOK(advapi32, OpenSCManagerW),
    HOOK(advapi32, CreateServiceA),
    HOOK(advapi32, CreateServiceW),
    HOOK(advapi32, OpenServiceA),
    HOOK(advapi32, OpenServiceW),
    HOOK(advapi32, StartServiceA),
    HOOK(advapi32, StartServiceW),
    HOOK(advapi32, ControlService),
    HOOK(advapi32, DeleteService),

    //
    // Sleep Hooks
    //
    HOOK(ntdll, NtDelayExecution),
    HOOK(kernel32, GetLocalTime),
    HOOK(kernel32, GetSystemTime),
	HOOK(kernel32, GetSystemTimeAsFileTime),
	HOOK(kernel32, GetTickCount),
    HOOK(ntdll, NtQuerySystemTime),
	HOOK(user32, GetLastInputInfo),
	HOOK(winmm, timeGetTime),

	//
    // Socket Hooks
    //
    HOOK(ws2_32, WSAStartup),
    HOOK(ws2_32, gethostbyname),
    HOOK(ws2_32, socket),
    HOOK(ws2_32, connect),
    HOOK(ws2_32, send),
    HOOK(ws2_32, sendto),
    HOOK(ws2_32, recv),
    HOOK(ws2_32, recvfrom),
    HOOK(ws2_32, accept),
    HOOK(ws2_32, bind),
    HOOK(ws2_32, listen),
    HOOK(ws2_32, select),
    HOOK(ws2_32, setsockopt),
    HOOK(ws2_32, ioctlsocket),
    HOOK(ws2_32, closesocket),
    HOOK(ws2_32, shutdown),

    HOOK(ws2_32, WSAAccept),
	HOOK(ws2_32, WSAConnect),
	HOOK(ws2_32, WSARecv),
    HOOK(ws2_32, WSARecvFrom),
    HOOK(ws2_32, WSASend),
    HOOK(ws2_32, WSASendTo),
    HOOK(ws2_32, WSASocketA),
    HOOK(ws2_32, WSASocketW),

    // HOOK(wsock32, connect),
    // HOOK(wsock32, send),
    // HOOK(wsock32, recv),

    HOOK(mswsock, ConnectEx),
    HOOK(mswsock, TransmitFile),
    //
    // Crypto Functions
    //

	HOOK(advapi32, CryptAcquireContextA),
	HOOK(advapi32, CryptAcquireContextW),
    HOOK(advapi32, CryptProtectData),
    HOOK(advapi32, CryptUnprotectData),
    HOOK(advapi32, CryptProtectMemory),
    HOOK(advapi32, CryptUnprotectMemory),
    HOOK(advapi32, CryptDecrypt),
    HOOK(advapi32, CryptEncrypt),
    HOOK(advapi32, CryptHashData),
    HOOK(advapi32, CryptDecodeMessage),
    HOOK(advapi32, CryptDecryptMessage),
    HOOK(advapi32, CryptEncryptMessage),
    HOOK(advapi32, CryptHashMessage),
	HOOK(advapi32, CryptExportKey),
	HOOK(advapi32, CryptGenKey),
	HOOK(advapi32, CryptCreateHash),
};

// get a random hooking method, except for hook_jmp_direct
//#define HOOKTYPE randint(HOOK_NOP_JMP_DIRECT, HOOK_MOV_EAX_INDIRECT_PUSH_RETN)
// error testing with hook_jmp_direct only
#ifdef _WIN64
#define HOOKTYPE HOOK_JMP_INDIRECT
#else
#define HOOKTYPE HOOK_HOTPATCH_JMP_INDIRECT
#endif

void set_hooks_dll(const wchar_t *library)
{
    for (int i = 0; i < ARRAYSIZE(g_hooks); i++) {
        if(!wcsicmp(g_hooks[i].library, library)) {
			if (hook_api(&g_hooks[i], HOOKTYPE) < 0)
				pipe("WARNING:Unable to hook %z", g_hooks[i].funcname);
        }
    }
}

void set_hooks()
{
    // the hooks contain executable code as well, so they have to be RWX
    DWORD old_protect;
    VirtualProtect(g_hooks, sizeof(g_hooks), PAGE_EXECUTE_READWRITE,
        &old_protect);

	// before modifying any DLLs, let's first freeze all other threads in our process
	// otherwise our racy modifications can cause the task to crash prematurely
	// This code itself is racy as additional threads could be created while we're
	// processing the list, but the risk is at least greatly reduced
	PHANDLE suspended_threads = (PHANDLE)calloc(4096, sizeof(HANDLE));
	DWORD num_suspended_threads = 0;
	DWORD i;
	HANDLE hSnapShot;
	THREADENTRY32 threadInfo;
	DWORD our_tid = GetCurrentThreadId();
	DWORD our_pid = GetCurrentProcessId();
	memset(&threadInfo, 0, sizeof(threadInfo));
	threadInfo.dwSize = sizeof(threadInfo);

	hook_disable();

	hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	Thread32First(hSnapShot, &threadInfo);
	do {
		if (threadInfo.th32OwnerProcessID != our_pid || threadInfo.th32ThreadID == our_tid || num_suspended_threads >= 4096)
			continue;
		suspended_threads[num_suspended_threads] = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadInfo.th32ThreadID);
		if (suspended_threads[num_suspended_threads]) {
			SuspendThread(suspended_threads[num_suspended_threads]);
			num_suspended_threads++;
		}
	} while (Thread32Next(hSnapShot, &threadInfo));

    // now, hook each api :)
    for (int i = 0; i < ARRAYSIZE(g_hooks); i++) {
		//pipe("INFO:Hooking %z", g_hooks[i].funcname);
		if (hook_api(&g_hooks[i], HOOKTYPE) < 0)
			pipe("WARNING:Unable to hook %z", g_hooks[i].funcname);
    }

	for (i = 0; i < num_suspended_threads; i++) {
		ResumeThread(suspended_threads[i]);
		CloseHandle(suspended_threads[i]);
	}

	free(suspended_threads);

	hook_enable();
}

#if REPORT_EXCEPTIONS
LONG WINAPI cuckoomon_exception_handler(
	__in struct _EXCEPTION_POINTERS *ExceptionInfo
	) {
	char msg[1024];
	char *dllname;
	unsigned int offset;
	ULONG_PTR eip = (ULONG_PTR)ExceptionInfo->ExceptionRecord->ExceptionAddress;
	PUCHAR eipptr = (PUCHAR)eip;
#ifdef _WIN64
	ULONG_PTR *stack = (DWORD *)(ULONG_PTR)(ExceptionInfo->ContextRecord->Rsp);
#else
	DWORD *stack = (DWORD *)(ULONG_PTR)(ExceptionInfo->ContextRecord->Esp);
#endif
	lasterror_t lasterror;

#if REPORT_ALL_EXCEPTIONS == 0
	if (ExceptionInfo->ExceptionRecord->ExceptionCode < 0xc0000000)
		return EXCEPTION_CONTINUE_SEARCH;
#endif

	hook_disable();

	get_lasterrors(&lasterror);

	log_flush();

	dllname = convert_address_to_dll_name_and_offset(eip, &offset);

	sprintf(msg, "Exception Caught! PID: %u EIP:", GetCurrentProcessId());
	if (dllname)
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), " %s+%x", dllname, offset);
	snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), " %.08x, Fault Address: %.08x, Esp: %.08x, Exception Code: %08x, ",
		eip, ExceptionInfo->ExceptionRecord->ExceptionInformation[1], (ULONG_PTR)stack, ExceptionInfo->ExceptionRecord->ExceptionCode);
	if (is_valid_address_range((ULONG_PTR)stack, 10 * sizeof(ULONG_PTR))) 
	{
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "Stack Dump: %.08x %.08x %.08x %.08x %.08x %.08x %.08x %.08x %.08x %.08x, ",
			stack[0], stack[1], stack[2], stack[3], stack[4], stack[5], stack[6], stack[7], stack[8], stack[9]);
	}
	if (is_valid_address_range(eip, 16)) {
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "Bytes at EIP: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			eipptr[0], eipptr[1], eipptr[2], eipptr[3], eipptr[4], eipptr[5], eipptr[6], eipptr[7], eipptr[8], eipptr[9], eipptr[10], eipptr[11], eipptr[12], eipptr[13], eipptr[14], eipptr[15]);
	}
	debug_message(msg);
	if (dllname)
		free(dllname);

	set_lasterrors(&lasterror);

	hook_enable();

	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void notify_successful_load(void)
{
	// notify analyzer.py that we've loaded
	pipe("LOADED:%d", GetCurrentProcessId());
}

struct _g_config g_config;
wchar_t *our_process_path;
BOOL is_64bit_os;

void get_our_process_path(void)
{
	wchar_t *tmp = calloc(1, 32768 * sizeof(wchar_t));
	wchar_t *tmp2 = calloc(1, 32768 * sizeof(wchar_t));

	GetModuleFileNameW(NULL, tmp, 32768);

	ensure_absolute_unicode_path(tmp2, tmp);

	our_process_path = tmp2;

	free(tmp);
}

void set_os_bitness(void)
{
	LPFN_ISWOW64PROCESS pIsWow64Process;

	is_64bit_os = FALSE;

	pIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(GetModuleHandleA("kernel32"), "IsWow64Process");

	if (pIsWow64Process)
		pIsWow64Process(GetCurrentProcess(), &is_64bit_os);
}

HANDLE g_heap;

static void *malloc_func(size_t size)
{
	return malloc(size);
}

static void *realloc_func(void *ptr, size_t size)
{
	return realloc(ptr, size);
}

static void free_func(void *ptr)
{
	free(ptr);
}

void init_private_heap(void)
{
	bson_set_malloc_func(malloc_func);
	bson_set_realloc_func(realloc_func);
	bson_set_free_func(free_func);
#ifdef USE_PRIVATE_HEAP
	g_heap = HeapCreate(0, 4 * 1024 * 1024, 0);
#endif
}

BOOLEAN g_dll_main_complete;

DWORD g_tls_hook_index;

BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved)
{
	lasterror_t lasterror;

	get_lasterrors(&lasterror);

    if(dwReason == DLL_PROCESS_ATTACH) {
		unsigned int i;
		DWORD pids[MAX_PROTECTED_PIDS];
		unsigned int length = sizeof(pids);

		/* we can sometimes be injected twice into a process, say if we queued up an APC that we timed out waiting to
		   complete, and then did a successful createremotethread, so just do a cheap check for our hooks and fake that
		   we loaded successfully
		*/
#ifdef _WIN64
#if HOOKTYPE != HOOK_JMP_INDIRECT
#error Update hook check
#endif
		if (((PUCHAR)ReadProcessMemory)[0] == 0xff && ((PUCHAR)ReadProcessMemory)[1] == 0x25) {
			notify_successful_load();
			goto out;
		}
#else
#if HOOKTYPE != HOOK_HOTPATCH_JMP_INDIRECT
#error Update hook check
#endif
		if (((PUCHAR)ReadProcessMemory)[0] == 0x8b && ((PUCHAR)ReadProcessMemory)[1] == 0xff && ((PUCHAR)ReadProcessMemory)[2] == 0xff && ((PUCHAR)ReadProcessMemory)[3] == 0x25) {
			notify_successful_load();
			goto out;
		}
#endif

		g_our_dll_base = (ULONG_PTR)hModule;
		g_our_dll_size = get_image_size(g_our_dll_base);
		
		resolve_runtime_apis();

		init_private_heap();

		init_capstone();

		set_os_bitness();

		get_our_process_path();

		g_tls_hook_index = TlsAlloc();
		if (g_tls_hook_index == TLS_OUT_OF_INDEXES)
			goto out;

		// there's a small list of processes which we don't want to inject
		if (is_ignored_process())
			goto out;
#if REPORT_EXCEPTIONS
		AddVectoredExceptionHandler(1, cuckoomon_exception_handler);
		SetUnhandledExceptionFilter(cuckoomon_exception_handler);
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOALIGNMENTFAULTEXCEPT | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

		add_all_dlls_to_dll_ranges();

#if !CUCKOODBG
		// hide our module from peb
        hide_module_from_peb(hModule);
#endif

        // initialize file stuff
        file_init();

        // read the config settings
		if (!read_config())
#if CUCKOODBG
			;
#else
			// if we're not debugging, then failure to read the cuckoomon config should be a critical error
			goto out;
#endif
        g_pipe_name = g_config.pipe_name;

		// obtain all protected pids
        pipe2(pids, &length, "GETPIDS");
        for (i = 0; i < length / sizeof(pids[0]); i++) {
            add_protected_pid(pids[i]);
        }

		hkcu_init();

        // initialize the log file
        log_init(g_config.host_ip, g_config.host_port, CUCKOODBG);

        // initialize the Sleep() skipping stuff
        init_sleep_skip(g_config.first_process);

        // we skip a random given amount of milliseconds each run
        init_startup_time(g_config.startup_time);

        // disable the retaddr check if the user wants so
        //if(g_config.retaddr_check == 0) {
        //    hook_disable_retaddr_check();
        //}

		// initialize our unhook detection
        unhook_init_detection();

		// initialize terminate notification event
		terminate_event_init();

		// initialize all hooks
        set_hooks();

		// initialize context watchdog
		//init_watchdog();

		notify_successful_load();
    }
    else if(dwReason == DLL_PROCESS_DETACH) {
        log_free();
    }

	g_dll_main_complete = TRUE;

out:
	set_lasterrors(&lasterror);

	return TRUE;
}
