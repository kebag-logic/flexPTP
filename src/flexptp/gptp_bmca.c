/* (C) Kebag-Logic, 2026 */

// The BMCA of a one-port IEEE 802.1AS time-aware end station (IEEE
// 802.1AS-2020 clause 10). The port is:
//
//   DISABLED   without a link, or while it is not asCapable;
//   SLAVE      when the best master information received beats our own
//              systemIdentity;
//   MASTER     when it does not and we may be grandmaster: we are the
//              grandmaster;
//   LISTENING  otherwise, and for announceReceiptTimeout after the port comes
//              up, before it takes the grandmaster role.
//
// The master information ages out on announceReceiptTimeout or
// syncReceiptTimeout. Sync and Follow_Up count only from the port that sent
// the selected Announce.

#include "gptp.h"

#include <string.h>

#include "event.h"
#include "format_utils.h"
#include "minmax.h"
#include "msg_utils.h"
#include "ptp_core.h"
#include "ptp_defs.h"
#include "slave.h"

///\cond 0
#define S (gPtpCoreState)
#define B (gPtpCoreState.gptp.bmca)
///\endcond

#define STEPS_REMOVED_MAX (255) ///< an Announce with stepsRemoved this high is discarded

// ---- priority vectors ----

/**
 * Compare two clockIdentities kept in wire order, as the numbers they are.
 */
static int compare_identities(uint64_t a, uint64_t b) {
    uint64_t ha = FLEXPTP_ntohll(a);
    uint64_t hb = FLEXPTP_ntohll(b);

    if (ha == hb) {
        return 0;
    }

    return (ha < hb) ? -1 : 1;
}

#define COMPARE_FIELD(a, b)                \
    if ((a) != (b)) {                      \
        return ((a) < (b)) ? -1 : 1;       \
    }

/**
 * IEEE 802.1AS-2020 10.3.5: the lower vector is the better one.
 *
 * @return <0 if a is better than b, >0 if worse, 0 if equal
 */
static int compare_vectors(const PtpGptpPriorityVector *a, const PtpGptpPriorityVector *b) {
    // the grandmaster's systemIdentity
    COMPARE_FIELD(a->priority1, b->priority1);
    COMPARE_FIELD(a->clockClass, b->clockClass);
    COMPARE_FIELD(a->clockAccuracy, b->clockAccuracy);
    COMPARE_FIELD(a->variance, b->variance);
    COMPARE_FIELD(a->priority2, b->priority2);

    int c = compare_identities(a->gmIdentity, b->gmIdentity);
    if (c != 0) {
        return c;
    }

    // then the path to it
    COMPARE_FIELD(a->stepsRemoved, b->stepsRemoved);

    c = compare_identities(a->senderIdentity, b->senderIdentity);
    if (c != 0) {
        return c;
    }

    COMPARE_FIELD(a->senderPort, b->senderPort);

    return 0;
}

/**
 * Our own systemIdentity, as the vector we would announce as grandmaster.
 */
static void own_vector(PtpGptpPriorityVector *v) {
    const PtpMasterProperties *c = &S.capabilities;

    v->priority1 = c->priority1;
    v->clockClass = c->grandmasterClockClass;
    v->clockAccuracy = c->grandmasterClockAccuracy;
    v->variance = c->grandmasterClockVariance;
    v->priority2 = c->priority2;
    v->gmIdentity = S.hwoptions.clockIdentity;
    v->stepsRemoved = 0;
    v->senderIdentity = S.hwoptions.clockIdentity;
    v->senderPort = PTP_PORT_ID;
}

/**
 * May we be grandmaster: allowed by the settings, the build and the profile,
 * and priority1 not 255 (IEEE 802.1AS-2020 8.6.2.1).
 */
static bool gm_capable(void) {
    return S.gptp.gmCapable &&
           PTP_ENABLE_MASTER_OPERATION &&
           !(S.profile.flags & PTP_PF_SLAVE_ONLY) &&
           (S.capabilities.priority1 != 255);
}

/**
 * announceReceiptTimeout of our own announce interval: how long a port that
 * just came up listens before it takes the grandmaster role.
 */
static uint32_t listen_ticks(void) {
    return PTP_GPTP_ANNOUNCE_RECEIPT_TIMEOUT * ptp_gptp_log_to_ticks(S.gptp.logAnnounce);
}

// ---- the grandmaster ----

static void set_grandmaster(uint64_t gm, const PtpGptpPriorityVector *v) {
    if (gm != B.gmIdentity) {
        B.gmIdentity = gm;
        B.gmChanges++;

        CLILOG(S.logging.logid && S.logging.bmca, "[LOG-BMCA] ");
        CLILOG(S.logging.bmca, "grandmaster %02x%02x%02x.%02x%02x.%02x%02x%02x\n",
               ((uint8_t *)&gm)[0], ((uint8_t *)&gm)[1], ((uint8_t *)&gm)[2], ((uint8_t *)&gm)[3],
               ((uint8_t *)&gm)[4], ((uint8_t *)&gm)[5], ((uint8_t *)&gm)[6], ((uint8_t *)&gm)[7]);
    }

    // the core's own view of the master: the servo's lock statistics and the CLI use it
    PtpMasterProperties *mp = &S.bmca.masterProps;

    if (gm == 0) {
        memset(mp, 0, sizeof(PtpMasterProperties));
        return;
    }

    mp->priority1 = v->priority1;
    mp->grandmasterClockClass = v->clockClass;
    mp->grandmasterClockAccuracy = v->clockAccuracy;
    mp->grandmasterClockVariance = v->variance;
    mp->priority2 = v->priority2;
    mp->grandmasterClockIdentity = gm;
    mp->localStepsRemoved = v->stepsRemoved;
    mp->currentUTCOffset = B.infoValid ? B.utcOffset : S.capabilities.currentUTCOffset;
    mp->timeSource = B.infoValid ? B.timeSource : S.capabilities.timeSource;
}

// ---- role selection ----

void ptp_gptp_bmca_reselect(void) {
    PtpBmcaFsmState current = S.bmca.state;
    PtpBmcaFsmState state;

    PtpGptpPriorityVector own;
    own_vector(&own);

    if (!S.gptp.linkUp || !S.gptp.pdelay.asCapable) {
        // the information received went with the link
        B.infoValid = false;
        state = PTP_BMCA_DISABLED;
    } else if (B.infoValid && (compare_vectors(&B.master, &own) < 0)) {
        state = PTP_BMCA_SLAVE;
    } else if (gm_capable()) {
        state = PTP_BMCA_MASTER;
    } else {
        state = PTP_BMCA_LISTENING;
    }

    // a port that just came up listens before it takes the grandmaster role
    bool comingUp = (current != PTP_BMCA_MASTER) && (current != PTP_BMCA_SLAVE);
    if ((state == PTP_BMCA_MASTER) && comingUp && (B.listenTmr < listen_ticks())) {
        state = PTP_BMCA_LISTENING;
    }

    if ((state == PTP_BMCA_LISTENING) && (current != PTP_BMCA_LISTENING)) {
        B.listenTmr = 0;
    }

    // the grandmaster this role implies
    switch (state) {
    case PTP_BMCA_SLAVE:
        set_grandmaster(B.master.gmIdentity, &B.master);
        break;
    case PTP_BMCA_MASTER:
        set_grandmaster(own.gmIdentity, &own);
        break;
    default:
        set_grandmaster(0, NULL);
        break;
    }

    ptp_gptp_set_port_state(state);
}

bool ptp_gptp_bmca_is_gm(void) {
    return S.bmca.state == PTP_BMCA_MASTER;
}

// ---- timing ----

void ptp_gptp_bmca_reset(void) {
    memset(&B, 0, sizeof(B));

    B.masterLogAnnounce = S.gptp.logAnnounce;
    B.masterLogSync = S.gptp.logSync;

    set_grandmaster(0, NULL);
}

void ptp_gptp_bmca_tick(void) {
    PtpBmcaFsmState state = S.bmca.state;

    if (state == PTP_BMCA_LISTENING) {
        B.listenTmr++;

        // nothing better heard in time: the grandmaster role, if we may take it
        if (B.listenTmr == listen_ticks()) {
            ptp_gptp_bmca_reselect();
        }

        return;
    }

    if ((state != PTP_BMCA_SLAVE) || !B.infoValid) {
        return;
    }

    B.announceTmr++;
    B.syncTmr++;

    uint32_t announceLimit = PTP_GPTP_ANNOUNCE_RECEIPT_TIMEOUT * ptp_gptp_log_to_ticks(B.masterLogAnnounce);
    uint32_t syncLimit = PTP_GPTP_SYNC_RECEIPT_TIMEOUT * ptp_gptp_log_to_ticks(B.masterLogSync);

    bool announceLost = B.announceTmr > announceLimit;
    bool syncLost = B.syncTmr > syncLimit;

    if (!announceLost && !syncLost) {
        return;
    }

    if (announceLost) {
        B.announceTimeouts++;
    } else {
        B.syncTimeouts++;
    }

    CLILOG(S.logging.logid && S.logging.bmca, "[LOG-BMCA] ");
    CLILOG(S.logging.bmca, "%s receipt timeout: the master information is aged\n", announceLost ? "announce" : "sync");

    B.infoValid = false;
    ptp_gptp_bmca_reselect();
}

// ---- reception ----

void ptp_gptp_bmca_announce_rx(const RawPtpMessage *pRawMsg, const PtpHeader *pHeader) {
    // a port that is not asCapable takes no Announce
    if (!S.gptp.linkUp || !S.gptp.pdelay.asCapable) {
        return;
    }

    const uint8_t *data = pRawMsg->data;
    uint32_t size = MIN(pHeader->messageLength, pRawMsg->size);

    if (size < PTP_PCKT_SIZE_ANNOUNCE) {
        return;
    }

    // our own Announce, come back
    if (pHeader->clockIdentity == S.hwoptions.clockIdentity) {
        return;
    }

    PtpAnnounceBody ann;
    ptp_extract_announce_message(&ann, (void *)data);

    if (ann.localStepsRemoved >= STEPS_REMOVED_MAX) {
        CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
        CLILOG(S.logging.info, "Announce discarded: stepsRemoved %u\n", (unsigned)ann.localStepsRemoved);
        return;
    }

    // an Announce that has already passed through us is a loop
    uint64_t path[PTP_GPTP_PATH_TRACE_MAX];
    uint16_t pathN = 0;

    uint16_t tlvLen = 0;
    const uint8_t *tlv = ptp_gptp_find_tlv(data, size, PTP_PCKT_SIZE_ANNOUNCE, PTP_TLV_PATH_TRACE, -1, &tlvLen);

    if (tlv != NULL) {
        uint16_t n = tlvLen / 8;

        for (uint16_t i = 0; i < n; i++) {
            uint64_t id;
            memcpy(&id, tlv + 4 + 8 * i, 8);

            if (id == S.hwoptions.clockIdentity) {
                CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
                CLILOG(S.logging.info, "Announce discarded: our clockIdentity is in its path trace\n");
                return;
            }

            if (pathN < PTP_GPTP_PATH_TRACE_MAX) {
                path[pathN] = id;
                pathN++;
            }
        }
    }

    // naming us grandmaster without a path through us: stale or looped too
    if (ann.grandmasterClockIdentity == S.hwoptions.clockIdentity) {
        return;
    }

    PtpGptpPriorityVector v = {
        .priority1 = ann.priority1,
        .clockClass = ann.grandmasterClockClass,
        .clockAccuracy = ann.grandmasterClockAccuracy,
        .variance = ann.grandmasterClockVariance,
        .priority2 = ann.priority2,
        .gmIdentity = ann.grandmasterClockIdentity,
        .stepsRemoved = ann.localStepsRemoved,
        .senderIdentity = pHeader->clockIdentity,
        .senderPort = pHeader->sourcePortID,
    };

    B.announceRx++;
    PTP_IUEV(PTP_UEV_ANNOUNCE_RECVED);

    bool fromMaster = B.infoValid &&
                      (v.senderIdentity == B.master.senderIdentity) &&
                      (v.senderPort == B.master.senderPort);

    // the selected master's port updates its information, whatever it says;
    // another port replaces it only with better information
    if (!fromMaster && B.infoValid && (compare_vectors(&v, &B.master) >= 0)) {
        return;
    }

    B.master = v;
    B.infoValid = true;
    B.utcOffset = ann.currentUTCOffset;
    B.timeSource = ann.timeSource;
    B.pathTraceN = pathN;
    memcpy(B.pathTrace, path, pathN * sizeof(uint64_t));

    B.masterLogAnnounce = pHeader->logMessagePeriod;
    B.announceTmr = 0;

    if (!fromMaster) {
        // a new master: its Syncs are awaited from now
        B.syncTmr = 0;
        B.masterLogSync = S.gptp.logSync;
    }

    ptp_gptp_bmca_reselect();
}

void ptp_gptp_bmca_sync_rx(RawPtpMessage *pRawMsg, PtpHeader *pHeader) {
    if ((S.bmca.state != PTP_BMCA_SLAVE) || !B.infoValid) {
        return;
    }

    // only from the port that sent the selected Announce
    if ((pHeader->clockIdentity != B.master.senderIdentity) ||
        (pHeader->sourcePortID != B.master.senderPort)) {
        return;
    }

    if (pHeader->messageType == PTP_MT_Sync) {
        B.syncTmr = 0;

        // the master's Sync interval paces the reverse sync
        if (B.masterLogSync != pHeader->logMessagePeriod) {
            B.masterLogSync = pHeader->logMessagePeriod;
            ptp_gptp_rsync_update();
        }
    } else {
        // the Follow_Up information TLV: the grandmaster's rate and time base
        uint32_t size = MIN(pHeader->messageLength, pRawMsg->size);

        PtpGptpFollowUpInfo info;
        memset(&info, 0, sizeof(info));

        if (ptp_gptp_read_follow_up_info(pRawMsg->data, size, &info)) {
            if (S.gptp.fuInfo.valid && (info.gmTimeBaseIndicator != S.gptp.fuInfo.gmTimeBaseIndicator)) {
                CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
                CLILOG(S.logging.info, "gmTimeBaseIndicator %u -> %u\n",
                       (unsigned)S.gptp.fuInfo.gmTimeBaseIndicator,
                       (unsigned)info.gmTimeBaseIndicator);
            }

            S.gptp.fuInfo = info;

            // rateRatio = (1 + cumulativeScaledRateOffset / 2^41) * neighborRateRatio
            double cumulative = 1.0 + (double)info.cumulativeScaledRateOffset / 2199023255552.0;
            double nrr = S.gptp.pdelay.nrrValid ? S.gptp.pdelay.nrr : 1.0;

            S.gptp.rateRatio = cumulative * nrr;
        }

        B.syncRx++;
    }

    ptp_slave_process_message(pRawMsg, pHeader);
}
