// Adapted from AwesomeObserver/windows; bounded I/O and credential redaction.
/* Preserve exceptions/fatal assertions, guest callers and nearby objects on Windows.
 * DbgHelp runs on a dedicated thread, with a bounded wait on the faulting thread. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#include "crash_win.h"

/* MinGW's callback union omits the documented MINIDUMP_IO_CALLBACK member.
 * Copy its SDK layout at the existing union offset, with the SDK's pack(4).
 * https://learn.microsoft.com/windows/win32/api/minidumpapiset/ns-minidumpapiset-minidump_io_callback */
#pragma pack(push, 4)
typedef struct { HANDLE Handle; ULONG64 Offset; void *Buffer; ULONG BufferBytes; } BbDumpIo;
#pragma pack(pop)
_Static_assert(sizeof(BbDumpIo) == 28, "Windows x64 minidump I/O ABI");

typedef BOOL (WINAPI *WriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);
static WriteDump write_dump;
static HANDLE requested, completed;
static EXCEPTION_RECORD record;
static CONTEXT context;
static EXCEPTION_POINTERS pointers = {&record, &context};
static DWORD fault_thread, dump_error;
static HANDLE fault_process;
static DWORD fault_pid;
static volatile LONG dump_claimed;
static DWORD reporter_thread, watchdog_thread;
static DWORD fatal_exit_code(void) {
    return record.ExceptionCode == BB_CRASH_ASSERT_EXCEPTION ? 23 : 128 + 11;
}
static ULONGLONG write_deadline;
#define MAX_DUMP_BYTES (64u*1024u*1024u)
#define MAX_SECRETS 64
static struct { volatile LONG64 address; size_t size; } secrets[MAX_SECRETS];
static unsigned secret_cursor;
static size_t system_page;

unsigned crash_win_protect_secret(const void *address, size_t size) {
    if (!address || !size || size > 4096) return 0;
    for (unsigned i=0; i<MAX_SECRETS; ++i) {
        if (InterlockedCompareExchange64(&secrets[i].address, -1, 0) == 0) {
            secrets[i].size = size;
            InterlockedExchange64(&secrets[i].address, (LONG64)(uintptr_t)address);
            return i+1;
        }
    }
    /* Fail closed: capturing optional memory is no longer safe. */
    InterlockedExchange(&dump_claimed, 1);
    return 0;
}
void crash_win_unprotect_secret(unsigned token) {
    if (token && token <= MAX_SECRETS) InterlockedExchange64(&secrets[token-1].address, 0);
}
static int sensitive_page(uintptr_t address) {
    for (unsigned i=0; i<MAX_SECRETS; ++i) {
        uintptr_t at=(uintptr_t)InterlockedCompareExchange64(&secrets[i].address, 0, 0);
        if (at == UINTPTR_MAX) return 1;
        if (at && at <= UINTPTR_MAX-secrets[i].size && at < address+system_page && address < at+secrets[i].size) return 1;
    }
    return 0;
}
static char fatal_reason[256];
static wchar_t dump_directory[1024];
static char dump_path_utf8[4096];
static uintptr_t guest_base;
static size_t guest_size;

/* Manually mapped guest code has no PE module for DbgHelp to copy. A normal
 * minidump keeps only 256 bytes around the fault PC and omits guest heap objects.
 * Select at most 2 MiB of extra readable pages, without taking runtime locks or
 * allocating on a possibly damaged heap. Nothing here runs during gameplay. */
#define EXTRA_BYTES (2u*1024u*1024u)
#define MAX_PAGES 512u
#define MAX_FRAMES 32u
#define MAX_ROOT_PAGES 128u
static struct { uintptr_t address; unsigned object; } pages[MAX_PAGES];
static unsigned page_count, object_pages, page_cursor, frame_count;
static ULONGLONG capture_deadline;

void crash_win_set_guest(const void *base, size_t size) {
    guest_base = (uintptr_t)base;
    guest_size = size;
}

static int read_memory(uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(fault_process, (const void *)address, out, size, &got) && got == size;
}

static void add_page(uintptr_t address, unsigned object) {
    if (address < 65536 || page_count >= MAX_PAGES ||
            (page_count+1)*system_page > EXTRA_BYTES || GetTickCount64() >= capture_deadline) return;
    address -= address % system_page;
    if (address > UINTPTR_MAX-system_page || sensitive_page(address)) return;
    for (unsigned i=0; i<page_count; ++i) if (pages[i].address == address) return;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQueryEx(fault_process, (const void *)address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readable)) return;
    /* Do not follow code or DLL globals as heap roots. Guest heaps are private
     * allocations; executable pages are captured explicitly for PCs/callers. */
    if (object && (info.Type == MEM_IMAGE ||
            (info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))) return;
    pages[page_count].address = address;
    pages[page_count++].object = object;
    object_pages += object != 0;
}

static void add_object(uintptr_t address) {
    if (address < 65536 || address > UINTPTR_MAX-system_page) return;
    /* Keep fields on either side of an interior pointer, including fields
     * crossing a page boundary. Never make an unreadable page readable. */
    add_page(address, 1);
    add_page(address-256, 1);
    add_page(address+512, 1);
}

static void add_code(uintptr_t address) {
    if (fault_process != GetCurrentProcess()) {
        /* No guest registration exists in the observer. Capture readable private
         * executable pages around PCs/callers, under the same 2 MiB budget. */
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQueryEx(fault_process, (const void *)address, &info, sizeof(info)) ||
                info.Type != MEM_PRIVATE || !(info.Protect &
                (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return;
        uintptr_t first = address > 8192 ? address-8192 : 0;
        first -= first % system_page;
        for (unsigned i=0; i<5 && first<=UINTPTR_MAX-system_page; ++i, first+=system_page) add_page(first, 0);
        return;
    }
    if (!guest_base || address < guest_base || address-guest_base >= guest_size) return;
    uintptr_t first = address-guest_base > 8192 ? address-8192 : guest_base;
    uintptr_t last = guest_base+guest_size;
    if (address <= UINTPTR_MAX-8192 && address+8192 < last) last = address+8192;
    first -= first % system_page;
    for (uintptr_t p=first; p<last && p<=UINTPTR_MAX-system_page; p+=system_page) add_page(p, 0);
}

static void select_memory(void) {
    page_count = object_pages = page_cursor = frame_count = 0;
    capture_deadline = GetTickCount64()+1500;
    add_code((uintptr_t)context.Rip);
    /* Preserve raw stack bytes without asking DbgHelp to unwind a broken
     * frame pointer. These pages share the existing extra-memory budget. */
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    if (context.Rbp > (DWORD64)(uintptr_t)system.lpMaximumApplicationAddress)
        for (unsigned i=0;i<4 && context.Rsp<=UINTPTR_MAX-i*system_page;++i)
            add_page((uintptr_t)context.Rsp+i*system_page, 0);
    uintptr_t frame = (uintptr_t)context.Rbp;
    for (; frame_count<MAX_FRAMES && GetTickCount64()<capture_deadline; ++frame_count) {
        uintptr_t pair[2];
        if (!read_memory(frame, pair, sizeof(pair))) break;
        add_code(pair[1]);
        /* A corrupt chain must not cause an unbounded walk or address wrap. */
        if (pair[0] <= frame || pair[0]-frame > 1024*1024) break;
        frame = pair[0];
    }
    const uintptr_t registers[] = {context.Rax,context.Rbx,context.Rcx,context.Rdx,
        context.Rsi,context.Rdi,context.R8,context.R9,context.R10,context.R11,
        context.R12,context.R13,context.R14,context.R15};
    for (unsigned i=0; i<sizeof(registers)/sizeof(registers[0]); ++i) add_object(registers[i]);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
        add_object((uintptr_t)record.ExceptionInformation[1]);
    /* Stack locals often contain the object from which a now-null field was
     * loaded. Normal minidumps preserve the stack, but not its pointed-to data. */
    uintptr_t words[32];
    uintptr_t stack = (uintptr_t)context.Rsp;
    for (size_t offset=0; offset<16384 && stack<=UINTPTR_MAX-offset &&
            object_pages<MAX_ROOT_PAGES && GetTickCount64()<capture_deadline; offset+=sizeof(words)) {
        if (!read_memory(stack+offset, words, sizeof(words))) break;
        for (unsigned j=0; j<sizeof(words)/sizeof(words[0]); ++j) add_object(words[j]);
    }
    /* One further pointer level. Interior pointers may address an object in
     * the middle of a page, so scan the whole selected root page. */
    unsigned roots = page_count;
    uintptr_t object_words[512];
    size_t bytes = system_page < sizeof(object_words) ? system_page : sizeof(object_words);
    for (unsigned i=0; i<roots && GetTickCount64()<capture_deadline; ++i) {
        if (!pages[i].object || !read_memory(pages[i].address, object_words, bytes)) continue;
        for (size_t j=0; j<bytes/sizeof(object_words[0]); ++j) add_object(object_words[j]);
    }
}

static BOOL CALLBACK dump_callback(void *unused, MINIDUMP_CALLBACK_INPUT *input,
        MINIDUMP_CALLBACK_OUTPUT *output) {
    (void)unused;
    if (input->CallbackType == IncludeThreadCallback) {
        /* Never let DbgHelp suspend the reporter or its deadline watchdog.
         * IncludeThread is invoked before suspension (Microsoft's contract). */
        if (fault_process == GetCurrentProcess() &&
                (input->IncludeThread.ThreadId == reporter_thread ||
                 input->IncludeThread.ThreadId == watchdog_thread)) return FALSE;
        SYSTEM_INFO system;
        GetSystemInfo(&system);
        if (input->IncludeThread.ThreadId == fault_thread &&
                context.Rbp > (DWORD64)(uintptr_t)system.lpMaximumApplicationAddress)
            /* Server DbgHelp can unwind even with ThreadWriteStack cleared.
             * Exclude only this automatic thread capture when its frame is
             * noncanonical. ExceptionStream still receives the ORIGINAL full
             * CONTEXT/record via MINIDUMP_EXCEPTION_INFORMATION; select_memory
             * independently retains bounded raw stack/code/object pages.
             * Never manufacture registers or relax the diagnostic deadline. */
            return FALSE;
    } else if (input->CallbackType == ThreadCallback) {
        SYSTEM_INFO system;
        GetSystemInfo(&system);
        if (input->Thread.ThreadId == fault_thread &&
                context.Rbp > (DWORD64)(uintptr_t)system.lpMaximumApplicationAddress)
            output->ThreadWriteFlags &= ~ThreadWriteStack;
        /* Context, exception and manually bounded stack pages remain original.
         * Do not alter the guest registers to manufacture an unwindable frame. */
    } else if (input->CallbackType == IoStartCallback) {
        output->Status = S_FALSE; // Own I/O permits a hard file size limit.
    } else if (input->CallbackType == IoWriteAllCallback) {
        BbDumpIo io;
        memcpy(&io, &input->Thread, sizeof(io));
        if (GetTickCount64() >= write_deadline || io.Offset > MAX_DUMP_BYTES ||
                io.BufferBytes > MAX_DUMP_BYTES-io.Offset) {
            output->Status = HRESULT_FROM_WIN32(ERROR_CANCELLED); return FALSE;
        }
        LARGE_INTEGER offset; offset.QuadPart = io.Offset;
        DWORD written=0;
        if (!SetFilePointerEx(io.Handle, offset, NULL, FILE_BEGIN) ||
                !WriteFile(io.Handle, io.Buffer, io.BufferBytes, &written, NULL) ||
                written != io.BufferBytes) {
            output->Status = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT); return FALSE;
        }
        output->Status = S_OK;
    } else if (input->CallbackType == IoFinishCallback) {
        output->Status = S_OK;
    } else if (input->CallbackType == RemoveMemoryCallback) {
        while (secret_cursor < MAX_SECRETS) {
            unsigned i=secret_cursor++;
            uintptr_t at=(uintptr_t)InterlockedCompareExchange64(&secrets[i].address, 0, 0);
            if (!at || at == UINTPTR_MAX) continue;
            uintptr_t base=at-at%system_page;
            output->MemoryBase=base;
            output->MemorySize=(ULONG)((at-base+secrets[i].size+system_page-1)/system_page*system_page);
            return TRUE;
        }
        return FALSE;
    } else if (input->CallbackType == ModuleCallback) {
        output->ModuleWriteFlags &= ~(ModuleWriteDataSeg | ModuleWriteTlsData);
    } else if (input->CallbackType == MemoryCallback) {
        if (page_cursor >= page_count) return FALSE;
        output->MemoryBase = pages[page_cursor++].address;
        output->MemorySize = (ULONG)system_page;
    } else if (input->CallbackType == ReadMemoryFailureCallback) {
        /* Other guest threads may release an optional page during collection.
         * Retain the exception and readable pages when that happens. */
        output->Status = S_OK;
    } else if (input->CallbackType == CancelCallback) {
        output->CheckCancel = TRUE;
        output->Cancel = GetTickCount64() >= write_deadline;
    }
    return TRUE;
}

static DWORD WINAPI dump_worker(void *unused) {
    (void)unused;
    WaitForSingleObject(requested, INFINITE);
#ifdef BB_CRASH_TEST_STALL
    if (GetEnvironmentVariableW(L"BB_TEST_CRASH_WRITER_STALL", NULL, 0)) Sleep(20000);
#endif
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[1200];
    _snwprintf(path, sizeof(path)/sizeof(path[0]),
        L"%ls\\bb-crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", dump_directory,
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        fault_pid);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, dump_path_utf8, sizeof(dump_path_utf8), NULL, NULL);
    wchar_t partial[1210];
    _snwprintf(partial, sizeof(partial)/sizeof(partial[0]), L"%ls.part", path);
    /* A deadline/process termination leaves an explicitly incomplete file,
     * never a .dmp that appears to be a successfully published diagnosis. */
    HANDLE file = CreateFileW(partial, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        dump_error = GetLastError();
    } else {
        MINIDUMP_EXCEPTION_INFORMATION info = {fault_thread, &pointers, FALSE};
        select_memory();
        secret_cursor=0;
        write_deadline=GetTickCount64()+5000;
        char description[768];
        int length = snprintf(description, sizeof(description),
            "BBPORT_GUEST_CONTEXT_V1\nimage_base=0x%llx\nimage_size=0x%llx\n"
            "page_size=%zu\nextra_pages=%u\nextra_limit=%u\ncaller_frames=%u\nreason=%s\n",
            (unsigned long long)guest_base, (unsigned long long)guest_size,
            system_page, page_count, EXTRA_BYTES, frame_count, fatal_reason);
        MINIDUMP_USER_STREAM stream = {CommentStreamA, (ULONG)length+1, description};
        MINIDUMP_USER_STREAM_INFORMATION streams = {1, &stream};
        MINIDUMP_CALLBACK_INFORMATION callback = {dump_callback, NULL};
        if (!write_dump(fault_process, fault_pid, file,
                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo | MiniDumpFilterModulePaths | MiniDumpFilterMemory,
                &info, &streams, &callback)) {
            /* Optional context must not cost us the original minimal dump. */
            page_count=page_cursor=secret_cursor=0;
            LARGE_INTEGER start = {.QuadPart=0};
            if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN) || !SetEndOfFile(file) ||
                    !write_dump(fault_process, fault_pid, file,
                        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpFilterModulePaths | MiniDumpFilterMemory, &info, NULL, &callback)) {
                dump_error = GetLastError();
            }
        }
        if (dump_error) {
            CloseHandle(file);
            DeleteFileW(partial); // discard only this newly created, incomplete dump
        } else {
            CloseHandle(file);
            if (!MoveFileExW(partial, path, 0)) {
                dump_error=GetLastError();
                DeleteFileW(partial);
            }
        }
    }
    SetEvent(completed);
    return 0;
}

static DWORD WINAPI dump_watchdog(void *unused) {
    (void)unused;
    WaitForSingleObject(requested, INFINITE);
    if (fault_process == GetCurrentProcess()) {
        /* Independent of DbgHelp callbacks: a stuck native unwinder cannot
         * defeat the deadline by never invoking CancelCallback or returning.
         * DbgHelp explicitly excludes this thread before suspending peers. */
        DWORD result=WaitForSingleObject(completed, 8000);
        if (result == WAIT_OBJECT_0) Sleep(3000);
        TerminateProcess(GetCurrentProcess(), fatal_exit_code());
    }
    return 0;
}

void crash_win_init(void) {
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    system_page = system_info.dwPageSize;
    const unsigned char *base = (const unsigned char *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *pe = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    printf("Runtime build: PE timestamp=%08lx, image_size=0x%lx\n",
        pe->FileHeader.TimeDateStamp, pe->OptionalHeader.SizeOfImage);
    wchar_t setting[8];
    if (GetEnvironmentVariableW(L"BB_CRASH_DUMP", setting, 8) && setting[0] == L'0') {
        puts("Crash reporting: disabled (BB_CRASH_DUMP=0)");
        return;
    }
    DWORD length = GetEnvironmentVariableW(L"BB_CRASH_DIR", dump_directory, 1024);
    if (length >= 1024) { fputs("Crash reporting: BB_CRASH_DIR is too long\n", stderr); return; }
    if (!length) {
        CreateDirectoryW(L"logs", NULL);
        wcscpy(dump_directory, L"logs\\crashes");
    }
    CreateDirectoryW(dump_directory, NULL);
    /* Preload before entering the guest/driver, avoiding LoadLibrary in a fault handler. */
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbghelp) {
        fprintf(stderr, "Crash reporting: cannot load system DbgHelp (Windows error 0x%08lx)\n", GetLastError());
        return;
    }
    write_dump = (WriteDump)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    if (!write_dump) { fputs("Crash reporting: MiniDumpWriteDump unavailable\n", stderr); return; }
    requested = CreateEventW(NULL, TRUE, FALSE, NULL);
    completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!requested || !completed) {
        fprintf(stderr, "Crash reporting: event creation failed (Windows error 0x%08lx)\n", GetLastError());
        if (requested) CloseHandle(requested);
        if (completed) CloseHandle(completed);
        requested = completed = NULL;
        return;
    }
    /* Start the independent guard first. Both wait for the one published
     * fatal snapshot; no game work runs on either diagnostic thread. */
    HANDLE thread = CreateThread(NULL, 0, dump_watchdog, NULL, 0, &watchdog_thread);
    if (!thread) {
        fprintf(stderr, "Crash reporting: watchdog creation failed (Windows error 0x%08lx)\n", GetLastError());
        CloseHandle(requested); CloseHandle(completed);
        requested=completed=NULL;
        return;
    }
    CloseHandle(thread);
    thread = CreateThread(NULL, 0, dump_worker, NULL, 0, &reporter_thread);
    if (!thread) {
        fprintf(stderr, "Crash reporting: writer thread creation failed (Windows error 0x%08lx)\n", GetLastError());
        /* The guard is parked on this event until process exit. Do not close
         * a handle under its wait; disabling submission cannot wake it. */
        requested=NULL;
        return;
    }
    CloseHandle(thread);
    puts("Crash reporting: dedicated dump writer and deadline watchdog ready");
}

static void submit_dump(EXCEPTION_POINTERS *exception, const char *reason,
        HANDLE process, DWORD pid, DWORD tid) {
    if (!requested || !completed || GetCurrentThreadId() == reporter_thread) return;
    /* First failure owns the snapshot. Other fatal threads wait for the same
     * completed file instead of overwriting context or terminating mid-write. */
    if (InterlockedCompareExchange(&dump_claimed, 1, 0) == 0) {
        record = *exception->ExceptionRecord;
        context = *exception->ContextRecord;
        snprintf(fatal_reason, sizeof(fatal_reason), "%s", reason);
        fault_process = process;
        fault_pid = pid;
        fault_thread = tid;
        SetEvent(requested);
    }
    if (WaitForSingleObject(completed, 10000) != WAIT_OBJECT_0) {
        fprintf(stderr, "Crash dump: timed out after 10 seconds\n");
    } else if (dump_error) {
        fprintf(stderr, "Crash dump: failed (Windows error 0x%08lx)\n", dump_error);
    } else {
        fprintf(stderr, "Crash dump: %s\n", dump_path_utf8);
    }
}

void crash_win_dump(EXCEPTION_POINTERS *exception) {
    if (GetCurrentThreadId() == reporter_thread && dump_claimed) {
        /* A damaged reporter must not replace the original fault/exit status.
         * Wake the first faulting thread and retire only this diagnostic thread. */
        dump_error=ERROR_INVALID_DATA;
        SetEvent(completed);
        ExitThread(1);
    }
    submit_dump(exception, "Windows exception", GetCurrentProcess(), GetCurrentProcessId(), GetCurrentThreadId());
}

void crash_win_dump_external(HANDLE process, DWORD pid, DWORD tid, EXCEPTION_POINTERS *exception) {
    submit_dump(exception, "External crash monitor: unhandled Windows exception", process, pid, tid);
}

__attribute__((noinline)) void crash_win_dump_fatal(const char *reason) {
    if (!requested || !completed) return;
    CONTEXT current;
    RtlCaptureContext(&current);
    EXCEPTION_RECORD fatal = {0};
    fatal.ExceptionCode = BB_CRASH_ASSERT_EXCEPTION;
    fatal.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    fatal.ExceptionAddress = (void *)(uintptr_t)current.Rip;
    EXCEPTION_POINTERS exception = {&fatal, &current};
    submit_dump(&exception, reason ? reason : "Fatal stop", GetCurrentProcess(), GetCurrentProcessId(), GetCurrentThreadId());
}
#endif
