/**
 ******************************************************************************
 * @file    gptp_settings.h
 * @copyright Kebag-Logic, 2026
 * @brief   Settings of the IEEE 802.1AS (gPTP) layer and of the reverse sync.
 *
 * Callable from any thread: a setter stores the value and has the core apply
 * it in its own context. The settings survive a core reset.
 ******************************************************************************
 */

#ifndef FLEXPTP_GPTP_SETTINGS_H_
#define FLEXPTP_GPTP_SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set neighborPropDelayThresh: asCapable needs a link delay at most this.
 *
 * @param ns threshold in nanoseconds
 */
void ptp_gptp_set_neighbor_prop_delay_thresh(int64_t ns);

/**
 * @return neighborPropDelayThresh in nanoseconds
 */
int64_t ptp_gptp_get_neighbor_prop_delay_thresh(void);

/**
 * Set allowedLostResponses: consecutive unanswered Pdelay_Req tolerated
 * before asCapable drops.
 *
 * @param n number of requests
 */
void ptp_gptp_set_allowed_lost_responses(uint16_t n);

/**
 * @return allowedLostResponses
 */
uint16_t ptp_gptp_get_allowed_lost_responses(void);

/**
 * Allow or forbid the grandmaster role. The build must allow master
 * operation (PTP_ENABLE_MASTER_OPERATION), and the profile must not be
 * slave-only, for the role to be taken.
 *
 * @param en true to allow it
 */
void ptp_gptp_set_gm_capable(bool en);

/**
 * @return true if the grandmaster role is allowed
 */
bool ptp_gptp_get_gm_capable(void);

/**
 * Turn reverse sync (reverseSyncEnabled) on or off.
 *
 * @param en true to turn it on
 */
void ptp_rsync_enable(bool en);

/**
 * @return true if reverse sync is on
 */
bool ptp_rsync_is_enabled(void);

/**
 * Set reverseSyncDomain. It must differ from the measured domain.
 *
 * @param domain domainNumber of the reverse Sync/Follow_Up
 * @return false (and no change) if it is the measured domain
 */
bool ptp_rsync_set_domain(uint8_t domain);

/**
 * @return reverseSyncDomain
 */
uint8_t ptp_rsync_get_domain(void);

/**
 * Set reverseSyncRate: the interval as a percentage of the incoming Sync
 * interval, and drop any fixed interval.
 *
 * @param pct percentage, 10 to 1000
 */
void ptp_rsync_set_rate_pct(uint16_t pct);

/**
 * @return reverseSyncRate as a percentage
 */
uint16_t ptp_rsync_get_rate_pct(void);

/**
 * Set a fixed interval instead of reverseSyncRate.
 *
 * @param ms interval in milliseconds, 0 to use reverseSyncRate again
 */
void ptp_rsync_set_interval_ms(uint32_t ms);

/**
 * @return the fixed interval in milliseconds, 0 if reverseSyncRate applies
 */
uint32_t ptp_rsync_get_interval_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* FLEXPTP_GPTP_SETTINGS_H_ */
