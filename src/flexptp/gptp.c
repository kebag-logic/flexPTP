/* (C) Kebag-Logic, 2026 */

// The IEEE 802.1AS layer: whether the profile runs it, the link, the tick,
// the routing of received messages, and the port state.

#include "gptp.h"

#include <string.h>

#include "common.h"
#include "event.h"
#include "minmax.h"
#include "ptp_core.h"
#include "ptp_defs.h"
#include "task_ptp.h"

///\cond 0
#define S (gPtpCoreState)
///\endcond

#define LOG_INTERVAL_MIN (-7) ///< shortest interval handled: 7.8 ms
#define LOG_INTERVAL_MAX (8)  ///< longest interval handled: 256 s

static const char *PORT_STATE_NAMES[] = {
    "INITIALIZING",
    "LISTENING",
    "PRE_MASTER",
    "MASTER",
    "SLAVE",
    "PASSIVE",
    "UNCALIBRATED",
    "FAULTY",
    "DISABLED"};

/**
 * An 802.1AS profile: majorSdoId 1, the peer-to-peer delay mechanism, 802.3.
 */
bool ptp_gptp_profile_is_gptp(void) {
    return (S.profile.transportSpecific == PTP_TSPEC_GPTP_8021AS) &&
           (S.profile.delayMechanism == PTP_DM_P2P) &&
           (S.profile.transportType == PTP_TP_802_3);
}

// ---- helpers ----

uint32_t ptp_gptp_log_to_us(int8_t logInterval) {
    int8_t l = MIN(MAX(logInterval, LOG_INTERVAL_MIN), LOG_INTERVAL_MAX);

    if (l >= 0) {
        return 1000000u << l;
    }

    return 1000000u >> (-l);
}

uint32_t ptp_gptp_log_to_ticks(int8_t logInterval) {
    uint32_t us = ptp_gptp_log_to_us(logInterval);
    uint32_t tick_us = PTP_HEARTBEAT_TICKRATE_MS * 1000;

    uint32_t ticks = (us + tick_us / 2) / tick_us;

    return MAX(ticks, 1);
}

void ptp_gptp_set_port_state(PtpBmcaFsmState state) {
    if (S.bmca.state == state) {
        return;
    }

    CLILOG(S.logging.logid && S.logging.bmca, "[LOG-BMCA] ");
    CLILOG(S.logging.bmca, "%s -> %s\n", PORT_STATE_NAMES[S.bmca.state], PORT_STATE_NAMES[state]);

    S.bmca.stateDuration = 0;
    S.bmca.state = state;

    // the core enables the slave or the master module
    PtpCoreEvent event = {.code = PTP_CEV_BMCA_STATE_CHANGED, .w = {.w = state}, .dw = {.dw = 0}};
    ptp_event_enqueue(&event);

    // a reverse sync paced on the incoming Syncs follows the role
    ptp_gptp_rsync_update();

    ptp_gptp_publish_status();
}

// ---- life cycle ----

void ptp_gptp_init(void) {
    S.gptp.linkUp = true;

    S.gptp.neighborPropDelayThresh_ns = PTP_GPTP_NEIGHBOR_PROP_DELAY_THRESH_NS;
    S.gptp.allowedLostResponses = PTP_GPTP_ALLOWED_LOST_RESPONSES;
    S.gptp.gmCapable = true;

    memset(&S.gptp.rsync, 0, sizeof(S.gptp.rsync));
    S.gptp.rsync.domain = PTP_GPTP_RSYNC_DOMAIN;
    S.gptp.rsync.ratePct = PTP_GPTP_RSYNC_RATE_PCT;
}

void ptp_gptp_reset(void) {
    S.gptp.enabled = ptp_gptp_profile_is_gptp();

    // the initial intervals: the profile's
    S.gptp.logPdelay = (S.profile.logDelayReqPeriod == PTP_LOGPER_SYNCMATCHED) ? 0 : S.profile.logDelayReqPeriod;
    S.gptp.logSync = S.profile.logSyncPeriod;
    S.gptp.logAnnounce = S.profile.logAnnouncePeriod;

    S.gptp.pdelayStopped = false;
    S.gptp.syncStopped = false;
    S.gptp.announceStopped = false;
    S.gptp.computeNrr = true;
    S.gptp.computeDelay = true;

    memset(&S.gptp.fuInfo, 0, sizeof(S.gptp.fuInfo));
    S.gptp.rateRatio = 1.0;

    ptp_gptp_pdelay_reset();
    ptp_gptp_bmca_reset();

    if (S.gptp.enabled) {
        // the port comes up once the peer delay mechanism finds it asCapable
        ptp_gptp_bmca_reselect();
    }

    ptp_gptp_rsync_update();
    ptp_gptp_publish_status();
}

void ptp_gptp_tick(void) {
    if (S.gptp.enabled) {
        ptp_gptp_pdelay_tick();
        ptp_gptp_bmca_tick();
    }

    ptp_gptp_publish_status();
}

void ptp_gptp_link_state(bool up) {
    if (S.gptp.linkUp == up) {
        return;
    }

    S.gptp.linkUp = up;

    CLILOG(S.logging.logid && S.logging.bmca, "[LOG-BMCA] ");
    CLILOG(S.logging.bmca, "link %s\n", up ? "up" : "down");

    if (S.gptp.enabled) {
        // a new link may lead to a new neighbour: measure from scratch
        ptp_gptp_pdelay_reset();
        ptp_gptp_bmca_reselect();
    }

    ptp_gptp_rsync_update();
    ptp_gptp_publish_status();
}

void ptp_gptp_aux_timer(void) {
    if (S.gptp.enabled) {
        ptp_gptp_rsync_send();
    }
}

void ptp_gptp_apply(void) {
    if (S.gptp.enabled) {
        // the asCapable thresholds, and the grandmaster capability
        ptp_gptp_pdelay_judge();
        ptp_gptp_bmca_reselect();
    }

    ptp_gptp_rsync_update();
    ptp_gptp_publish_status();
}

// ---- reception ----

void ptp_gptp_process_packet(RawPtpMessage *pRawMsg, PtpHeader *pHeader) {
    // gPTP messages of the measured domain only: the reverse sync domain is for sending
    if ((pHeader->transportSpecific != PTP_TSPEC_GPTP_8021AS) ||
        (pHeader->domainNumber != S.profile.domainNumber)) {
        return;
    }

    if (!S.gptp.linkUp) {
        return;
    }

    switch (pHeader->messageType) {
    case PTP_MT_PDelay_Req:
        // answered in every port state
        PTP_IUEV(PTP_UEV_PDELAY_REQ_RECVED);
        ptp_send_pdelay_resp(pRawMsg);
        PTP_IUEV(PTP_UEV_PDELAY_RESP_SENT);
        break;

    case PTP_MT_PDelay_Resp:
    case PTP_MT_PDelay_Resp_Follow_Up:
        ptp_gptp_pdelay_rx(pRawMsg, pHeader);
        break;

    case PTP_MT_Announce:
        ptp_gptp_bmca_announce_rx(pRawMsg, pHeader);
        break;

    case PTP_MT_Sync:
    case PTP_MT_Follow_Up:
        ptp_gptp_bmca_sync_rx(pRawMsg, pHeader);
        break;

    case PTP_MT_Signaling:
        ptp_gptp_signaling_rx(pRawMsg, pHeader);
        break;

    default:
        // Delay_Req and Delay_Resp have no place in 802.1AS
        break;
    }
}
