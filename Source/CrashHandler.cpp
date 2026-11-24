// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CrashHandler.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <dbghelp.h>
    #include <werapi.h>
    #pragma comment (lib, "dbghelp.lib")
#else
    #include <unistd.h>
#endif

namespace
{
    enum class TestCrash { none, av, throwing, abort, heap };

    TestCrash testKind  = TestCrash::none;
    int       testIndex = 0;

    void readTestCrashSetting()
    {
        const char* env = std::getenv ("DEXED_CLI_CRASH_TEST");
        if (env == nullptr || *env == 0)
            return;

        std::string spec (env);
        std::string kind = spec;
        const auto colon = spec.find (':');
        if (colon != std::string::npos)
        {
            kind = spec.substr (0, colon);
            testIndex = std::atoi (spec.c_str() + colon + 1);
        }

        if      (kind == "av")    testKind = TestCrash::av;
        else if (kind == "throw") testKind = TestCrash::throwing;
        else if (kind == "abort") testKind = TestCrash::abort;
        else if (kind == "heap")  testKind = TestCrash::heap;
    }

    // Writes straight to the OS handle: the CRT's stream locks may be held by
    // the very thread that just died.
    void writeStderr (const char* text)
    {
       #ifdef _WIN32
        HANDLE h = GetStdHandle (STD_ERROR_HANDLE);
        if (h != nullptr && h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile (h, text, (DWORD) std::strlen (text), &written, nullptr);
        }
       #else
        const ssize_t r = ::write (2, text, std::strlen (text));
        (void) r;
       #endif
    }

   #ifdef _WIN32
    volatile LONG reporting = 0;

    struct FatalInfo
    {
        EXCEPTION_POINTERS* exception;   // null for terminate/abort-style failures
        const char*         message;     // used when exception is null
        DWORD               threadId;
    };

    bool dumpDirectory (wchar_t* dir, DWORD size)
    {
        DWORD n = GetEnvironmentVariableW (L"DEXED_CLI_DUMP_DIR", dir, size);
        if (n > 0 && n < size)
            return true;

        n = GetEnvironmentVariableW (L"LOCALAPPDATA", dir, size);
        if (n == 0 || n + 16 >= size)
            return false;

        wcscat_s (dir, size, L"\\CrashDumps");
        return true;
    }

    void writeMiniDump (const FatalInfo& info)
    {
        wchar_t dir[MAX_PATH * 2];
        if (! dumpDirectory (dir, (DWORD) (sizeof (dir) / sizeof (dir[0]))))
        {
            writeStderr ("dexed-cli: no directory for a minidump (set DEXED_CLI_DUMP_DIR)\n");
            return;
        }

        CreateDirectoryW (dir, nullptr);

        wchar_t path[MAX_PATH * 2 + 64];
        swprintf_s (path, L"%s\\dexed-cli.%lu.dmp", dir, GetCurrentProcessId());

        HANDLE file = CreateFileW (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            writeStderr ("dexed-cli: could not create the minidump file\n");
            return;
        }

        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId          = info.threadId;
        mei.ExceptionPointers = info.exception;
        mei.ClientPointers    = FALSE;

        const auto type = (MINIDUMP_TYPE) (MiniDumpWithIndirectlyReferencedMemory
                                         | MiniDumpWithThreadInfo
                                         | MiniDumpWithProcessThreadData
                                         | MiniDumpWithUnloadedModules);

        const BOOL ok = MiniDumpWriteDump (GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                           info.exception != nullptr ? &mei : nullptr,
                                           nullptr, nullptr);
        CloseHandle (file);

        char utf8[MAX_PATH * 6];
        if (WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, (int) sizeof (utf8), nullptr, nullptr) == 0)
            std::strcpy (utf8, "(path not representable)");

        char line[sizeof (utf8) + 64];
        std::snprintf (line, sizeof (line), ok ? "dexed-cli: minidump written to %s\n"
                                               : "dexed-cli: minidump failed: %s\n", utf8);
        writeStderr (line);
    }

    DWORD WINAPI reportThread (LPVOID param)
    {
        const auto& info = *static_cast<const FatalInfo*> (param);

        char line[512];
        if (info.exception != nullptr)
        {
            const EXCEPTION_RECORD& rec = *info.exception->ExceptionRecord;

            if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2)
                std::snprintf (line, sizeof (line),
                               "dexed-cli: fatal: access violation (0xC0000005) %s address %p, "
                               "code at %p, thread %lu\n",
                               rec.ExceptionInformation[0] == 0 ? "reading"
                                 : rec.ExceptionInformation[0] == 8 ? "executing" : "writing",
                               (void*) rec.ExceptionInformation[1], rec.ExceptionAddress,
                               info.threadId);
            else
                std::snprintf (line, sizeof (line),
                               "dexed-cli: fatal: unhandled exception 0x%08lX at %p, thread %lu\n",
                               rec.ExceptionCode, rec.ExceptionAddress, info.threadId);
        }
        else
        {
            std::snprintf (line, sizeof (line), "dexed-cli: fatal: %s (thread %lu)\n",
                           info.message, info.threadId);
        }

        writeStderr (line);
        writeMiniDump (info);
        return 0;
    }

    [[noreturn]] void fatal (EXCEPTION_POINTERS* exception, const char* message, UINT exitCode)
    {
        // Report only the first failure. Any other thread that dies meanwhile
        // just waits to be torn down along with the process.
        if (InterlockedCompareExchange (&reporting, 1, 0) != 0)
            for (;;) Sleep (INFINITE);

        FatalInfo info { exception, message, GetCurrentThreadId() };

        // Report from a fresh thread: the failing one may have no stack left
        // (stack overflow) and MiniDumpWriteDump needs plenty.
        HANDLE t = CreateThread (nullptr, 1 << 20, reportThread, &info, 0, nullptr);
        if (t != nullptr)
        {
            WaitForSingleObject (t, 120000);
            CloseHandle (t);
        }
        else
        {
            reportThread (&info);
        }

        TerminateProcess (GetCurrentProcess(), exitCode);
        for (;;) Sleep (INFINITE);
    }

    LONG WINAPI onUnhandledException (EXCEPTION_POINTERS* exception)
    {
        fatal (exception, nullptr, exception->ExceptionRecord->ExceptionCode);
    }

    void onAbortSignal (int)
    {
        fatal (nullptr, "abort() called", CrashHandler::kExitAbnormal);
    }

    void onPureCall()
    {
        fatal (nullptr, "pure virtual function call", CrashHandler::kExitAbnormal);
    }

    void onInvalidParameter (const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
    {
        fatal (nullptr, "invalid parameter passed to a CRT function", CrashHandler::kExitAbnormal);
    }
   #endif

    [[noreturn]] void onTerminate()
    {
        std::string what = "std::terminate called";

        if (auto e = std::current_exception())
        {
            try                              { std::rethrow_exception (e); }
            catch (const std::exception& x)  { what = std::string ("uncaught exception: ") + x.what(); }
            catch (...)                      { what = "uncaught non-standard exception"; }
        }

       #ifdef _WIN32
        fatal (nullptr, what.c_str(), CrashHandler::kExitAbnormal);
       #else
        writeStderr (("dexed-cli: fatal: " + what + "\n").c_str());
        _exit (CrashHandler::kExitAbnormal);
       #endif
    }
}

void CrashHandler::installForThread()
{
    std::set_terminate (onTerminate);
}

void CrashHandler::install()
{
    readTestCrashSetting();

    std::set_terminate (onTerminate);

   #ifdef _WIN32
    SetUnhandledExceptionFilter (onUnhandledException);

    // abort() would otherwise fast-fail straight past every handler here.
    _set_abort_behavior (0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal (SIGABRT, onAbortSignal);
    _set_purecall_handler (onPureCall);
    _set_invalid_parameter_handler (onInvalidParameter);

    // Undo an inherited SEM_NOGPFAULTERRORBOX so fail-fast crashes still reach
    // WER (event log, LocalDumps), but never let WER block on a dialog.
    SetErrorMode (GetErrorMode() & ~(UINT) SEM_NOGPFAULTERRORBOX);
    WerSetFlags (WER_FAULT_REPORTING_NO_UI);
   #endif
}

void CrashHandler::maybeCrashForTest (int jobIndex)
{
    if (testKind == TestCrash::none || jobIndex != testIndex)
        return;

    switch (testKind)
    {
        case TestCrash::av:
            *static_cast<volatile int*> (nullptr) = 0;
            break;

        case TestCrash::throwing:
            throw std::runtime_error ("DEXED_CLI_CRASH_TEST");

        case TestCrash::abort:
            std::abort();

        case TestCrash::heap:
        {
            char* volatile p = static_cast<char*> (std::malloc (64));
            std::free (p);
            std::free (p);
            break;
        }

        case TestCrash::none:
            break;
    }
}
