#ifndef FLEXPTP_LINUX_NSD_LINUX_H
#define FLEXPTP_LINUX_NSD_LINUX_H

#include <stdbool.h>
#include <stdint.h>

#include "../../timeutils.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Select the software-timestamp mode, before linux_nsd_preinit(). The driver
 * then asks the kernel for software timestamps (CLOCK_REALTIME) and keeps a
 * virtual clock in place of a PHC:
 *
 *     v = base + (rt - t0) * (1 + (drift + tuning) / 1e9)
 *
 * where rt is CLOCK_REALTIME, drift the free-running error given here and
 * tuning the servo's. Every timestamp the kernel delivers is converted to the
 * virtual clock before it reaches the core. Meant for interfaces without a
 * PHC, such as veth pairs in tests.
 *
 * @param initial_offset_ns the virtual clock's offset from CLOCK_REALTIME at start
 * @param drift_ppb its free-running frequency error
 */
void linux_nsd_set_sw_mode(int64_t initial_offset_ns, double drift_ppb);

/**
 * Set the time a transmit timestamp may take to come back (20 ms by
 * default). A message whose timestamp has not come back by then is reported
 * lost to the core (ptp_transmit_timestamp_lost()).
 *
 * @param timeout_ms timeout in milliseconds, at least 1
 */
void linux_nsd_set_txts_timeout_ms(uint32_t timeout_ms);

/**
 * The transmit timestamp timeout in effect.
 *
 * @return timeout in milliseconds
 */
uint32_t linux_nsd_get_txts_timeout_ms(void);

/**
 * Run the transceiver thread at a SCHED_FIFO priority, from its next start
 * (set it before linux_nsd_preinit()).
 *
 * @param priority SCHED_FIFO priority, 0 for the default policy
 */
void linux_nsd_set_rt_priority(int priority);

/**
 * Run the PTP task thread at a SCHED_FIFO priority (implemented in
 * task_ptp.c; call it after reg_task_ptp()).
 *
 * @param priority SCHED_FIFO priority, 0 for the default policy
 * @return true if applied
 */
bool ptp_task_set_rt_priority(int priority);

/**
 * In software-timestamp mode, the virtual clock minus CLOCK_REALTIME, now.
 *
 * @return the difference in nanoseconds, 0 outside the software-timestamp mode
 */
int64_t linux_nsd_vclock_error_ns(void);

/**
 * Preinitialize the Linux network stack driver.
 *
 * @param ifn name of the targeted network interface (e.g. eth0)
 *
 * @return initialization successful
 */
bool linux_nsd_preinit(const char * ifn);

/**
 * Clean up the Linux network stack driver.
 */
void linux_nsd_cleanup(void);

/**
 * Set PHC time.
 *
 * @param seconds seconds part
 * @param nanoseconds nanoseconds part
 */
void linux_set_time(uint32_t seconds, uint32_t nanoseconds);

/**
 * Adjust the PHC.
 *
 * @param tuning_ppb clock adjustment
 */
void linux_adjust_clock(double tuning_ppb);

/**
 * Query PHC time.
 *
 * @param pTime pointer to a timestamp object
 */
void linux_get_time(TimestampU * pTime);

#ifdef __cplusplus
}
#endif

#endif // FLEXPTP_LINUX_NSD_LINUX_H
