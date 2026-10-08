/**
 ******************************************************************************
 * @file    gptp.h
 * @copyright Kebag-Logic, 2026
 * @brief   The IEEE 802.1AS (gPTP) time-aware end station layer of the core:
 * the link state, the peer delay mechanism and asCapable, the 802.1AS BMCA,
 * signaling, the reverse sync, and the status snapshot.
 *
 * It runs when the loaded profile is an 802.1AS one (majorSdoId 1, the
 * peer-to-peer delay mechanism, the 802.3 transport): the "gPTP" preset.
 * Every other profile keeps flexPTP's IEEE 1588 behaviour.
 *
 * Everything here runs in the core's context (the PTP task), except where a
 * function says otherwise.
 ******************************************************************************
 */

#ifndef FLEXPTP_GPTP_H_
#define FLEXPTP_GPTP_H_

#include <stdbool.h>
#include <stdint.h>

#include "ptp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- defaults (IEEE 802.1AS-2020 and Milan) ----

#ifndef PTP_GPTP_NEIGHBOR_PROP_DELAY_THRESH_NS
#define PTP_GPTP_NEIGHBOR_PROP_DELAY_THRESH_NS (800) ///< neighborPropDelayThresh
#endif

#ifndef PTP_GPTP_ALLOWED_LOST_RESPONSES
#define PTP_GPTP_ALLOWED_LOST_RESPONSES (9) ///< allowedLostResponses
#endif

#ifndef PTP_GPTP_ANNOUNCE_RECEIPT_TIMEOUT
#define PTP_GPTP_ANNOUNCE_RECEIPT_TIMEOUT (3) ///< announceReceiptTimeout, in announce intervals
#endif

#ifndef PTP_GPTP_SYNC_RECEIPT_TIMEOUT
#define PTP_GPTP_SYNC_RECEIPT_TIMEOUT (3) ///< syncReceiptTimeout, in sync intervals
#endif

#ifndef PTP_GPTP_RSYNC_DOMAIN
#define PTP_GPTP_RSYNC_DOMAIN (1) ///< default reverseSyncDomain
#endif

#ifndef PTP_GPTP_RSYNC_RATE_PCT
#define PTP_GPTP_RSYNC_RATE_PCT (90) ///< default reverseSyncRate: 90 % of the incoming Sync interval
#endif

#define PTP_GPTP_MINOR_VERSION (1) ///< minorVersionPTP of IEEE 802.1AS-2020

// ---- message interval request values (IEEE 802.1AS-2020 10.6.4.3) ----

#define PTP_GPTP_INTERVAL_NO_CHANGE (-128) ///< keep the current interval
#define PTP_GPTP_INTERVAL_INITIAL (126)    ///< back to the initial interval
#define PTP_GPTP_INTERVAL_STOP (127)       ///< stop sending

// ---- gptp.c: the layer ----

/**
 * Initialize the gPTP layer (once, at core initialization): defaults of the
 * configuration.
 */
void ptp_gptp_init(void);

/**
 * Whether the loaded profile is an 802.1AS one. Unlike S.gptp.enabled, it is
 * right also during a core reset, before the gPTP layer's own reset.
 *
 * @return true for majorSdoId 1, the peer-to-peer delay mechanism and 802.3
 */
bool ptp_gptp_profile_is_gptp(void);

/**
 * Reset the gPTP layer (on every core reset): whether the profile is an
 * 802.1AS one, the initial intervals, and all dynamic state. The
 * configuration and the link state are kept.
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
 * Apply settings changed from another context (PTP_CEV_GPTP_APPLY).
 */
void ptp_gptp_apply(void);

/**
 * Process a received message in 802.1AS mode.
 *
 * @param pRawMsg the message, with its reception timestamp
 * @param pHeader its extracted header
 */
void ptp_gptp_process_packet(RawPtpMessage *pRawMsg, PtpHeader *pHeader);

/**
 * Change the port state, as the BMCA decides it: log it, and let the core
 * enable the slave or the master module.
 *
 * @param state the new state
 */
void ptp_gptp_set_port_state(PtpBmcaFsmState state);

/**
 * Number of heartbeat ticks in a logarithmic message interval, at least one.
 *
 * @param logInterval log2 of the interval in seconds
 * @return the interval in ticks
 */
uint32_t ptp_gptp_log_to_ticks(int8_t logInterval);

/**
 * A logarithmic message interval in microseconds.
 *
 * @param logInterval log2 of the interval in seconds
 * @return the interval in microseconds
 */
uint32_t ptp_gptp_log_to_us(int8_t logInterval);

/**
 * Refresh the status snapshot that ptp_gptp_get_status() hands out.
 */
void ptp_gptp_publish_status(void);

// ---- gptp_pdelay.c: the peer delay mechanism ----

/**
 * Reset the peer delay mechanism: nothing in flight, nothing measured, not
 * asCapable.
 */
void ptp_gptp_pdelay_reset(void);

/**
 * Advance the peer delay mechanism by one tick: send a Pdelay_Req when due,
 * and count the one before as lost if it was not answered.
 */
void ptp_gptp_pdelay_tick(void);

/**
 * Decide asCapable again, after its thresholds changed.
 */
void ptp_gptp_pdelay_judge(void);

/**
 * Process a Pdelay_Resp or a Pdelay_Resp_Follow_Up.
 *
 * @param pRawMsg the message
 * @param pHeader its header
 */
void ptp_gptp_pdelay_rx(const RawPtpMessage *pRawMsg, const PtpHeader *pHeader);

// ---- gptp_bmca.c: the BMCA ----

/**
 * Reset the BMCA: no master information.
 */
void ptp_gptp_bmca_reset(void);

/**
 * Advance the BMCA by one tick: the receipt timeouts, and the LISTENING
 * timeout after which a grandmaster-capable end station takes the role.
 */
void ptp_gptp_bmca_tick(void);

/**
 * Reselect the port's role from the link, asCapable and the current master
 * information.
 */
void ptp_gptp_bmca_reselect(void);

/**
 * Process an Announce.
 *
 * @param pRawMsg the message
 * @param pHeader its header
 */
void ptp_gptp_bmca_announce_rx(const RawPtpMessage *pRawMsg, const PtpHeader *pHeader);

/**
 * Process a Sync or a Follow_Up: pass it to the slave if it comes from the
 * selected master's port, while the port is a slave.
 *
 * @param pRawMsg the message
 * @param pHeader its header
 */
void ptp_gptp_bmca_sync_rx(RawPtpMessage *pRawMsg, PtpHeader *pHeader);

/**
 * Whether we are the grandmaster: the port is a master with no better master
 * information.
 *
 * @return true if we are
 */
bool ptp_gptp_bmca_is_gm(void);

// ---- gptp_signaling.c: message interval requests ----

/**
 * Process a Signaling message.
 *
 * @param pRawMsg the message
 * @param pHeader its header
 */
void ptp_gptp_signaling_rx(const RawPtpMessage *pRawMsg, const PtpHeader *pHeader);

// ---- gptp_rsync.c: reverse sync ----

/**
 * Start, retime or stop the reverse sync timer after anything it depends on
 * changed: the settings, the link, the incoming Sync interval.
 */
void ptp_gptp_rsync_update(void);

/**
 * Send one reverse Sync (its Follow_Up follows the egress timestamp).
 */
void ptp_gptp_rsync_send(void);

// ---- gptp_msg.c: message helpers ----

/**
 * Fill a header for an 802.1AS message sent by this port: majorSdoId 1,
 * versionPTP 2, minorVersionPTP 1, correctionField 0, our port identity.
 *
 * @param pHeader the header to fill
 * @param type messageType
 * @param length messageLength
 * @param domain domainNumber
 * @param seq sequenceId
 * @param logInterval logMessageInterval
 */
void ptp_gptp_fill_header(PtpHeader *pHeader, PtpMessageType type, uint16_t length, uint8_t domain, uint16_t seq, int8_t logInterval);

/**
 * Find a TLV in a message.
 *
 * @param pData the message
 * @param size its size
 * @param from offset of the first TLV
 * @param type tlvType
 * @param orgSubType for an ORGANIZATION_EXTENSION with organizationId 00-80-C2,
 *        its organizationSubType; -1 for any other type
 * @param pLen where to store the TLV's lengthField
 * @return pointer to the TLV (its tlvType), NULL if absent or malformed
 */
const uint8_t *ptp_gptp_find_tlv(const uint8_t *pData, uint32_t size, uint32_t from, uint16_t type, int32_t orgSubType, uint16_t *pLen);

/**
 * Write the Follow_Up information TLV (32 bytes).
 *
 * @param p where to write it
 * @param pInfo its contents
 */
void ptp_gptp_write_follow_up_info(uint8_t *p, const PtpGptpFollowUpInfo *pInfo);

/**
 * Read the Follow_Up information TLV of a Follow_Up.
 *
 * @param pData the Follow_Up
 * @param size its size
 * @param pInfo where to store the contents
 * @return false if the Follow_Up carries none
 */
bool ptp_gptp_read_follow_up_info(const uint8_t *pData, uint32_t size, PtpGptpFollowUpInfo *pInfo);

/**
 * Read a big-endian 16-bit field.
 */
uint16_t ptp_gptp_be16(const uint8_t *p);

/**
 * Read a big-endian 32-bit field.
 */
uint32_t ptp_gptp_be32(const uint8_t *p);

#define PTP_GPTP_FOLLOW_UP_INFO_TLV_LEN (32) ///< Follow_Up information TLV, header included

#ifdef __cplusplus
}
#endif

#endif /* FLEXPTP_GPTP_H_ */
