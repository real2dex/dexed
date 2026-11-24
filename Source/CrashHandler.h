// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * Makes an abnormal end of the process loud instead of silent.
 *
 * A batch that dies half way through must never look like a short, successful
 * one. After install():
 *
 *  - an unhandled SEH exception (access violation, ...) prints its code to
 *    stderr, writes a minidump and ends the process with that code as the
 *    exit status (e.g. 0xC0000005);
 *  - std::terminate (an exception escaping a thread, ...), abort(), a pure
 *    virtual call or a CRT invalid parameter prints what happened, writes a
 *    minidump and exits with kExitAbnormal;
 *  - on Windows, Windows Error Reporting is switched back on for this process
 *    without any UI. Parents built on libuv (VS Code's terminal, node, ...)
 *    hand down SEM_NOGPFAULTERRORBOX, which silently disables WER, so a crash
 *    left neither an Application Error event nor a LocalDumps dump. Fail-fast
 *    crashes such as heap corruption (0xC0000374) bypass every in-process
 *    handler; WER is the only thing that can record them.
 *
 * Minidumps go to %DEXED_CLI_DUMP_DIR% if set, else %LOCALAPPDATA%\CrashDumps,
 * as dexed-cli.<pid>.dmp. Nothing is written on non-Windows platforms, where
 * the default signal behaviour already yields a non-zero status.
 */
namespace CrashHandler
{
    /** Exit status for terminate/abort-style failures. */
    constexpr int kExitAbnormal = 3;

    void install();

    /**
     * Must also be called at the start of every other thread: MSVC keeps the
     * std::terminate handler per thread, so without it an exception escaping
     * a worker would only show up as "abort() called".
     */
    void installForThread();

    /**
     * Testing aid: DEXED_CLI_CRASH_TEST=<kind>[:<job index>] makes the batch
     * crash on purpose when that job reports (default index 0). Kinds are
     * av, throw, abort and heap. Lets a caller check that it notices a batch
     * that dies part way through, and that dumps land where expected.
     */
    void maybeCrashForTest (int jobIndex);
}
