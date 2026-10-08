// SPDX-License-Identifier: MIT
// Copyright Kebag-Logic, 2026
//
// flexptpd_status.c - flexptpd-status, print flexptpd's status block.
//
//   flexptpd-status [NAME]      NAME in /dev/shm, default flexptpd.eth0
//
// One "key value" pair per line, for scripts. Exit 0, or 1 if the block is
// missing or unreadable.

#include <inttypes.h>
#include <stdio.h>

#include "flexptpd_shm.h"

static const char *PORT_STATES[] = {
    "INITIALIZING",
    "LISTENING",
    "PRE_MASTER",
    "MASTER",
    "SLAVE",
    "PASSIVE",
    "UNCALIBRATED",
    "FAULTY",
    "DISABLED",
};

static void print_identity(const char *key, uint64_t id) {
    printf("%s %02x%02x%02x.%02x%02x.%02x%02x%02x\n",
           key,
           (unsigned)(id >> 56) & 0xFF, (unsigned)(id >> 48) & 0xFF, (unsigned)(id >> 40) & 0xFF,
           (unsigned)(id >> 32) & 0xFF, (unsigned)(id >> 24) & 0xFF,
           (unsigned)(id >> 16) & 0xFF, (unsigned)(id >> 8) & 0xFF, (unsigned)id & 0xFF);
}

int main(int argc, char *argv[]) {
    const char *name = (argc > 1) ? argv[1] : "flexptpd.eth0";

    const FlexptpdShm *pShm = flexptpd_shm_open(name);
    if (pShm == NULL) {
        fprintf(stderr, "flexptpd-status: no status block /dev/shm/%s\n", name);
        return 1;
    }

    PtpGptpStatus st;
    if (!flexptpd_shm_read(pShm, &st)) {
        fprintf(stderr, "flexptpd-status: no consistent copy of /dev/shm/%s\n", name);
        flexptpd_shm_close(pShm);
        return 1;
    }

    const char *state = (st.portState < sizeof(PORT_STATES) / sizeof(PORT_STATES[0]))
                            ? PORT_STATES[st.portState]
                            : "?";

    printf("updates %" PRIu32 "\n", st.updates);
    printf("portState %s\n", state);
    printf("linkUp %u\n", st.linkUp);
    printf("asCapable %u\n", st.asCapable);
    printf("isMeasuringDelay %u\n", st.isMeasuringDelay);
    printf("domain %u\n", st.domain);
    printf("gmPresent %u\n", st.gmPresent);
    printf("locked %u\n", st.locked);
    print_identity("ownIdentity", st.ownIdentity);
    print_identity("gmIdentity", st.gmIdentity);
    printf("gmPriority1 %u\n", st.gmPriority1);
    printf("gmClockClass %u\n", st.gmClockClass);
    printf("gmClockAccuracy 0x%02x\n", st.gmClockAccuracy);
    printf("gmPriority2 %u\n", st.gmPriority2);
    printf("gmVariance 0x%04x\n", st.gmVariance);
    printf("stepsRemoved %u\n", st.stepsRemoved);
    printf("gmTimeBaseIndicator %u\n", st.gmTimeBaseIndicator);
    printf("gmChanges %" PRIu32 "\n", st.gmChanges);
    printf("meanLinkDelay_ns %" PRId64 "\n", st.meanLinkDelay_ns);
    printf("neighborRateRatio %.9f\n", st.neighborRateRatio);
    printf("rateRatio %.9f\n", st.rateRatio);
    printf("timeError_ns %" PRId64 "\n", st.timeError_ns);
    printf("tuning_ppb %.3f\n", st.tuning_ppb);
    printf("logSyncInterval %d\n", st.logSyncInterval);
    printf("logPdelayInterval %d\n", st.logPdelayInterval);
    printf("logAnnounceInterval %d\n", st.logAnnounceInterval);
    printf("syncRx %" PRIu32 "\n", st.syncRx);
    printf("syncTimeouts %" PRIu32 "\n", st.syncTimeouts);
    printf("announceRx %" PRIu32 "\n", st.announceRx);
    printf("announceTimeouts %" PRIu32 "\n", st.announceTimeouts);
    printf("pdelayLost %" PRIu32 "\n", st.pdelayLost);
    printf("pdelayMultiple %" PRIu32 "\n", st.pdelayMultiple);
    printf("signalingRx %" PRIu32 "\n", st.signalingRx);
    printf("txTimestampsLost %" PRIu32 "\n", st.txTimestampsLost);
    printf("rsyncEnabled %u\n", st.rsyncEnabled);
    printf("rsyncDomain %u\n", st.rsyncDomain);
    printf("rsyncInterval_us %" PRIu32 "\n", st.rsyncInterval_us);
    printf("rsyncSent %" PRIu32 "\n", st.rsyncSent);

    flexptpd_shm_close(pShm);

    return 0;
}
