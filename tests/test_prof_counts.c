/*
 * test_prof_counts.c - renderer counters averaged in the real prof_frame
 * report.
 *
 * This links the actual src/prof.c with IGN_PROF. clock_gettime is defined
 * here so the two-second report boundary is deterministic. The expectation is:
 *
 *   counter 0 copy calls, 1 requested copy pixels, 2 draw calls,
 *   3 uploaded vertex bytes;
 *   per-frame averages over the same window as the existing "prof:" line;
 *   reset when that line is printed; totals are 64-bit;
 *   one separate "prof-counts:" stdout line, under 512 bytes.
 *
 * Return 1 is a test failure, 2 an environment failure (no IGN_PROF
 * prof_frame, or the clock replacement was not used).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "prof.h"

typedef void (*prof_frame_fn)(void);
typedef void (*prof_count_fn)(int counter, uint64_t amount);

enum { TEST_COUNTERS = PROF_COUNTERS };

/* The header enum is part of the contract, including in disabled builds. */
typedef char test_counter_ids_are_contract[
    PROF_COPY_CALLS == 0 && PROF_COPY_PIXELS == 1 && PROF_DRAW_CALLS == 2 &&
    PROF_UPLOAD_BYTES == 3 && PROF_COUNTERS == 4 ? 1 : -1];

#define REPORT_CAP 8192
#define COUNTS_LINE_MAX 512

static long long s_now_ms = 10000;
static int s_clock_calls;

/* The production clock boundary is deterministic in this host executable. */
static int test_clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;
    s_clock_calls++;
    tp->tv_sec = (time_t)(s_now_ms / 1000);
    tp->tv_nsec = (long)((s_now_ms % 1000) * 1000000);
    return 0;
}

#define clock_gettime test_clock_gettime
#include "../src/prof.c"
#undef clock_gettime

struct capture {
    int saved_fd;
    int read_fd;
};

static int capture_begin(struct capture *cap)
{
    int fds[2];

    if (fflush(stdout) != 0) {
        return -1;
    }
    cap->saved_fd = dup(STDOUT_FILENO);
    if (cap->saved_fd < 0) {
        return -1;
    }
#ifdef _WIN32
    if (_pipe(fds, REPORT_CAP, _O_BINARY) != 0) {
#else
    if (pipe(fds) != 0) {
#endif
        close(cap->saved_fd);
        return -1;
    }
    if (dup2(fds[1], STDOUT_FILENO) < 0) {
        close(fds[0]);
        close(fds[1]);
        close(cap->saved_fd);
        return -1;
    }
    close(fds[1]);
    cap->read_fd = fds[0];
    return 0;
}

static int capture_end(struct capture *cap, char *out, size_t out_size)
{
    size_t used = 0;
    int failed = 0;

    if (fflush(stdout) != 0) {
        failed = 1;
    }
    if (dup2(cap->saved_fd, STDOUT_FILENO) < 0) {
        failed = 1;
    }
    close(cap->saved_fd);
    for (;;) {
        ssize_t got;

        if (used + 1 >= out_size) {
            break;
        }
        got = read(cap->read_fd, out + used, out_size - 1 - used);
        if (got > 0) {
            used += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(cap->read_fd);
    out[used] = '\0';
    return failed ? -1 : 0;
}

static int occurrences(const char *text, const char *token)
{
    int count = 0;
    const char *p = text;

    while ((p = strstr(p, token)) != NULL) {
        count++;
        p++;
    }
    return count;
}

static int has_report_line(const char *text)
{
    const char *p = text;

    while ((p = strstr(p, "prof: a frame")) != NULL) {
        if (p == text || p[-1] == '\n') {
            return 1;
        }
        p++;
    }
    return 0;
}

static int parse_counts(const char *text, double out[TEST_COUNTERS])
{
    static const char *const names[TEST_COUNTERS] = {
        "copies", "pixels", "draws", "upload-bytes"
    };
    const char *p = strstr(text, "prof-counts:");
    const char *line_end;
    int i;

    if (p == NULL || (p != text && p[-1] != '\n')) {
        return -1;
    }
    line_end = strchr(p, '\n');
    if (line_end == NULL) {
        line_end = p + strlen(p);
    }
    if (line_end - p >= COUNTS_LINE_MAX) {
        return -1;
    }
    for (i = 0; i < TEST_COUNTERS; i++) {
        const char *field = strstr(p, names[i]);
        char *end;

        if (field == NULL || field >= line_end) {
            return -1;
        }
        field += strlen(names[i]);
        while (field < line_end && (*field == ' ' || *field == '\t')) {
            field++;
        }
        errno = 0;
        out[i] = strtod(field, &end);
        if (end == field || end > line_end || errno == ERANGE || !isfinite(out[i])) {
            return -1;
        }
        p = end;
    }
    return 0;
}

static int expect_counts(const char *label, const char *text,
                         const double want[TEST_COUNTERS])
{
    double got[TEST_COUNTERS];
    int i, failed = 0;

    if (!has_report_line(text)) {
        fprintf(stderr, "FAIL %s: prof_frame printed no report line\n", label);
        return 1;
    }
    if (occurrences(text, "prof-counts:") != 1) {
        fprintf(stderr, "FAIL %s: expected one prof-counts line, got %d\n",
                label, occurrences(text, "prof-counts:"));
        return 1;
    }
    if (parse_counts(text, got) != 0) {
        fprintf(stderr, "FAIL %s: malformed prof-counts line in:\n%s", label, text);
        return 1;
    }
    for (i = 0; i < TEST_COUNTERS; i++) {
        if (fabs(got[i] - want[i]) > 0.05) {
            fprintf(stderr, "FAIL %s: counter %d is %.3f, want %.3f\n",
                    label, i, got[i], want[i]);
            failed = 1;
        }
    }
    return failed;
}

static int run_window(prof_frame_fn frame, prof_count_fn count,
                      const char *label, const uint64_t amount[TEST_COUNTERS],
                      const double want[TEST_COUNTERS])
{
    struct capture cap;
    char text[REPORT_CAP];
    int i;

    if (capture_begin(&cap) != 0) {
        fprintf(stderr, "ENV %s: cannot redirect stdout\n", label);
        return 2;
    }
    for (i = 0; i < TEST_COUNTERS; i++) {
        if (amount[i] != 0) {
            count(i, amount[i]);
        }
    }
    frame();
    s_now_ms += 2000;
    frame();
    if (capture_end(&cap, text, sizeof(text)) != 0) {
        fprintf(stderr, "ENV %s: cannot restore stdout\n", label);
        return 2;
    }
    return expect_counts(label, text, want);
}

int main(void)
{
    static const double zeros[TEST_COUNTERS] = { 0.0, 0.0, 0.0, 0.0 };
    static const double normal[TEST_COUNTERS] = { 3.0, 3000.0, 5.0, 4.0 };
    static const double large[TEST_COUNTERS] = { 0.0, 0.0, 0.0, 2147483649.0 };
    static const uint64_t normal_amount[TEST_COUNTERS] = { 6, 6000, 10, 8 };
    static const uint64_t large_amount[TEST_COUNTERS] = { 0, 0, 0, UINT64_C(4294967298) };
    static const uint64_t no_amount[TEST_COUNTERS] = { 0, 0, 0, 0 };
    prof_frame_fn frame = prof_frame;
    prof_count_fn count;
    struct capture cap;
    char text[REPORT_CAP];
    int failed, status;

    /* One real report before looking for the counter API. */
    s_now_ms = 10000;
    frame();
    if (capture_begin(&cap) != 0) {
        fprintf(stderr, "ENV: cannot redirect stdout\n");
        return 2;
    }
    s_now_ms = 12000;
    frame();
    if (capture_end(&cap, text, sizeof(text)) != 0) {
        fprintf(stderr, "ENV: cannot restore stdout\n");
        return 2;
    }
    if (s_clock_calls == 0 || !has_report_line(text)) {
        fprintf(stderr, "ENV: prof_frame did not use the clock shim and report\n");
        return 2;
    }

    count = prof_count;

    failed = expect_counts("empty window", text, zeros);

    /* Averages are divided by the two frames in the report window. */
    status = run_window(frame, count, "normal window", normal_amount, normal);
    if (status == 2) {
        return 2;
    }
    failed += status != 0;
    /* 2^32 + 2 over two frames catches a 32-bit counter accumulator. */
    status = run_window(frame, count, "64-bit total", large_amount, large);
    if (status == 2) {
        return 2;
    }
    failed += status != 0;
    /* The report resets the counters, not just the timing totals. */
    status = run_window(frame, count, "reset window", no_amount, zeros);
    if (status == 2) {
        return 2;
    }
    failed += status != 0;

    puts(failed ? "FAIL" : "ok");
    return failed ? 1 : 0;
}
