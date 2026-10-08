/* (C) Kebag-Logic, 2026 */

// Reverse sync: the egress method of Avnu's "802.1AS Recovered Clock Quality
// Testing" (and of the IEEE 802.1 proposal it comes from). A second,
// static-master instance on the same port sends the recovered clock back to a
// directly attached tester:
//
//   - two-step Sync/Follow_Up on reverseSyncDomain, which must differ from
//     the measured domain; no Announce, no BMCA;
//   - preciseOriginTimestamp = the recovered gPTP time at the Sync's egress:
//     our clock, steered onto the grandmaster's, less the servo offset;
//   - the Follow_Up information TLV with the grandmaster's time base, and a
//     rate ratio of 1, our clock being syntonized;
//   - on a timer of its own, at reverseSyncRate percent of the incoming Sync
//     interval (90 % by default, about 112 ms for 125 ms), so the samples
//     sweep the whole Sync interval instead of following each Sync;
//   - also while the grandmaster is lost, so the tester sees the holdover.

#include "gptp.h"

#include <math.h>
#include <string.h>

#include "minmax.h"
#include "msg_utils.h"
#include "ptp_core.h"
#include "ptp_defs.h"
#include "task_ptp.h"
#include "timeutils.h"

///\cond 0
#define S (gPtpCoreState)
#define R (gPtpCoreState.gptp.rsync)
///\endcond

#define RSYNC_PERIOD_MIN_US (1000) ///< the shortest period the timer is asked for

/**
 * The period: a fixed interval, or a percentage of the incoming Sync
 * interval (ours when there is no master to follow).
 */
static uint32_t rsync_period_us(void) {
    if (R.interval_ms > 0) {
        return R.interval_ms * 1000;
    }

    int8_t logSync = (S.bmca.state == PTP_BMCA_SLAVE) ? S.gptp.bmca.masterLogSync : S.gptp.logSync;

    uint64_t us = (uint64_t)ptp_gptp_log_to_us(logSync) * R.ratePct / 100;

    return (uint32_t)MAX(us, RSYNC_PERIOD_MIN_US);
}

/**
 * The nearest logMessageInterval for a period.
 */
static int8_t log_interval_of(uint32_t period_us) {
    if (period_us == 0) {
        return 0;
    }

    return (int8_t)lround(log2((double)period_us / 1e6));
}

void ptp_gptp_rsync_update(void) {
    bool run = S.gptp.enabled &&
               R.enabled &&
               S.gptp.linkUp &&
               (R.domain != S.profile.domainNumber);

    uint32_t period = run ? rsync_period_us() : 0;

    if (period == R.period_us) {
        return;
    }

    if (!ptp_aux_timer_set(period)) {
        CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
        CLILOG(S.logging.info, "reverse sync: the timer cannot run at %u us\n", (unsigned)period);
        return;
    }

    R.period_us = period;

    CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
    if (period != 0) {
        CLILOG(S.logging.info, "reverse sync on domain %u every %u us\n", (unsigned)R.domain, (unsigned)period);
    } else {
        CLILOG(S.logging.info, "reverse sync stopped\n");
    }
}

/**
 * Transmit callback of a reverse Sync: its Follow_Up, with the recovered time
 * at its egress.
 */
static void rsync_sync_sent(const RawPtpMessage *pMsg) {
    PtpHeader syncHeader;
    ptp_extract_header(&syncHeader, pMsg->data);

    // the recovered gPTP time: the servo holds our clock at the grandmaster's plus the offset
    TimestampI origin;
    subTime(&origin, &pMsg->ts, &S.hwoptions.offset);

    PtpHeader h;
    ptp_gptp_fill_header(&h,
                         PTP_MT_Follow_Up,
                         PTP_PCKT_SIZE_FOLLOW_UP + PTP_GPTP_FOLLOW_UP_INFO_TLV_LEN,
                         R.domain,
                         syncHeader.sequenceID,
                         syncHeader.logMessagePeriod);

    RawPtpMessage fu;
    memset(&fu, 0, sizeof(fu));

    ptp_construct_binary_header(fu.data, &h);
    ptp_write_binary_timestamps(fu.data, &origin, 1);

    // the grandmaster's time base; our clock is syntonized to it
    PtpGptpFollowUpInfo info = S.gptp.fuInfo;
    info.cumulativeScaledRateOffset = 0;
    ptp_gptp_write_follow_up_info(fu.data + PTP_PCKT_SIZE_FOLLOW_UP, &info);

    fu.size = PTP_PCKT_SIZE_FOLLOW_UP + PTP_GPTP_FOLLOW_UP_INFO_TLV_LEN;
    fu.tag = RPMT_RANDOM;
    fu.pTxCb = NULL;
    fu.tx_dm = PTP_DM_P2P;
    fu.tx_mc = PTP_MC_GENERAL;
    fu.tx_mt = PTP_MT_Follow_Up;
    fu.ttl = FLEXPTP_RANDOM_TAGGED_MESSAGE_TTL_TICKS;

    ptp_transmit_enqueue(&fu);

    R.sent++;
}

void ptp_gptp_rsync_send(void) {
    if (!S.gptp.linkUp || !R.enabled || (R.period_us == 0)) {
        return;
    }

    PtpHeader h;
    ptp_gptp_fill_header(&h,
                         PTP_MT_Sync,
                         PTP_PCKT_SIZE_SYNC,
                         R.domain,
                         R.seq,
                         log_interval_of(R.period_us));
    h.flags.PTP_TWO_STEP = true;

    R.seq++;

    // the originTimestamp of a two-step Sync is reserved: zero
    RawPtpMessage msg;
    memset(&msg, 0, sizeof(msg));
    ptp_construct_binary_header(msg.data, &h);

    msg.size = PTP_PCKT_SIZE_SYNC;
    msg.tag = RPMT_RANDOM;
    msg.pTxCb = rsync_sync_sent;
    msg.tx_dm = PTP_DM_P2P;
    msg.tx_mc = PTP_MC_EVENT;
    msg.tx_mt = PTP_MT_Sync;
    msg.ttl = FLEXPTP_RANDOM_TAGGED_MESSAGE_TTL_TICKS;

    ptp_transmit_enqueue(&msg);
}
