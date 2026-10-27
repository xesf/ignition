/*
 * test_batch_copies.c - the compiled copy-batching policy.
 *
 * The library under test is the actual engine build, so the policy is
 * fixed at library compile time. TEST_EXPECT_BATCH_COPIES is this test's
 * expectation for that build:
 *
 *   0 for explicit 0, for IGN_GLES outside Android with the flag
 *     undefined, and for any build without IGN_GLES (even with flag 1);
 *   1 for IGN_GLES with numeric IGN_GLES_BATCH_COPIES=1, and for Android
 *     IGN_GLES with the flag undefined.
 *
 * Build this file once per library variant. The Android build-default
 * invocation (expectation 1) fails on the old source; explicit-0 and
 * non-GLES variants pass both before and after the policy change.
 */

#include <stdio.h>

#include "rnd.h"

#ifndef TEST_EXPECT_BATCH_COPIES
#error "define TEST_EXPECT_BATCH_COPIES=0 or 1 for the library under test"
#endif

#if TEST_EXPECT_BATCH_COPIES != 0 && TEST_EXPECT_BATCH_COPIES != 1
#error "TEST_EXPECT_BATCH_COPIES must be 0 or 1"
#endif

int main(void)
{
    int got = rnd_batch_copies();

    if (got != TEST_EXPECT_BATCH_COPIES) {
        fprintf(stderr, "FAIL: rnd_batch_copies() is %d, expected %d\n",
                got, TEST_EXPECT_BATCH_COPIES);
        puts("FAIL");
        return 1;
    }
    puts("ok");
    return 0;
}
