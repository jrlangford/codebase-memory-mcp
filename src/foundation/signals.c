/*
 * signals.c — see signals.h.
 *
 * On Windows the fatal-signal install is a no-op; only SIGPIPE-ignore is
 * meaningful on POSIX. The TLS slot is portable.
 */
#include "signals.h"
#include "compat.h"

#include <signal.h>
#include <stddef.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define CBM_SIGNALS_TOOL_BUFSZ 64

/* Per-thread buffer holding the currently-executing tool name. Read from
 * the fatal-signal handler, which on POSIX runs on the same thread that
 * received the signal — a plain TLS char buffer is therefore safe. */
static CBM_TLS char g_current_tool[CBM_SIGNALS_TOOL_BUFSZ] = {0};

void cbm_signals_set_current_tool(const char *tool_name) {
    if (!tool_name) {
        g_current_tool[0] = '\0';
        return;
    }
    size_t n = strlen(tool_name);
    if (n >= CBM_SIGNALS_TOOL_BUFSZ) {
        n = CBM_SIGNALS_TOOL_BUFSZ - 1;
    }
    memcpy(g_current_tool, tool_name, n);
    g_current_tool[n] = '\0';
}

void cbm_signals_clear_current_tool(void) {
    g_current_tool[0] = '\0';
}

const char *cbm_signals_current_tool(void) {
    return g_current_tool;
}

#ifndef _WIN32

/* Async-signal-safe stderr emitter: write(2) only, no stdio. */
static void safe_write(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDERR_FILENO, s, n);
        if (w <= 0) {
            return;
        }
        s += (size_t)w;
        n -= (size_t)w;
    }
}

static void safe_write_cstr(const char *s) {
    if (!s) {
        return;
    }
    safe_write(s, strlen(s));
}

static const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGABRT:
        return "SIGABRT";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
    default:
        return "UNKNOWN";
    }
}

/* Fatal-signal handler. Emits one structured stderr line then re-raises
 * the signal under SIG_DFL so the kernel's default behavior (core dump,
 * non-zero exit) is preserved for downstream tooling. */
static void fatal_handler(int sig) {
    safe_write_cstr("level=fatal msg=signal name=");
    safe_write_cstr(signal_name(sig));
    safe_write_cstr(" tool=");
    if (g_current_tool[0] != '\0') {
        safe_write_cstr(g_current_tool);
    } else {
        safe_write_cstr("-");
    }
    safe_write_cstr("\n");

    /* Restore default and re-raise so core/exit-code semantics are kept. */
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    raise(sig);
}

void cbm_signals_install(void) {
    /* (1) Ignore SIGPIPE — broken-client-pipe must not silently kill us.
     *     Write paths inspect ferror/errno and shut down gracefully. */
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, NULL);

    /* (2) Fatal signals → diagnostic line + re-raise. */
    struct sigaction fa = {0};
    fa.sa_handler = fatal_handler;
    sigemptyset(&fa.sa_mask);
    fa.sa_flags = SA_NODEFER | SA_RESETHAND; /* one-shot per signal */
    sigaction(SIGSEGV, &fa, NULL);
    sigaction(SIGBUS, &fa, NULL);
    sigaction(SIGABRT, &fa, NULL);
    sigaction(SIGFPE, &fa, NULL);
    sigaction(SIGILL, &fa, NULL);
}

#else /* _WIN32 */

void cbm_signals_install(void) {
    /* No SIGPIPE on Windows; structured fatal-signal reporting is left to
     * a future Windows-specific implementation. */
}

#endif
