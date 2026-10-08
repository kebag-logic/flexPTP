/**
 ******************************************************************************
 * @file    flexptpd_print.h
 * @copyright Kebag-Logic, 2026
 * @brief   flexptpd's printing: everything flexPTP and the CLI print goes
 * through flexptpd_printf(), which drops the ANSI colour sequences unless
 * stdout is a terminal.
 ******************************************************************************
 */

#ifndef FLEXPTPD_PRINT_H
#define FLEXPTPD_PRINT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Turn the colours on or off (off: the ANSI sequences are dropped).
 *
 * @param on keep the colours
 */
void flexptpd_print_set_color(bool on);

/**
 * printf() to stdout, with or without the colours.
 *
 * @param fmt format
 * @return the number of characters written
 */
int flexptpd_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif // FLEXPTPD_PRINT_H
