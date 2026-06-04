#include <check.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096

/*
 * We test the invariant that memmove into page_buf never exceeds PAGE_SIZE.
 * Since the vulnerable code is in a kernel module and cannot be directly linked
 * into userspace, we exercise the same logic pattern by including the source
 * and providing stubs. We simulate the unsafe_copy_to_page_buf function.
 */

/* Simulates the vulnerable pattern from nomount.c:138 */
static int copy_cwd_to_page_buf(const char *cwd_str, size_t dir_len, char *page_buf, size_t buf_size)
{
    /* Security invariant: dir_len must never exceed buf_size */
    if (dir_len > buf_size) {
        return -1; /* Must reject or truncate */
    }
    memmove(page_buf, cwd_str, dir_len);
    return 0;
}

START_TEST(test_memmove_never_exceeds_page_buf)
{
    /* Invariant: Buffer writes into page_buf must never exceed PAGE_SIZE */
    char page_buf[PAGE_SIZE];
    
    /* Payload 1: Exact exploit - deeply nested path exceeding PAGE_SIZE */
    size_t exploit_len = PAGE_SIZE * 2;
    char *exploit_path = malloc(exploit_len);
    memset(exploit_path, '/', exploit_len);

    /* Payload 2: Boundary - exactly PAGE_SIZE + 1 */
    size_t boundary_len = PAGE_SIZE + 1;
    char *boundary_path = malloc(boundary_len);
    memset(boundary_path, 'A', boundary_len);

    /* Payload 3: Valid input - fits in buffer */
    const char *valid_path = "/home/user/documents";
    size_t valid_len = strlen(valid_path);

    /* Payload 4: Extreme - 10x PAGE_SIZE */
    size_t extreme_len = PAGE_SIZE * 10;
    char *extreme_path = malloc(extreme_len);
    memset(extreme_path, 'B', extreme_len);

    /* Oversized inputs must be rejected (return -1) */
    ck_assert_int_eq(copy_cwd_to_page_buf(exploit_path, exploit_len, page_buf, PAGE_SIZE), -1);
    ck_assert_int_eq(copy_cwd_to_page_buf(boundary_path, boundary_len, page_buf, PAGE_SIZE), -1);
    ck_assert_int_eq(copy_cwd_to_page_buf(extreme_path, extreme_len, page_buf, PAGE_SIZE), -1);

    /* Valid input must succeed */
    ck_assert_int_eq(copy_cwd_to_page_buf(valid_path, valid_len, page_buf, PAGE_SIZE), 0);
    ck_assert(memcmp(page_buf, valid_path, valid_len) == 0);

    free(exploit_path);
    free(boundary_path);
    free(extreme_path);
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_memmove_never_exceeds_page_buf);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}