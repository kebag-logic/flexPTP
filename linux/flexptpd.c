// SPDX-License-Identifier: MIT
// Copyright Kebag-Logic, 2026
//
// flexptpd.c - flexPTP as a Linux daemon: the gPTP profile on one interface,
// its status published in /dev/shm.
//
//   flexptpd -i IF [-c CONFIG] [-s] [-O NS] [-D PPB] [-P PRIO] [-m NAME] [-q] [-v]
//
// See README.md next to this file for the options and the status block.

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <flexptp/gptp_status.h>
#include <flexptp/logging.h>
#include <flexptp/port/example_netstack_drivers/nsd_linux.h>
#include <flexptp/ptp_profile_presets.h>
#include <flexptp/settings_interface.h>
#include <flexptp/task_ptp.h>

#include "cli/cli.h"
#include "flexptpd_print.h"
#include "flexptpd_shm.h"

// the interface, for PTP_HW_INIT() in flexptp_options.h
char ptp_if_name[IFNAMSIZ] = {0};

// ---- options ----

static struct {
    const char *config;     // -c: file of CLI commands
    bool sw;                // -s: software timestamps, virtual clock
    int64_t swOffset_ns;    // -O: the virtual clock's initial offset
    double swDrift_ppb;     // -D: its free-running drift
    int priority;           // -P: SCHED_FIFO priority, 0: none
    char shmName[NAME_MAX]; // -m: status block name, "" for none
    bool shmNameGiven;      // -m was given
    bool quiet;             // -q: no interactive CLI
    bool verbose;           // -v: the virtual clock's error, once a second
} sOpt;

// ---- status block ----

static FlexptpdShm *sShm = NULL;
static char sShmPath[NAME_MAX + 2];
static pthread_t sPublisher;
static bool sPublisherRunning = false;
static atomic_bool sStopPublisher = false;

#define PUBLISH_PERIOD_NS (20000000) ///< the snapshot is polled every 20 ms

// ---- main loop ----

static bool sExit = false; // the interactive "exit" command

// ---------------------------

static void usage(FILE *to) {
    fprintf(to,
            "usage: flexptpd -i IF [-c CONFIG] [-s] [-O NS] [-D PPB] [-P PRIO] [-m NAME] [-q] [-v]\n"
            "  -i IF      the PTP interface\n"
            "  -c CONFIG  file of CLI commands, one per line, '#' starts a comment; applied\n"
            "             once the core is up and the gPTP preset is loaded\n"
            "  -s         software timestamps and a virtual clock, for interfaces without a PHC\n"
            "  -O NS      the virtual clock's initial offset from CLOCK_REALTIME (0)\n"
            "  -D PPB     the virtual clock's free-running drift (0)\n"
            "  -P PRIO    SCHED_FIFO priority of the daemon's threads (none)\n"
            "  -m NAME    status block in /dev/shm/NAME (flexptpd.IF); -m \"\" for none\n"
            "  -q         no interactive CLI, even on a terminal\n"
            "  -v         with -s: print \"vclock-error-ns N\" once a second\n");
}

// ---- configuration file ----

/**
 * Apply the configuration file: one CLI command per line.
 */
static void apply_config(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        flexptpd_printf("flexptpd: cannot read the configuration %s: %s\n", path, strerror(errno));
        return;
    }

    char *line = NULL;
    size_t cap = 0;

    while (getline(&line, &cap, fp) >= 0) {
        // skip leading whitespace, comments and empty lines
        char *p = line;
        while ((*p == ' ') || (*p == '\t')) {
            p++;
        }

        if ((*p == '#') || (*p == '\n') || (*p == '\r') || (*p == '\0')) {
            continue;
        }

        flexptpd_printf("config: %s", p);
        if (p[strlen(p) - 1] != '\n') {
            flexptpd_printf("\n");
        }

        process_cli_line(p);
    }

    free(line);
    fclose(fp);
}

void flexptpd_user_event_cb(PtpUserEventCode uev) {
    if (uev != PTP_UEV_INIT_DONE) {
        return;
    }

    // the gPTP preset, then the configuration on top of it
    ptp_load_profile(ptp_profile_preset_get("gPTP"));
    ptp_log_enable(PTP_LOG_BMCA, true);

    if (sOpt.config != NULL) {
        apply_config(sOpt.config);
    }
}

// ---- status block ----

static bool shm_create(void) {
    snprintf(sShmPath, sizeof(sShmPath), "/%s", sOpt.shmName);

    int fd = shm_open(sShmPath, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        flexptpd_printf("flexptpd: cannot create /dev/shm%s: %s\n", sShmPath, strerror(errno));
        return false;
    }

    if (ftruncate(fd, sizeof(FlexptpdShm)) < 0) {
        flexptpd_printf("flexptpd: cannot size /dev/shm%s: %s\n", sShmPath, strerror(errno));
        close(fd);
        return false;
    }

    void *p = mmap(NULL, sizeof(FlexptpdShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (p == MAP_FAILED) {
        flexptpd_printf("flexptpd: cannot map /dev/shm%s: %s\n", sShmPath, strerror(errno));
        return false;
    }

    sShm = (FlexptpdShm *)p;

    // the header; seq stays even (no write in progress)
    memset(sShm, 0, sizeof(FlexptpdShm));
    sShm->layout = PTP_GPTP_STATUS_VERSION;
    sShm->size = sizeof(PtpGptpStatus);
    atomic_store_explicit(&sShm->seq, 0, memory_order_relaxed);
    memcpy(sShm->magic, FLEXPTPD_SHM_MAGIC, sizeof(FLEXPTPD_SHM_MAGIC));

    return true;
}

/**
 * Write a status under the seqlock.
 */
static void shm_write(const PtpGptpStatus *pSt) {
    uint32_t seq = atomic_load_explicit(&sShm->seq, memory_order_relaxed);

    // odd: a write is in progress
    atomic_store_explicit(&sShm->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);

    memcpy(&sShm->status, pSt, sizeof(PtpGptpStatus));

    // even again: the status is consistent
    atomic_store_explicit(&sShm->seq, seq + 2, memory_order_release);
}

static void *publisher_thread(void *arg) {
    (void)arg;

    bool first = true;
    uint32_t lastUpdates = 0;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!atomic_load(&sStopPublisher)) {
        PtpGptpStatus st;
        ptp_gptp_get_status(&st);

        if (first || (st.updates != lastUpdates)) {
            shm_write(&st);

            lastUpdates = st.updates;
            first = false;
        }

        next.tv_nsec += PUBLISH_PERIOD_NS;
        if (next.tv_nsec >= 1000000000) {
            next.tv_sec += 1;
            next.tv_nsec -= 1000000000;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    return NULL;
}

static void shm_start(void) {
    if (!shm_create()) {
        return;
    }

    atomic_store(&sStopPublisher, false);

    sPublisherRunning = (pthread_create(&sPublisher, NULL, publisher_thread, NULL) == 0);
    if (!sPublisherRunning) {
        flexptpd_printf("flexptpd: cannot start the status publisher\n");
    }
}

static void shm_stop(void) {
    if (sPublisherRunning) {
        atomic_store(&sStopPublisher, true);
        pthread_join(sPublisher, NULL);
        sPublisherRunning = false;
    }

    if (sShm != NULL) {
        munmap(sShm, sizeof(FlexptpdShm));
        shm_unlink(sShmPath);
        sShm = NULL;
    }
}

// ---- interactive CLI ----

static CMD_FUNCTION(cmd_exit) {
    (void)ppArgs;
    (void)argc;

    sExit = true;
    return 0;
}

// ---- options ----

static bool parse_options(int argc, char *argv[]) {
    int c;
    while ((c = getopt(argc, argv, "i:c:sO:D:P:m:qvh")) != -1) {
        switch (c) {
        case 'i':
            strncpy(ptp_if_name, optarg, IFNAMSIZ - 1);
            break;
        case 'c':
            sOpt.config = optarg;
            break;
        case 's':
            sOpt.sw = true;
            break;
        case 'O':
            sOpt.swOffset_ns = strtoll(optarg, NULL, 0);
            break;
        case 'D':
            sOpt.swDrift_ppb = strtod(optarg, NULL);
            break;
        case 'P':
            sOpt.priority = atoi(optarg);
            break;
        case 'm':
            strncpy(sOpt.shmName, optarg, sizeof(sOpt.shmName) - 1);
            sOpt.shmNameGiven = true;
            break;
        case 'q':
            sOpt.quiet = true;
            break;
        case 'v':
            sOpt.verbose = true;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            return false;
        }
    }

    if (ptp_if_name[0] == '\0') {
        return false;
    }

    // the default status block name follows the interface
    if (!sOpt.shmNameGiven) {
        snprintf(sOpt.shmName, sizeof(sOpt.shmName), "flexptpd.%s", ptp_if_name);
    }

    return true;
}

/**
 * The daemon's threads inherit the main thread's policy: SCHED_FIFO for all,
 * set before any of them starts.
 */
static void apply_priority(void) {
    if (sOpt.priority <= 0) {
        return;
    }

    struct sched_param sp = {.sched_priority = sOpt.priority};

    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
        flexptpd_printf("flexptpd: cannot run at SCHED_FIFO %d: %s\n", sOpt.priority, strerror(errno));
        return;
    }

    linux_nsd_set_rt_priority(sOpt.priority);
}

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(int argc, char *argv[]) {
    if (!parse_options(argc, argv)) {
        usage(stderr);
        return 2;
    }

    // logs: line by line, coloured only on a terminal
    setvbuf(stdout, NULL, _IOLBF, 0);
    flexptpd_print_set_color(isatty(STDOUT_FILENO));

    // SIGINT and SIGTERM end the daemon through a signalfd: blocked in every
    // thread, the main one included, before any thread starts
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stopSignals, NULL);

    int sigFd = signalfd(-1, &stopSignals, SFD_CLOEXEC);
    if (sigFd < 0) {
        flexptpd_printf("flexptpd: signalfd: %s\n", strerror(errno));
        return 1;
    }

    apply_priority();

    if (sOpt.sw) {
        linux_nsd_set_sw_mode(sOpt.swOffset_ns, sOpt.swDrift_ppb);
    }

    // the CLI first: the core registers its commands while it initializes
    cli_init();
    cli_register_command("exit \t\t\tExit flexptpd", 1, 0, cmd_exit);

    if (!reg_task_ptp()) {
        return 1;
    }

    if (sOpt.priority > 0) {
        ptp_task_set_rt_priority(sOpt.priority);
    }

    if (sOpt.shmName[0] != '\0') {
        shm_start();
    }

    // an interactive CLI only on a terminal, and never with -q
    bool interactive = !sOpt.quiet && isatty(STDIN_FILENO);
    if (interactive) {
        flexptpd_printf("Type '?' for help, 'exit' to leave.\n");
    }

    bool vclockLines = sOpt.verbose && sOpt.sw;
    int64_t nextPrint = mono_ns() + 1000000000;

    char *line = NULL;
    size_t cap = 0;

    while (!sExit) {
        struct pollfd pfd[2] = {
            {.fd = sigFd, .events = POLLIN},
            {.fd = interactive ? STDIN_FILENO : -1, .events = POLLIN},
        };

        int timeout = -1;
        if (vclockLines) {
            int64_t left = nextPrint - mono_ns();
            timeout = (left > 0) ? (int)((left + 999999) / 1000000) : 0;
        }

        int pret = poll(pfd, 2, timeout);
        if ((pret < 0) && (errno != EINTR)) {
            break;
        }

        // SIGINT or SIGTERM
        if (pfd[0].revents & POLLIN) {
            break;
        }

        // a command from the terminal; at end of input, stop listening to it
        if (pfd[1].revents & (POLLIN | POLLHUP)) {
            if (getline(&line, &cap, stdin) < 0) {
                interactive = false;
            } else {
                process_cli_line(line);
            }
        }

        if (vclockLines && (mono_ns() >= nextPrint)) {
            flexptpd_printf("vclock-error-ns %" PRId64 "\n", linux_nsd_vclock_error_ns());
            nextPrint += 1000000000;
        }
    }

    free(line);

    // shut down: the PTP task and the transceiver, then the status block
    unreg_task_ptp();
    linux_nsd_cleanup();
    shm_stop();

    close(sigFd);

    flexptpd_printf("flexptpd: stopped\n");

    return 0;
}
