/*
 * signals.h — Fatal-signal handlers + per-thread tool tracking.
 *
 * Three responsibilities:
 *   1. Ignore SIGPIPE so a closed client pipe does not silently terminate
 *      the server. Callers are expected to detect EPIPE on write and shut
 *      down gracefully.
 *   2. Install async-signal-safe handlers for SIGSEGV/SIGBUS/SIGABRT that
 *      emit a single diagnostic stderr line (including the currently-
 *      executing tool name, if known) before re-raising the default
 *      handler so core dumps and exit codes are preserved.
 *   3. Maintain per-thread "currently-executing tool" state, set around
 *      MCP tool dispatch so the fatal-signal handler can report which
 *      tool was running when the process crashed.
 */
#ifndef CBM_SIGNALS_H
#define CBM_SIGNALS_H

#include <stdbool.h>
#include <stdio.h>

/* Install SIGPIPE-ignore + fatal-signal handlers. Idempotent; safe to call
 * from main() before any IO. SIGTERM/SIGINT are handled separately by the
 * existing signal_handler in main.c. */
void cbm_signals_install(void);

/* Record the tool currently executing on this thread. The name is copied
 * into a fixed-size TLS buffer so the pointer does not need to outlive the
 * call. Pass NULL or call cbm_signals_clear_current_tool to clear. */
void cbm_signals_set_current_tool(const char *tool_name);

/* Clear the per-thread current-tool slot. */
void cbm_signals_clear_current_tool(void);

/* Read the per-thread current-tool slot (mostly for tests). Returns the
 * empty string when no tool is in flight. The buffer is owned by signals.c
 * and remains valid for the thread's lifetime. */
const char *cbm_signals_current_tool(void);

#endif /* CBM_SIGNALS_H */
