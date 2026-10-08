/**
 ******************************************************************************
 * @file    gptp_status.h
 * @copyright Kebag-Logic, 2026
 * @brief   A snapshot of the IEEE 802.1AS (gPTP) state of the port, for
 * other threads or processes: the grandmaster, asCapable, the link delay,
 * the rate ratios, the servo's time error, and the reverse sync.
 *
 * The core refreshes the snapshot on every heartbeat and on every state
 * change; ptp_gptp_get_status() copies it out, from any thread.
 ******************************************************************************
 */

#ifndef FLEXPTP_GPTP_STATUS_H_
#define FLEXPTP_GPTP_STATUS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTP_GPTP_STATUS_VERSION (1) ///< bumped on any layout change of PtpGptpStatus

/**
 * @brief The gPTP state of the port. Identities are in host order: the
 * 64-bit value whose big-endian bytes are the clockIdentity on the wire.
 */
typedef struct {
    uint32_t version; ///< PTP_GPTP_STATUS_VERSION
    uint32_t updates; ///< incremented on every refresh

    // ---- port ----
    uint8_t portState;        ///< PtpBmcaFsmState
    uint8_t linkUp;           ///< the link (carrier) is up
    uint8_t asCapable;        ///< 802.1AS asCapable
    uint8_t isMeasuringDelay; ///< the peer delay mechanism is measuring
    uint8_t domain;           ///< domainNumber
    uint8_t gmPresent;        ///< a grandmaster is known (us included)
    uint8_t locked;           ///< the servo holds the time error within the lock limit
    uint8_t _r0;

    uint64_t ownIdentity; ///< our clockIdentity

    // ---- grandmaster ----
    uint64_t gmIdentity;          ///< grandmaster clockIdentity, 0 when none
    uint8_t gmPriority1;          ///< grandmaster priority1
    uint8_t gmClockClass;         ///< grandmaster clockClass
    uint8_t gmClockAccuracy;      ///< grandmaster clockAccuracy
    uint8_t gmPriority2;          ///< grandmaster priority2
    uint16_t gmVariance;          ///< grandmaster offsetScaledLogVariance
    uint16_t stepsRemoved;        ///< stepsRemoved of the grandmaster's Announce, 0 when we are it
    uint16_t gmTimeBaseIndicator; ///< from the last Follow_Up information TLV
    uint16_t _r1;
    uint32_t gmChanges; ///< number of grandmaster changes since start

    // ---- link and time ----
    int64_t meanLinkDelay_ns;  ///< neighborPropDelay
    double neighborRateRatio;  ///< neighbour's clock / ours
    double rateRatio;          ///< grandmaster's clock / ours
    int64_t timeError_ns;      ///< the servo's last input (ours - grandmaster's)
    double tuning_ppb;         ///< the clock's frequency tuning
    int8_t logSyncInterval;    ///< currentLogSyncInterval
    int8_t logPdelayInterval;  ///< currentLogPdelayReqInterval
    int8_t logAnnounceInterval; ///< currentLogAnnounceInterval
    uint8_t _r2;

    // ---- counters ----
    uint32_t syncRx;            ///< Sync/Follow_Up pairs used
    uint32_t syncTimeouts;      ///< sync receipt timeouts
    uint32_t announceRx;        ///< Announce messages accepted
    uint32_t announceTimeouts;  ///< announce receipt timeouts
    uint32_t pdelayLost;        ///< Pdelay_Req without a complete response
    uint32_t pdelayMultiple;    ///< Pdelay_Req answered by more than one peer
    uint32_t signalingRx;       ///< message interval requests applied
    uint32_t txTimestampsLost;  ///< transmit timestamps that never arrived

    // ---- reverse sync ----
    uint8_t rsyncEnabled;      ///< reverse sync is on
    uint8_t rsyncDomain;       ///< its domainNumber
    uint16_t _r3;
    uint32_t rsyncInterval_us; ///< its transmission interval
    uint32_t rsyncSent;        ///< reverse Sync/Follow_Up pairs sent
} PtpGptpStatus;

/**
 * Copy out the latest snapshot. Callable from any thread.
 *
 * @param pStatus where to copy it
 */
void ptp_gptp_get_status(PtpGptpStatus *pStatus);

#ifdef __cplusplus
}
#endif

#endif /* FLEXPTP_GPTP_STATUS_H_ */
