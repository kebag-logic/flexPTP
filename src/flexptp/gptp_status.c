/* (C) Kebag-Logic, 2026 */

#include "gptp_status.h"

#include <string.h>

#include "gptp.h"
#include "ptp_core.h"
#include "format_utils.h"
#include "settings_interface.h"

#ifdef FLEXPTP_LINUX
#include <pthread.h>
#endif

///\cond 0
#define S (gPtpCoreState)
///\endcond

static PtpGptpStatus sStatus; ///< the snapshot handed out

#ifdef FLEXPTP_LINUX
static pthread_mutex_t sStatusLock = PTHREAD_MUTEX_INITIALIZER;
#define STATUS_LOCK() pthread_mutex_lock(&sStatusLock)
#define STATUS_UNLOCK() pthread_mutex_unlock(&sStatusLock)
#else
// single context: the snapshot is read where the core runs
#define STATUS_LOCK()
#define STATUS_UNLOCK()
#endif

void ptp_gptp_publish_status(void) {
    const PtpGptpPdelayState *pd = &S.gptp.pdelay;
    const PtpGptpBmcaState *bm = &S.gptp.bmca;

    PtpGptpStatus st;
    memset(&st, 0, sizeof(st));

    st.version = PTP_GPTP_STATUS_VERSION;

    // ---- port ----
    st.portState = S.bmca.state;
    st.linkUp = S.gptp.linkUp;
    st.asCapable = pd->asCapable;
    st.isMeasuringDelay = pd->isMeasuringDelay;
    st.domain = S.profile.domainNumber;
    st.gmPresent = (bm->gmIdentity != 0);
    st.locked = S.stats.locked;
    st.ownIdentity = FLEXPTP_ntohll(S.hwoptions.clockIdentity);

    // ---- grandmaster: the master's when slave, ours when we are it ----
    bool slave = (S.bmca.state == PTP_BMCA_SLAVE);
    const PtpMasterProperties *mp = &S.bmca.masterProps;

    st.gmIdentity = FLEXPTP_ntohll(bm->gmIdentity);

    if (st.gmPresent) {
        st.gmPriority1 = mp->priority1;
        st.gmClockClass = mp->grandmasterClockClass;
        st.gmClockAccuracy = mp->grandmasterClockAccuracy;
        st.gmPriority2 = mp->priority2;
        st.gmVariance = mp->grandmasterClockVariance;
        st.stepsRemoved = mp->localStepsRemoved;
    }

    st.gmTimeBaseIndicator = S.gptp.fuInfo.gmTimeBaseIndicator;
    st.gmChanges = bm->gmChanges;

    // ---- link and time ----
    st.meanLinkDelay_ns = pd->meanLinkDelay;
    st.neighborRateRatio = pd->nrrValid ? pd->nrr : 1.0;
    st.rateRatio = S.gptp.rateRatio;
    st.timeError_ns = slave ? nsI(&S.slave.prevTimeError) : 0;
#ifdef PTP_HLT_INTERFACE
    st.tuning_ppb = S.hwclock.tuning_ppb;
#endif
    st.logSyncInterval = slave ? bm->masterLogSync : S.gptp.logSync;
    st.logPdelayInterval = S.gptp.logPdelay;
    st.logAnnounceInterval = slave ? bm->masterLogAnnounce : S.gptp.logAnnounce;

    // ---- counters ----
    st.syncRx = bm->syncRx;
    st.syncTimeouts = bm->syncTimeouts;
    st.announceRx = bm->announceRx;
    st.announceTimeouts = bm->announceTimeouts;
    st.pdelayLost = pd->lostTotal;
    st.pdelayMultiple = pd->multipleTotal;
    st.signalingRx = S.gptp.signalingRx;
    st.txTimestampsLost = S.stats.drop_cntrs.txts;

    // ---- reverse sync ----
    st.rsyncEnabled = S.gptp.rsync.enabled;
    st.rsyncDomain = S.gptp.rsync.domain;
    st.rsyncInterval_us = S.gptp.rsync.period_us;
    st.rsyncSent = S.gptp.rsync.sent;

    STATUS_LOCK();
    st.updates = sStatus.updates + 1;
    sStatus = st;
    STATUS_UNLOCK();
}

void ptp_gptp_get_status(PtpGptpStatus *pStatus) {
    STATUS_LOCK();
    *pStatus = sStatus;
    STATUS_UNLOCK();
}
