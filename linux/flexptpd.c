// SPDX-License-Identifier: MIT
//
// flexptpd.c - flexPTP as a Linux daemon (a first, minimal shell).
//
//   flexptpd -i IF
//
// Runs the gPTP profile on IF until SIGINT or SIGTERM.

#include <getopt.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <flexptp/profiles.h>
#include <flexptp/ptp_profile_presets.h>
#include <flexptp/settings_interface.h>
#include <flexptp/task_ptp.h>

char ptp_if_name[IFNAMSIZ] = {0};

void flexptpd_user_event_cb(PtpUserEventCode uev) {
    if (uev == PTP_UEV_INIT_DONE) {
        ptp_load_profile(ptp_profile_preset_get("gPTP"));
    }
}

static void usage(FILE *to) {
    fprintf(to, "usage: flexptpd -i IF\n");
}

int main(int argc, char *argv[]) {
    int c;
    while ((c = getopt(argc, argv, "i:h")) != -1) {
        switch (c) {
        case 'i':
            strncpy(ptp_if_name, optarg, IFNAMSIZ - 1);
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (ptp_if_name[0] == '\0') {
        usage(stderr);
        return 2;
    }

    // the signals are taken by sigwait() below, not by handlers
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop, NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!reg_task_ptp()) {
        return 1;
    }

    int sig;
    sigwait(&stop, &sig);

    unreg_task_ptp();
    linux_nsd_cleanup();

    return 0;
}
