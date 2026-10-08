/* (C) Kebag-Logic, 2026 */

// The peer delay mechanism of IEEE 802.1AS-2020 clause 11 for full-duplex
// 802.3 links: the initiator (MDPdelayReq), the neighborRateRatio, the
// neighborPropDelay, and asCapable. The responder is the core's (common.c),
// which answers in every port state.
//
// A request completes whatever the order in which its four timestamps
// arrive: the egress timestamp of the Pdelay_Req (t1) comes from the network
// stack driver, possibly after the Pdelay_Resp (t2, t4) and its Follow_Up
// (t3).

#include "gptp.h"

#include <math.h>
#include <string.h>

#include "event.h"
#include "msg_utils.h"
#include "ptp_core.h"
#include "ptp_defs.h"
#include "task_ptp.h"
#include "timeutils.h"

///\cond 0
#define S (gPtpCoreState)
#define PD (gPtpCoreState.gptp.pdelay)
///\endcond

#define NRR_MAX_DEVIATION (1e-3) ///< a ratio further than 1000 ppm from 1 is a step or an outlier, not a rate
#define RATE_RING_LEN (PTP_GPTP_NRR_WINDOW + 1)

// ---- asCapable ----

static void pdelay_set_as_capable(bool asCapable) {
    if (PD.asCapable == asCapable) {
        return;
    }

    PD.asCapable = asCapable;

    CLILOG(S.logging.logid && S.logging.bmca, "[LOG-BMCA] ");
    CLILOG(S.logging.bmca, "asCapable %s\n", asCapable ? "TRUE" : "FALSE");

    ptp_gptp_bmca_reselect();
    ptp_gptp_publish_status();
}

/**
 * asCapable (IEEE 802.1AS-2020 11.2.2): a measured link delay within the
 * threshold, the neighbour answering, one responder, not ourselves, and a
 * measured neighborRateRatio. The last one means a neighbour that was lost
 * takes two exchanges to be trusted again, not one.
 */
void ptp_gptp_pdelay_judge(void) {
    bool ok = PD.delayValid &&
              PD.isMeasuringDelay &&
              (PD.nrrValid || !S.gptp.computeNrr) &&
              !PD.multipleResponders &&
              !PD.selfResponder &&
              (PD.meanLinkDelay <= S.gptp.neighborPropDelayThresh_ns) &&
              (PD.lostResponses <= S.gptp.allowedLostResponses);

    pdelay_set_as_capable(ok);
}

// ---- measurements ----

/**
 * neighborRateRatio: the neighbour's clock over ours, across the window of
 * the latest (t3, t4) pairs.
 */
static void pdelay_update_nrr(void) {
    PtpGptpRatePoint *newest = &PD.ratePts[PD.ratePtsHead];
    newest->t3 = PD.t3;
    newest->t4 = PD.t4;

    PD.ratePtsHead = (PD.ratePtsHead + 1) % RATE_RING_LEN;

    if (PD.ratePtsN < RATE_RING_LEN) {
        PD.ratePtsN++;
    }

    if (PD.ratePtsN < 2) {
        return;
    }

    // the oldest pair in the ring against the newest
    uint8_t oldest = (PD.ratePtsHead + RATE_RING_LEN - PD.ratePtsN) % RATE_RING_LEN;

    TimestampI d3;
    TimestampI d4;
    subTime(&d3, &newest->t3, &PD.ratePts[oldest].t3);
    subTime(&d4, &newest->t4, &PD.ratePts[oldest].t4);

    int64_t n3 = nsI(&d3);
    int64_t n4 = nsI(&d4);

    if (n4 <= 0) {
        return;
    }

    double nrr = (double)n3 / (double)n4;

    // a clock stepped on either side: start the window over from this pair
    if (fabs(nrr - 1.0) > NRR_MAX_DEVIATION) {
        PD.ratePts[0] = *newest;
        PD.ratePtsN = 1;
        PD.ratePtsHead = 1;
        return;
    }

    PD.nrr = nrr;
    PD.nrrValid = true;
}

static int64_t median(const int64_t *samples, uint8_t n) {
    int64_t sorted[PTP_GPTP_DELAY_FILTER_LEN];
    memcpy(sorted, samples, n * sizeof(int64_t));

    // insertion sort: a handful of samples
    for (uint8_t i = 1; i < n; i++) {
        int64_t v = sorted[i];
        int j = i - 1;

        while ((j >= 0) && (sorted[j] > v)) {
            sorted[j + 1] = sorted[j];
            j--;
        }

        sorted[j + 1] = v;
    }

    return sorted[n / 2];
}

/**
 * neighborPropDelay = (r (t4 - t1) - (t3 - t2)) / 2, filtered by the median
 * of the latest samples. The correctionFields of the Pdelay_Resp and its
 * Follow_Up carry the fractional nanoseconds of t2 and t3.
 */
static void pdelay_update_delay(void) {
    TimestampI rtt;
    TimestampI turn;
    subTime(&rtt, &PD.t4, &PD.t1);
    subTime(&turn, &PD.t3, &PD.t2);

    int64_t turn_ns = nsI(&turn) + PD.cf3 - PD.cf2;
    double r = PD.nrrValid ? PD.nrr : 1.0;

    int64_t d = (int64_t)llround((r * (double)nsI(&rtt) - (double)turn_ns) / 2.0);

    PD.delaySamples[PD.delayHead] = d;
    PD.delayHead = (PD.delayHead + 1) % PTP_GPTP_DELAY_FILTER_LEN;

    if (PD.delayN < PTP_GPTP_DELAY_FILTER_LEN) {
        PD.delayN++;
    }

    PD.meanLinkDelay = median(PD.delaySamples, PD.delayN);
    PD.delayValid = true;

    // what the slave subtracts from every Sync
    nsToTsI(&S.network.meanPathDelay, PD.meanLinkDelay);
}

static void pdelay_try_complete(void) {
    if (!PD.inFlight || !PD.haveT1 || !PD.haveResp || !PD.haveRespFollowUp) {
        return;
    }

    PD.inFlight = false;

    // our own clock answered: a loop, or a peer impersonating us
    if (PD.responder == S.hwoptions.clockIdentity) {
        PD.selfResponder = true;
        ptp_gptp_pdelay_judge();
        return;
    }

    PD.selfResponder = false;

    // a one-step responder gives no t3 in its own clock: no rate from it
    if (S.gptp.computeNrr && !PD.oneStep) {
        pdelay_update_nrr();
    }

    if (S.gptp.computeDelay) {
        pdelay_update_delay();
    }

    PD.lostResponses = 0;
    PD.isMeasuringDelay = true;

    CLILOG(S.logging.logid && S.logging.timestamps, "[LOG-TS:P] ");
    CLILOG(S.logging.timestamps, "Pdelay #%u: t1 %d.%09d t2 %d.%09d t3 %d.%09d t4 %d.%09d -> %d ns, nrr %.9f\n",
           (unsigned)PD.seq,
           (int32_t)PD.t1.sec, PD.t1.nanosec,
           (int32_t)PD.t2.sec, PD.t2.nanosec,
           (int32_t)PD.t3.sec, PD.t3.nanosec,
           (int32_t)PD.t4.sec, PD.t4.nanosec,
           (int32_t)PD.meanLinkDelay,
           PD.nrr);

    ptp_gptp_pdelay_judge();
}

// ---- the request ----

/**
 * Transmit callback of a Pdelay_Req: its egress timestamp, t1.
 */
static void pdelay_req_sent(const RawPtpMessage *pMsg) {
    PtpHeader h;
    ptp_extract_header(&h, pMsg->data);

    // the timestamp of an older request
    if (!PD.inFlight || (h.sequenceID != PD.seq)) {
        return;
    }

    PD.t1 = pMsg->ts;
    PD.haveT1 = true;

    pdelay_try_complete();
}

static void pdelay_send_req(void) {
    PD.seq++;

    PtpHeader h;
    ptp_gptp_fill_header(&h,
                         PTP_MT_PDelay_Req,
                         PTP_PCKT_SIZE_PDELAY_REQ,
                         S.profile.domainNumber,
                         PD.seq,
                         S.gptp.logPdelay);

    // the body is 20 reserved octets
    RawPtpMessage msg;
    memset(&msg, 0, sizeof(msg));
    ptp_construct_binary_header(msg.data, &h);

    msg.size = PTP_PCKT_SIZE_PDELAY_REQ;
    msg.tag = RPMT_RANDOM;
    msg.pTxCb = pdelay_req_sent;
    msg.tx_dm = PTP_DM_P2P;
    msg.tx_mc = PTP_MC_EVENT;
    msg.tx_mt = PTP_MT_PDelay_Req;
    msg.ttl = FLEXPTP_RANDOM_TAGGED_MESSAGE_TTL_TICKS;

    // nothing of the new request has arrived yet
    PD.inFlight = true;
    PD.haveT1 = false;
    PD.haveResp = false;
    PD.haveRespFollowUp = false;
    PD.responses = 0;

    ptp_transmit_enqueue(&msg);

    PTP_IUEV(PTP_UEV_PDELAY_REQ_SENT);
}

// ---- interface ----

void ptp_gptp_pdelay_reset(void) {
    memset(&PD, 0, sizeof(PD));

    PD.nrr = 1.0;

    // the first request goes out on the next tick
    PD.tmr = UINT16_MAX;

    nsToTsI(&S.network.meanPathDelay, 0);
}

void ptp_gptp_pdelay_tick(void) {
    if (!S.gptp.linkUp || S.gptp.pdelayStopped) {
        return;
    }

    PD.tmr++;

    if (PD.tmr < ptp_gptp_log_to_ticks(S.gptp.logPdelay)) {
        return;
    }

    PD.tmr = 0;

    // the request before had its window: one responder clears an earlier
    // multiple-responder fault; more than one was flagged as they came
    if (PD.responses == 1) {
        PD.multipleResponders = false;
        ptp_gptp_pdelay_judge();
    }

    // the request before was not answered in time
    if (PD.inFlight) {
        PD.inFlight = false;
        PD.lostResponses++;
        PD.lostTotal++;

        CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
        CLILOG(S.logging.info, "Pdelay_Req #%u: no complete response (%u in a row)\n",
               (unsigned)PD.seq,
               (unsigned)PD.lostResponses);

        PTP_IUEV(PTP_UEV_NETWORK_ERROR);

        // the neighbour is gone: measure from scratch when it is back
        if (PD.lostResponses > S.gptp.allowedLostResponses) {
            PD.isMeasuringDelay = false;

            PD.ratePtsN = 0;
            PD.ratePtsHead = 0;
            PD.nrrValid = false;
            PD.nrr = 1.0;

            PD.delayN = 0;
            PD.delayHead = 0;
            PD.delayValid = false;
        }

        ptp_gptp_pdelay_judge();
    }

    pdelay_send_req();
}

void ptp_gptp_pdelay_rx(const RawPtpMessage *pRawMsg, const PtpHeader *pHeader) {
    // only the answers to our own requests
    PtpDelay_RespIdentification reqId;
    ptp_read_delay_resp_id_data(&reqId, (void *)pRawMsg->data);

    if ((reqId.requestingSourceClockIdentity != S.hwoptions.clockIdentity) ||
        (reqId.requestingSourcePortIdentity != PTP_PORT_ID)) {
        return;
    }

    // an answer to an older request
    if (pHeader->sequenceID != PD.seq) {
        return;
    }

    if (pHeader->messageType == PTP_MT_PDelay_Resp) {
        // counted until the next request, also after this one completed: a
        // second responder may answer after the first exchange is done
        PD.responses++;

        // more than one port answered: no measurement from this request
        if (PD.responses > 1) {
            if (PD.responses == 2) {
                PD.multipleTotal++;

                CLILOG(S.logging.logid && S.logging.info, "[LOG-INFO] ");
                CLILOG(S.logging.info, "Pdelay_Req #%u: answered by more than one port\n", (unsigned)PD.seq);
            }

            PD.inFlight = false;
            PD.multipleResponders = true;

            ptp_gptp_pdelay_judge();
            return;
        }

        // the exchange is done already (one-step), or was abandoned
        if (!PD.inFlight) {
            return;
        }

        PD.responder = pHeader->clockIdentity;
        PD.responderPort = pHeader->sourcePortID;

        ptp_extract_timestamps(&PD.t2, (void *)pRawMsg->data, 1);
        PD.t4 = pRawMsg->ts;
        PD.cf2 = (int64_t)pHeader->correction_ns;
        PD.haveResp = true;
        PD.oneStep = !pHeader->flags.PTP_TWO_STEP;

        // a one-step responder: the turnaround is in the correctionField, no Follow_Up comes
        if (PD.oneStep) {
            PD.t2 = zeroTs;
            nsToTsI(&PD.t3, PD.cf2);
            PD.cf2 = 0;
            PD.cf3 = 0;
            PD.haveRespFollowUp = true;
        }

        PTP_IUEV(PTP_UEV_PDELAY_RESP_RECVED);

    } else if (pHeader->messageType == PTP_MT_PDelay_Resp_Follow_Up) {
        // it must come from the port that sent the Pdelay_Resp, for an exchange still open
        if (!PD.inFlight ||
            !PD.haveResp ||
            (pHeader->clockIdentity != PD.responder) ||
            (pHeader->sourcePortID != PD.responderPort)) {
            return;
        }

        ptp_extract_timestamps(&PD.t3, (void *)pRawMsg->data, 1);
        PD.cf3 = (int64_t)pHeader->correction_ns;
        PD.haveRespFollowUp = true;

        PTP_IUEV(PTP_UEV_PDELAY_RESP_FOLLOW_UP_RECVED);
    }

    pdelay_try_complete();
}
