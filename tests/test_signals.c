/*
 * test_signals.c — Unit tests for foundation/signals.
 *
 * SIGPIPE-ignore and fatal-signal re-raise are process-level behaviors
 * exercised by integration tests (see scripts/sigpipe_smoke.sh). This
 * file covers the user-visible primitives: the per-thread current-tool
 * TLS slot.
 */
#include "test_framework.h"
#include "../src/foundation/signals.h"
#include <string.h>

TEST(signals_current_tool_starts_empty) {
    cbm_signals_clear_current_tool();
    ASSERT(strcmp(cbm_signals_current_tool(), "") == 0);
    PASS();
}

TEST(signals_set_and_read_current_tool) {
    cbm_signals_set_current_tool("index_repository");
    ASSERT(strcmp(cbm_signals_current_tool(), "index_repository") == 0);
    cbm_signals_clear_current_tool();
    PASS();
}

TEST(signals_set_null_clears_current_tool) {
    cbm_signals_set_current_tool("search_graph");
    cbm_signals_set_current_tool(NULL);
    ASSERT(strcmp(cbm_signals_current_tool(), "") == 0);
    PASS();
}

TEST(signals_set_long_name_truncates) {
    /* Buffer is 64 bytes; a 100-byte name should be truncated and still
     * NUL-terminated. We only check that strlen stays under the buffer
     * size — any truncation that preserves the prefix is acceptable. */
    const char *long_name =
        "this_is_a_very_long_tool_name_that_definitely_exceeds_the_fixed_buffer_size";
    cbm_signals_set_current_tool(long_name);
    const char *got = cbm_signals_current_tool();
    ASSERT(strlen(got) < 64);
    ASSERT(strncmp(got, long_name, strlen(got)) == 0);
    cbm_signals_clear_current_tool();
    PASS();
}

SUITE(signals) {
    RUN_TEST(signals_current_tool_starts_empty);
    RUN_TEST(signals_set_and_read_current_tool);
    RUN_TEST(signals_set_null_clears_current_tool);
    RUN_TEST(signals_set_long_name_truncates);
}
