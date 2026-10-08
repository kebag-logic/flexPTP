/**
 ******************************************************************************
 * @file    gptp.h
 * @copyright Kebag-Logic, 2026
 * @brief   The IEEE 802.1AS (gPTP) time-aware end station layer of the core:
 * the link state, the peer delay mechanism and asCapable, the 802.1AS BMCA,
 * the reverse sync, and the status snapshot.
 ******************************************************************************
 */

#ifndef FLEXPTP_GPTP_H_
#define FLEXPTP_GPTP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the gPTP layer (once, at core initialization).
 */
void ptp_gptp_init(void);

/**
 * Reset the gPTP layer (on every core reset).
 */
void ptp_gptp_reset(void);

/**
 * Advance the gPTP layer by one heartbeat tick.
 */
void ptp_gptp_tick(void);

/**
 * The link went up or down.
 *
 * @param up true if the link is up
 */
void ptp_gptp_link_state(bool up);

/**
 * The auxiliary timer has expired.
 */
void ptp_gptp_aux_timer(void);

/**
 * Refresh the status snapshot that ptp_gptp_get_status() hands out.
 */
void ptp_gptp_publish_status(void);

#ifdef __cplusplus
}
#endif

#endif /* FLEXPTP_GPTP_H_ */
