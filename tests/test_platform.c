/*
 * test_platform.c — RED phase tests for foundation/platform.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "../src/foundation/platform.h"
#include "../src/foundation/compat_fs.h"
#include <unistd.h>

TEST(platform_now_ns) {
    uint64_t t1 = cbm_now_ns();
    ASSERT_GT(t1, 0);
    /* Busy-wait a tiny bit */
    for (volatile int i = 0; i < 100000; i++) {}
    uint64_t t2 = cbm_now_ns();
    ASSERT_GT(t2, t1);
    PASS();
}

TEST(platform_now_ms) {
    uint64_t t1 = cbm_now_ms();
    ASSERT_GT(t1, 0);
    PASS();
}

TEST(platform_nprocs) {
    int n = cbm_nprocs();
    ASSERT_GT(n, 0);
    ASSERT_LT(n, 10000); /* sanity */
    PASS();
}

TEST(platform_file_exists) {
    /* This test file should exist */
    ASSERT_TRUE(cbm_file_exists("tests/test_platform.c"));
    ASSERT_FALSE(cbm_file_exists("nonexistent_file_xyz.txt"));
    PASS();
}

TEST(platform_is_dir) {
    ASSERT_TRUE(cbm_is_dir("tests"));
    ASSERT_FALSE(cbm_is_dir("tests/test_platform.c"));
    ASSERT_FALSE(cbm_is_dir("nonexistent_dir"));
    PASS();
}

TEST(platform_file_size) {
    int64_t sz = cbm_file_size("tests/test_platform.c");
    ASSERT_GT(sz, 0);
    ASSERT_EQ(cbm_file_size("nonexistent_file_xyz.txt"), -1);
    PASS();
}

TEST(platform_mmap) {
    /* mmap this test file and verify first bytes */
    size_t sz = 0;
    void *data = cbm_mmap_read("tests/test_platform.c", &sz);
    ASSERT_NOT_NULL(data);
    ASSERT_GT(sz, 0);
    /* First line should be the comment */
    ASSERT(memcmp(data, "/*", 2) == 0);
    cbm_munmap(data, sz);
    PASS();
}

TEST(platform_mmap_nonexistent) {
    size_t sz = 0;
    void *data = cbm_mmap_read("nonexistent_xyz.txt", &sz);
    ASSERT_NULL(data);
    PASS();
}

TEST(compat_rename_replaces_existing) {
    /* cbm_rename must overwrite an existing destination and remove the source —
     * the atomic swap the incremental persist relies on (beads-tm8ib). */
    char *dir = th_mktempdir("cbm_rename");
    ASSERT_NOT_NULL(dir);
    char from[300];
    char to[300];
    snprintf(from, sizeof(from), "%s/from", dir);
    snprintf(to, sizeof(to), "%s/to", dir);
    ASSERT_EQ(th_write_file(from, "NEW"), 0);
    ASSERT_EQ(th_write_file(to, "OLD"), 0);

    ASSERT_EQ(cbm_rename(from, to), 0);
    ASSERT_FALSE(cbm_file_exists(from)); /* source consumed */
    ASSERT_TRUE(cbm_file_exists(to));

    char buf[16] = {0};
    FILE *fp = fopen(to, "r");
    ASSERT_NOT_NULL(fp);
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    ASSERT_STR_EQ(buf, "NEW"); /* destination now holds the source's content */

    cbm_unlink(to);
    PASS();
}

SUITE(platform) {
    RUN_TEST(platform_now_ns);
    RUN_TEST(compat_rename_replaces_existing);
    RUN_TEST(platform_now_ms);
    RUN_TEST(platform_nprocs);
    RUN_TEST(platform_file_exists);
    RUN_TEST(platform_is_dir);
    RUN_TEST(platform_file_size);
    RUN_TEST(platform_mmap);
    RUN_TEST(platform_mmap_nonexistent);
}
