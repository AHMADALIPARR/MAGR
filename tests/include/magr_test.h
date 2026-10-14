// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 MAGR contributors
//
// This file is part of MAGR (Merkle Array Graph Runtime).
//
// MAGR is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, version 3.
//
// MAGR is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with MAGR. If not, see <https://www.gnu.org/licenses/>.
//
// Note for AGPL section 13: MAGR is designed to be used over a
// network. If you run a modified version as a service, section 13
// requires you to offer those users the corresponding source of
// your modified version. See docs/licensing.md.

/*
 * magr_test.h -- a deliberately tiny test harness.
 *
 * Not a framework. About 80 lines, no dependencies, no build-system magic.
 * The point of this repository's test suite is to be auditable: a claim like
 * "the ABI test passes" should be checkable by reading the assertion, not by
 * trusting a test framework's configuration.
 *
 * Usage:
 *   #include "magr_test.h"
 *   static int t_something(void) { MAGR_CHECK(1 + 1 == 2); return 0; }
 *   int main(void) { return magr_test_run("name", t_something); }
 */

#ifndef MAGR_TEST_H
#define MAGR_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef int (*magr_test_fn)(void);

extern int magr_test_failures;
extern int magr_test_checks;
extern const char *magr_test_current;

static int magr_test_report(int ok, const char *expr, const char *file, int line, const char *detail) {
    magr_test_checks++;
    if (ok) return 0;
    magr_test_failures++;
    fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, expr);
    if (detail != NULL && detail[0] != '\0') fprintf(stderr, "       %s\n", detail);
    return 1;
}

#define MAGR_CHECK(expr) magr_test_report((expr) ? 1 : 0, #expr, __FILE__, __LINE__, "")

#define MAGR_CHECK_MSG(expr, msg) magr_test_report((expr) ? 1 : 0, #expr, __FILE__, __LINE__, (msg))

#define MAGR_CHECK_EQ_I(a, b)                                                       \
    do {                                                                            \
        long long a_ = (long long)(a), b_ = (long long)(b);                          \
        char buf_[128];                                                              \
        snprintf(buf_, sizeof(buf_), "got %lld, expected %lld", a_, b_);              \
        magr_test_report(a_ == b_, #a " == " #b, __FILE__, __LINE__, buf_);          \
    } while (0)

#define MAGR_CHECK_EQ_S(a, b)                                                       \
    do {                                                                            \
        const char *a_ = (a), *b_ = (b);                                             \
        char buf_[256];                                                              \
        snprintf(buf_, sizeof(buf_), "got \"%s\", expected \"%s\"",                \
                 a_ ? a_ : "(null)", b_ ? b_ : "(null)");                            \
        magr_test_report(a_ != NULL && b_ != NULL && strcmp(a_, b_) == 0,           \
                         #a " == " #b, __FILE__, __LINE__, buf_);                   \
    } while (0)

#define MAGR_CHECK_STATUS(expr, expected)                                           \
    do {                                                                            \
        magr_status st_ = (expr);                                                   \
        char buf_[192];                                                              \
        snprintf(buf_, sizeof(buf_), "got %s, expected %s",                         \
                 magr_status_string(st_), magr_status_string(expected));            \
        magr_test_report(st_ == (expected), #expr " == " #expected,                 \
                         __FILE__, __LINE__, buf_);                                 \
    } while (0)

/* Absolute-or-relative tolerance comparison for floating-point results.
 * `rel` is applied to max(|a|,|b|) so that it behaves sensibly near zero. */
#define MAGR_CHECK_NEAR(a, b, rel, abs_tol)                                         \
    do {                                                                            \
        double a_ = (double)(a), b_ = (double)(b);                                   \
        double diff_ = fabs(a_ - b_);                                                \
        double scale_ = fmax(fabs(a_), fabs(b_));                                    \
        char buf_[192];                                                              \
        int ok_ = (diff_ <= (abs_tol)) || (scale_ > 0.0 && diff_ / scale_ <= (rel)); \
        snprintf(buf_, sizeof(buf_), "got %.17g, expected %.17g (diff %.3g)",       \
                 a_, b_, diff_);                                                     \
        magr_test_report(ok_, #a " ~= " #b, __FILE__, __LINE__, buf_);              \
    } while (0)

/* Bitwise equality: use when the contract demands bit-identity, e.g. to assert
 * that +0.0 and -0.0 are distinguished by a raw payload hash. */
#define MAGR_CHECK_BITS_F64(a, b)                                                   \
    do {                                                                            \
        double a_ = (double)(a), b_ = (double)(b);                                   \
        uint64_t ba_, bb_;                                                           \
        memcpy(&ba_, &a_, 8);                                                        \
        memcpy(&bb_, &b_, 8);                                                        \
        char buf_[192];                                                              \
        snprintf(buf_, sizeof(buf_), "got 0x%016llx, expected 0x%016llx",           \
                 (unsigned long long)ba_, (unsigned long long)bb_);                 \
        magr_test_report(ba_ == bb_, #a " bitwise == " #b, __FILE__, __LINE__, buf_); \
    } while (0)

static int magr_test_run(const char *name, magr_test_fn fn) {
    int before = magr_test_failures;
    magr_test_current = name;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", (magr_test_failures == before) ? "  OK " : "FAIL", name);
    return magr_test_failures - before;
}

#endif /* MAGR_TEST_H */