/**
 ******************************************************************************
 * @file    flexptp_options.h
 * @copyright Kebag-Logic, 2026
 * @brief   flexPTP build options for flexptpd, the Linux daemon: the PHC and
 * hardware timestamps of the Linux network stack driver, the Kalman servo,
 * and the defaults of an IEEE 802.1AS end station.
 ******************************************************************************
 */

#ifndef FLEXPTP_OPTIONS_H
#define FLEXPTP_OPTIONS_H

// ---- platform ----

#define FLEXPTP_LINUX
#define PTP_HLT_INTERFACE

#define PTP_HEARTBEAT_TICKRATE_MS (31)

// ---- printing ----

#include <stdio.h>

#define MSG(...) printf(__VA_ARGS__)

#define CLILOG(en, ...)          \
    {                            \
        if (en)                  \
            printf(__VA_ARGS__); \
    }

#define FLEXPTP_SNPRINTF(...) snprintf(__VA_ARGS__)

// ---- hardware clock: the PHC behind the interface ----

#include <stdlib.h>

#include <flexptp/port/example_netstack_drivers/nsd_linux.h>

extern char ptp_if_name[];

#define PTP_HW_INIT()                        \
    if (!linux_nsd_preinit(ptp_if_name)) {   \
        exit(1);                             \
    }

#define PTP_SET_CLOCK(s, ns) linux_set_time((s), (ns))
#define PTP_SET_TUNING(tuning_ppb) linux_adjust_clock(tuning_ppb)
#define PTP_HW_GET_TIME(pt) linux_get_time(pt)

// ---- servo ----

#include <flexptp/servo/kalman_filter.h>

#define PTP_SERVO_INIT() kalman_filter_init()
#define PTP_SERVO_DEINIT() kalman_filter_deinit()
#define PTP_SERVO_RESET() kalman_filter_reset()
#define PTP_SERVO_RUN(d, pscd) kalman_filter_run(d, pscd)

// ---- clock dataset: an 802.1AS end station that may become grandmaster ----

#define PTP_ENABLE_MASTER_OPERATION (1)

#define PTP_CLOCK_PRIORITY1 (248)
#define PTP_CLOCK_PRIORITY2 (248)
#define PTP_BEST_CLOCK_CLASS (PTP_CC_DEFAULT)
#define PTP_WORST_ACCURACY (PTP_CA_UNKNOWN)
#define PTP_TIME_SOURCE (PTP_TSRC_INTERNAL_OSCILLATOR)

// ---- user events ----

#include <flexptp/event.h>

extern void flexptpd_user_event_cb(PtpUserEventCode uev);

#define PTP_USER_EVENT_CALLBACK flexptpd_user_event_cb

#endif // FLEXPTP_OPTIONS_H
