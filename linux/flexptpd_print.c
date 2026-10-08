// SPDX-License-Identifier: MIT
// Copyright Kebag-Logic, 2026
//
// flexptpd_print.c - see flexptpd_print.h.

#include "flexptpd_print.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static bool sColor = true;

void flexptpd_print_set_color(bool on) {
    sColor = on;
}

/**
 * Remove the ANSI CSI sequences (ESC '[' ... final byte) in place.
 */
static size_t strip_ansi(char *s, size_t len) {
    size_t out = 0;
    size_t i = 0;

    while (i < len) {
        if ((s[i] == '\x1b') && (i + 1 < len) && (s[i + 1] == '[')) {
            // skip the parameters up to the final byte (0x40 to 0x7E)
            i += 2;
            while ((i < len) && !((s[i] >= 0x40) && (s[i] <= 0x7E))) {
                i++;
            }

            i++;
            continue;
        }

        s[out] = s[i];
        out++;
        i++;
    }

    return out;
}

int flexptpd_printf(const char *fmt, ...) {
    char small[1024];
    char *buf = small;

    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);

    if (len < 0) {
        return len;
    }

    // a long line: format it again into a buffer of its size
    if ((size_t)len >= sizeof(small)) {
        buf = malloc((size_t)len + 1);
        if (buf == NULL) {
            return -1;
        }

        va_start(ap, fmt);
        vsnprintf(buf, (size_t)len + 1, fmt, ap);
        va_end(ap);
    }

    size_t n = (size_t)len;
    if (!sColor) {
        n = strip_ansi(buf, n);
    }

    fwrite(buf, 1, n, stdout);

    if (buf != small) {
        free(buf);
    }

    return (int)n;
}
