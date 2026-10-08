/* (C) Kebag-Logic, 2026 */

#include "gptp_status.h"

#include <string.h>

#include "gptp.h"
#include "ptp_core.h"
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
    PtpGptpStatus st;
    memset(&st, 0, sizeof(st));

    st.version = PTP_GPTP_STATUS_VERSION;
    st.portState = S.bmca.state;
    st.linkUp = S.gptp.linkUp;
    st.domain = S.profile.domainNumber;
    st.locked = S.stats.locked;
    st.ownIdentity = ptp_get_own_clock_identity();

    st.meanLinkDelay_ns = nsI(&S.network.meanPathDelay);
#ifdef PTP_HLT_INTERFACE
    st.tuning_ppb = S.hwclock.tuning_ppb;
#endif
    st.txTimestampsLost = S.stats.drop_cntrs.txts;

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
