/* (C) Kebag-Logic, 2026 */

// Signaling of IEEE 802.1AS-2020 10.6.4: the message interval request TLV
// sets the port's own transmit intervals. -128 keeps an interval, 126 puts
// it back to its initial value, 127 stops the transmission. The flags say
// whether the neighborRateRatio and the neighborPropDelay are still to be
// computed.

#include "gptp.h"

#include <string.h>

#include "master.h"
#include "minmax.h"
#include "ptp_core.h"
#include "ptp_defs.h"

///\cond 0
#define S (gPtpCoreState)
///\endcond

#define MSG_INTERVAL_REQUEST_SUBTYPE (2) ///< organizationSubType of the message interval request TLV
#define MSG_INTERVAL_REQUEST_LEN (12)    ///< its lengthField

#define FLAG_COMPUTE_NRR (0x01)   ///< computeNeighborRateRatio
#define FLAG_COMPUTE_DELAY (0x02) ///< computeNeighborPropDelay

#define LOG_INTERVAL_MIN (-7) ///< the shortest interval accepted (7.8 ms)
#define LOG_INTERVAL_MAX (8)  ///< the longest interval accepted (256 s)

/**
 * Apply one requested interval.
 *
 * @param pCurrent the current log interval
 * @param pStopped whether the transmission is stopped
 * @param requested the requested value
 * @param initial the initial log interval
 */
static void apply_interval(int8_t *pCurrent, bool *pStopped, int8_t requested, int8_t initial) {
    if (requested == PTP_GPTP_INTERVAL_NO_CHANGE) {
        return;
    }

    if (requested == PTP_GPTP_INTERVAL_INITIAL) {
        *pCurrent = initial;
        *pStopped = false;
        return;
    }

    if (requested == PTP_GPTP_INTERVAL_STOP) {
        *pStopped = true;
        return;
    }

    // outside what the port can keep: ignored
    if ((requested < LOG_INTERVAL_MIN) || (requested > LOG_INTERVAL_MAX)) {
        return;
    }

    *pCurrent = requested;
    *pStopped = false;
}

void ptp_gptp_signaling_rx(const RawPtpMessage *pRawMsg, const PtpHeader *pHeader) {
    const uint8_t *data = pRawMsg->data;
    uint32_t size = MIN(pHeader->messageLength, pRawMsg->size);

    if (size < PTP_HEADER_LENGTH + PTP_PORT_ID_LENGTH) {
        return;
    }

    // targetPortIdentity: this port, or every port (all ones)
    const uint8_t *target = data + PTP_HEADER_LENGTH;

    uint64_t targetClock;
    memcpy(&targetClock, target, 8);
    uint16_t targetPort = ptp_gptp_be16(target + 8);

    bool clockMatches = (targetClock == UINT64_MAX) || (targetClock == S.hwoptions.clockIdentity);
    bool portMatches = (targetPort == 0xFFFF) || (targetPort == PTP_PORT_ID);

    if (!clockMatches || !portMatches) {
        return;
    }

    uint16_t len = 0;
    const uint8_t *tlv = ptp_gptp_find_tlv(data,
                                           size,
                                           PTP_HEADER_LENGTH + PTP_PORT_ID_LENGTH,
                                           PTP_TLV_ORGANIZATION_EXTENSION,
                                           MSG_INTERVAL_REQUEST_SUBTYPE,
                                           &len);

    if ((tlv == NULL) || (len != MSG_INTERVAL_REQUEST_LEN)) {
        return;
    }

    int8_t linkDelayInterval = (int8_t)tlv[10];
    int8_t timeSyncInterval = (int8_t)tlv[11];
    int8_t announceInterval = (int8_t)tlv[12];
    uint8_t flags = tlv[13];

    int8_t initialPdelay = (S.profile.logDelayReqPeriod == PTP_LOGPER_SYNCMATCHED) ? 0 : S.profile.logDelayReqPeriod;

    apply_interval(&S.gptp.logPdelay, &S.gptp.pdelayStopped, linkDelayInterval, initialPdelay);
    apply_interval(&S.gptp.logSync, &S.gptp.syncStopped, timeSyncInterval, S.profile.logSyncPeriod);
    apply_interval(&S.gptp.logAnnounce, &S.gptp.announceStopped, announceInterval, S.profile.logAnnouncePeriod);

    S.gptp.computeNrr = (flags & FLAG_COMPUTE_NRR) != 0;
    S.gptp.computeDelay = (flags & FLAG_COMPUTE_DELAY) != 0;

    S.gptp.signalingRx++;

    CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
    CLILOG(S.logging.info, "message interval request: pdelay %d%s, sync %d%s, announce %d%s, flags 0x%02x\n",
           S.gptp.logPdelay, S.gptp.pdelayStopped ? " (stopped)" : "",
           S.gptp.logSync, S.gptp.syncStopped ? " (stopped)" : "",
           S.gptp.logAnnounce, S.gptp.announceStopped ? " (stopped)" : "",
           flags);

    // the master's schedule follows, and so does a reverse sync paced on our own Sync interval
    ptp_master_update_intervals();
    ptp_gptp_rsync_update();
    ptp_gptp_publish_status();
}
