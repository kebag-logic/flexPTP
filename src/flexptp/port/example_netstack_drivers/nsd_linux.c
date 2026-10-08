#include "../../network_stack_driver.h"

#include "../../ptp_defs.h"
#include "../../task_ptp.h"

#include "nsd_linux.h"
#include "nsd_linux_txts.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/errqueue.h>
#include <linux/ethtool.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/netlink.h>
#include <linux/ptp_clock.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <math.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#define LINUX_NSD_TS_DEBUG (0) // timestamp debugging

// ---- configuration (set before linux_nsd_preinit()) ----

static bool sw_mode = false;          // software timestamps and a virtual clock in place of the PHC
static uint32_t txts_timeout_ms = 20; // time a transmit timestamp may take to come back
static int nsd_rt_priority = 0;       // SCHED_FIFO priority of the transceiver thread, 0: none

// ---- sockets ----

// event_fd receives everything over 802.3 (event and general messages) and
// sends the event messages; general_fd only sends. Over IPv4 the pdelay_*
// sockets serve the peer delay group in P2P mode.
static int event_fd = -1;
static int general_fd = -1;
static int pdelay_event_fd = -1;
static int pdelay_general_fd = -1;
static int nl_fd = -1; // netlink, for the link state

// store current settings
static PtpTransportType TP = -1;
static PtpDelayMechanism DM = -1;
static uint8_t primary_dest[ETH_ALEN] = {}; // 802.3 destination of the primary messages
static uint8_t pdelay_dest[ETH_ALEN] = {};  // 802.3 destination of the peer delay messages

static const uint8_t zero_mac[ETH_ALEN] = {};

// interface data
static int if_idx;                     // interface index
static char if_name[IFNAMSIZ];         // name of the interface
static uint8_t if_hwaddr[IFHWADDRLEN]; // hardware address of the interface
static struct sockaddr_in if_ipaddr;   // IP-address of the interface
static bool if_has_ipv4 = false;       // the interface has an IPv4 address

// hardware clock data
#define PHY_FILE_NAME_SIZE (32)
static int phc_index;                          // index of the PHC
static char phc_file_name[PHY_FILE_NAME_SIZE]; // PHC device file name
static int phc_fd = -1;                        // PHC file descriptor
static clockid_t phc_clkid;                    // PHC clock id
static int64_t phc_max_adj_ppb = 0;            // the PHC's frequency adjustment limit, 0: unknown

// transmit timestamps waiting to be matched
static TxtsTable txts;
static pthread_mutex_t txts_lock = PTHREAD_MUTEX_INITIALIZER;

// link state
static pthread_mutex_t link_lock = PTHREAD_MUTEX_INITIALIZER;
static bool link_known = false; // a state has been reported
static bool link_up = false;    // the last state reported

// virtual clock (software-timestamp mode)
static pthread_mutex_t vclock_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t vclock_v_base = 0;         // virtual time at vclock_rt_base [ns]
static int64_t vclock_rt_base = 0;        // CLOCK_REALTIME at the last rebase [ns]
static int64_t vclock_initial_offset = 0; // the offset given at start [ns]
static double vclock_drift_ppb = 0.0;     // free-running frequency error
static double vclock_tuning_ppb = 0.0;    // the servo's tuning

// transception management
static int notif_q[2] = {-1, -1};    // notification queue
static pthread_t transceiver_thread; // thread managing transmission and reception
static bool transceiver_running = false;
static void *nsd_thread(void *arg); // thread function

#define PRINT_HWADDR(a) MSG("%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);

#define NOTIF_QUIT_TRANSCEIVER_THREAD 'Q' // leave the thread
#define NOTIF_WAKE 'W'                    // a deadline was added: recompute the poll timeout

/* FD <-> CLOCKID conversions (man 2 clock_getres, 'Dynamic clocks' section) */
#define CLOCKFD 3
#define FD_TO_CLOCKID(fd) ((clockid_t)((((unsigned int)~fd) << 3) | CLOCKFD))
#define CLOCKID_TO_FD(clk) ((unsigned int)~((clk) >> 3))

#define NS_PER_SEC (1000000000LL)

// ---------------------------

/**
 * Put the interface's name into an ifreq.
 */
static void set_ifr_name(struct ifreq *pIfr) {
    snprintf(pIfr->ifr_name, IFNAMSIZ, "%s", if_name);
}

static void post_notification(char c) {
    if (notif_q[1] >= 0) {
        ssize_t n = write(notif_q[1], &c, 1);
        (void)n;
    }
}

static int64_t clock_ns(clockid_t clk) {
    struct timespec ts;
    clock_gettime(clk, &ts);

    return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

static void ns_to_sec_ns(int64_t t_ns, uint32_t *pSec, uint32_t *pNs) {
    int64_t sec = t_ns / NS_PER_SEC;
    int64_t ns = t_ns % NS_PER_SEC;

    if (ns < 0) {
        ns += NS_PER_SEC;
        sec -= 1;
    }

    *pSec = (uint32_t)sec;
    *pNs = (uint32_t)ns;
}

// ---- virtual clock ----

/**
 * The virtual time at a CLOCK_REALTIME instant (vclock_lock held).
 */
static int64_t vclock_at_locked(int64_t rt_ns) {
    int64_t delta = rt_ns - vclock_rt_base;
    double ppb = vclock_drift_ppb + vclock_tuning_ppb;

    return vclock_v_base + delta + (int64_t)llround((double)delta * ppb * 1e-9);
}

static int64_t vclock_at(int64_t rt_ns) {
    pthread_mutex_lock(&vclock_lock);
    int64_t v = vclock_at_locked(rt_ns);
    pthread_mutex_unlock(&vclock_lock);

    return v;
}

/**
 * Convert a kernel timestamp to the clock the core runs on: the PHC's time
 * as it is, CLOCK_REALTIME into the virtual clock.
 */
static void kernel_ts_to_core(const struct timespec *pTs, uint32_t *pSec, uint32_t *pNs) {
    if (!sw_mode) {
        *pSec = (uint32_t)pTs->tv_sec;
        *pNs = (uint32_t)pTs->tv_nsec;
        return;
    }

    int64_t rt = (int64_t)pTs->tv_sec * NS_PER_SEC + pTs->tv_nsec;

    ns_to_sec_ns(vclock_at(rt), pSec, pNs);
}

// ---- configuration ----

void linux_nsd_set_sw_mode(int64_t initial_offset_ns, double drift_ppb) {
    sw_mode = true;

    pthread_mutex_lock(&vclock_lock);
    vclock_initial_offset = initial_offset_ns;
    vclock_drift_ppb = drift_ppb;
    vclock_tuning_ppb = 0.0;
    vclock_rt_base = clock_ns(CLOCK_REALTIME);
    vclock_v_base = vclock_rt_base + initial_offset_ns;
    pthread_mutex_unlock(&vclock_lock);
}

void linux_nsd_set_txts_timeout_ms(uint32_t timeout_ms) {
    txts_timeout_ms = (timeout_ms > 0) ? timeout_ms : 1;
}

uint32_t linux_nsd_get_txts_timeout_ms(void) {
    return txts_timeout_ms;
}

void linux_nsd_set_rt_priority(int priority) {
    nsd_rt_priority = priority;
}

int64_t linux_nsd_vclock_error_ns(void) {
    if (!sw_mode) {
        return 0;
    }

    int64_t rt = clock_ns(CLOCK_REALTIME);

    return vclock_at(rt) - rt;
}

// ---- link state ----

/**
 * Hand a link state to the core: always when forced, otherwise only a change.
 */
static void report_link_state(bool up, bool force) {
    pthread_mutex_lock(&link_lock);

    bool changed = force || !link_known || (up != link_up);
    link_known = true;
    link_up = up;

    pthread_mutex_unlock(&link_lock);

    if (changed) {
        MSG("Link on %s is %s\n", if_name, up ? "up" : "down");
        ptp_nsd_link_state_changed(up);
    }
}

/**
 * The link state as the interface flags tell it now.
 */
static bool query_link_state(void) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return true;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr);

    bool up = true;
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
    }

    close(fd);

    return up;
}

static int open_netlink(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) {
        MSG("Could not open a netlink socket, the link state will not be followed!\n");
        return -1;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        MSG("Could not bind the netlink socket, the link state will not be followed!\n");
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * Read the pending netlink messages and report a change of our link.
 */
static void receive_netlink(void) {
    char buf[8192];

    for (;;) {
        ssize_t len = recv(nl_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (len <= 0) {
            break;
        }

        int left = (int)len;
        struct nlmsghdr *nlh = (struct nlmsghdr *)buf;

        for (; NLMSG_OK(nlh, left); nlh = NLMSG_NEXT(nlh, left)) {
            if ((nlh->nlmsg_type != RTM_NEWLINK) && (nlh->nlmsg_type != RTM_DELLINK)) {
                continue;
            }

            struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
            if (ifi->ifi_index != if_idx) {
                continue;
            }

            bool up = (nlh->nlmsg_type == RTM_NEWLINK) &&
                      (ifi->ifi_flags & IFF_UP) &&
                      (ifi->ifi_flags & IFF_RUNNING);

            report_link_state(up, false);
        }
    }
}

// ---- preinit and cleanup ----

bool linux_nsd_preinit(const char *ifn) {
    bool init_ok = false;

    // copy interface name
    snprintf(if_name, IFNAMSIZ, "%s", ifn);

    // create dummy socket
    int fd = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        MSG("Could not create a dummy socket to retrieve interface data!\n");
        return false;
    }

    int err;

    // retrieve the network interface index
    // man 7 netdevice
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr);
    err = ioctl(fd, SIOCGIFINDEX, &ifr);
    if (err < 0) {
        MSG("Could not get interface index!\n");
        goto cleanup;
    }
    if_idx = ifr.ifr_ifindex;

    // retrieve the hardware address of the interface
    // man 7 netdevice
    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr);
    err = ioctl(fd, SIOCGIFHWADDR, &ifr);
    if (err < 0) {
        MSG("Failed to retrieve the hardware address!\n");
        goto cleanup;
    }
    memcpy(if_hwaddr, ifr.ifr_hwaddr.sa_data, IFHWADDRLEN);

    // retrieve the IP address of the interface: only the IPv4 transport needs one
    // man 7 netdevice
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_addr.sa_family = AF_INET;
    set_ifr_name(&ifr);
    err = ioctl(fd, SIOCGIFADDR, &ifr);
    if_has_ipv4 = (err == 0);
    if (if_has_ipv4) {
        memcpy(&if_ipaddr, &ifr.ifr_addr, sizeof(struct sockaddr_in));
    }

    // print collected information
    MSG("Network Interface information\n");
    MSG("-- Interface: " PTP_COLOR_YELLOW "%s\n" PTP_COLOR_RESET, if_name);
    MSG("   Hardware address: " PTP_COLOR_CYAN);
    PRINT_HWADDR(if_hwaddr);
    MSG("\n");
    MSG(PTP_COLOR_RESET);
    if (if_has_ipv4) {
        MSG("   IP-address: " PTP_COLOR_CYAN "%s\n" PTP_COLOR_RESET, inet_ntoa(if_ipaddr.sin_addr));
    } else {
        MSG("   IP-address: none (only the 802.3 transport is available)\n");
    }

    // check the timestamp support
    // https://docs.kernel.org/networking/ethtool-netlink.html#tsinfo-get
    struct ethtool_ts_info tsi = {.cmd = ETHTOOL_GET_TS_INFO};
    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr);
    ifr.ifr_data = (caddr_t)&tsi;
    err = ioctl(fd, SIOCETHTOOL, &ifr);
    if (err < 0) {
        MSG("Failed to query the interface timestamp capabilities!\n");
        goto cleanup;
    }

    if (sw_mode) {
        // software timestamps, on a virtual clock
        MSG("   Timestamping: " PTP_COLOR_GREEN "software" PTP_COLOR_RESET
            ", virtual clock offset %" PRId64 " ns, drift %.1f ppb\n",
            vclock_initial_offset,
            vclock_drift_ppb);

        if (!(tsi.so_timestamping & SOF_TIMESTAMPING_TX_SOFTWARE)) {
            MSG("   (the interface does not announce software transmit timestamps)\n");
        }
    } else {
        // hardware timestamps, on the PHC
        MSG("   Hardware timestamping: ");

        bool hw_ok = (tsi.so_timestamping & SOF_TIMESTAMPING_TX_HARDWARE) &&
                     (tsi.so_timestamping & SOF_TIMESTAMPING_RX_HARDWARE) &&
                     (tsi.phc_index >= 0);

        if (!hw_ok) {
            MSG(PTP_COLOR_RED "MISSING\n" PTP_COLOR_RESET);
            goto cleanup;
        }

        phc_index = tsi.phc_index;
        FLEXPTP_SNPRINTF(phc_file_name, PHY_FILE_NAME_SIZE, "/dev/ptp%d", phc_index);

        MSG(PTP_COLOR_GREEN "OK\n" PTP_COLOR_RESET);
        MSG("   PHC index: " PTP_COLOR_CYAN "%d\n" PTP_COLOR_RESET, phc_index);
        MSG("    -- corresponding file: " PTP_COLOR_CYAN "%s\n" PTP_COLOR_RESET, phc_file_name);

        // open PHC
        phc_fd = open(phc_file_name, O_RDWR | O_CLOEXEC);
        if (phc_fd < 0) {
            MSG("Failed to open PHC file!\n");
            goto cleanup;
        }
        phc_clkid = FD_TO_CLOCKID(phc_fd);

        struct timespec ts;
        if (clock_gettime(phc_clkid, &ts) < 0) {
            MSG("Failed to access the PHC time!\n");
            goto cleanup;
        }

        // the PHC's frequency limit, to keep the servo's tuning within it
        struct ptp_clock_caps caps;
        memset(&caps, 0, sizeof(caps));
        if (ioctl(phc_fd, PTP_CLOCK_GETCAPS, &caps) == 0) {
            phc_max_adj_ppb = caps.max_adj;
            MSG("    -- frequency adjustment limit: %" PRId64 " ppb\n", phc_max_adj_ppb);
        }
    }
    MSG("---------------\n\n");

    // create the notification queue
    if (pipe2(notif_q, O_CLOEXEC | O_NONBLOCK) < 0) {
        MSG("Failed to create notification queue!\n");
        goto cleanup;
    }

    // follow the link state
    nl_fd = open_netlink();

    // no messages are waiting for their timestamps yet
    pthread_mutex_lock(&txts_lock);
    txts_init(&txts);
    pthread_mutex_unlock(&txts_lock);

    // initialization done
    init_ok = true;

cleanup:
    close(fd);

    if (!init_ok && (phc_fd >= 0)) {
        close(phc_fd);
        phc_fd = -1;
    }

    return init_ok;
}

void linux_nsd_cleanup(void) {
    if (phc_fd >= 0) {
        close(phc_fd);
        phc_fd = -1;
    }

    if (nl_fd >= 0) {
        close(nl_fd);
        nl_fd = -1;
    }

    for (int i = 0; i < 2; i++) {
        if (notif_q[i] >= 0) {
            close(notif_q[i]);
            notif_q[i] = -1;
        }
    }
}

// ---- sockets ----

static void socket_join_igmp(int fd, in_addr_t igmp_addr) {
    // fill in the multicast assignment request
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = igmp_addr;
    mreq.imr_interface = if_ipaddr.sin_addr;

    // join the IGMP group
    // man 7 ip
    int err = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    if (err < 0) {
        // MSG("Could not join the required network group!\n");
    }
}

void ptp_nsd_igmp_join_leave(bool join) {
    // only join IGMP if Transport Type is IP
    if ((TP == PTP_TP_IPv4) && join) {
        if (event_fd >= 0) {
            socket_join_igmp(event_fd, PTP_IGMP_PRIMARY);
        }
        if (general_fd >= 0) {
            socket_join_igmp(general_fd, PTP_IGMP_PRIMARY);
        }
        if (pdelay_event_fd >= 0) {
            socket_join_igmp(pdelay_event_fd, PTP_IGMP_PEER_DELAY);
        }
        if (pdelay_general_fd >= 0) {
            socket_join_igmp(pdelay_general_fd, PTP_IGMP_PEER_DELAY);
        }
    }

    // don't have to explicitly leave the IGMP group
}

static int open_udp_socket(in_addr_t s_addr, uint16_t port, const char *hint) {
    // prepare socket address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = PF_INET;
    addr.sin_addr.s_addr = s_addr;

    // create socket
    // man 2 socket
    int sfd = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (sfd < 0) {
        MSG("Could not open the %s socket!\n", hint);
        return -1;
    }

    // enable the reuseaddr option
    // man 3 setsockopt
    int optval = 1;
    int err = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (err < 0) {
        MSG("Could not set the %s socket REUSEADDR option!\n", hint);
        goto cleanup;
    }

    // assign the multicast transmit interface with the socket
    // man 7 ip
    err = setsockopt(sfd, IPPROTO_IP, IP_MULTICAST_IF, &if_ipaddr.sin_addr, sizeof(struct in_addr));
    if (err < 0) {
        MSG("Could not set the %s socket IP_MULTICAST_IF option!\n", hint);
        goto cleanup;
    }

    // bind the socket
    // man 2 bind
    addr.sin_port = htons(port);
    err = bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
    if (err < 0) {
        MSG("Could not bind the %s socket!\n", hint);
        goto cleanup;
    }

    // normal return
    return sfd;

cleanup:
    close(sfd);
    return -1;
}

/**
 * Open the packet socket that receives every PTP frame of the interface and
 * sends the event messages, member of both multicast groups.
 */
static int open_raw_event_socket(void) {
    // SOCK_DGRAM: the kernel fills in the Ethernet header
    int sfd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(PTP_ETHERTYPE));
    if (sfd < 0) {
        MSG("Could not open the EVENT socket!\n");
        return -1;
    }

    // receive from our interface only
    // man 7 packet
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(PTP_ETHERTYPE);
    addr.sll_ifindex = if_idx;

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        MSG("Could not bind the EVENT socket!\n");
    }

    // let both groups through the interface's multicast filter
    const uint8_t *groups[2] = {primary_dest, pdelay_dest};
    int nGroups = (memcmp(primary_dest, pdelay_dest, ETH_ALEN) != 0) ? 2 : 1;

    for (int i = 0; i < nGroups; i++) {
        struct packet_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.mr_ifindex = if_idx;
        mreq.mr_type = PACKET_MR_MULTICAST;
        mreq.mr_alen = ETH_ALEN;
        memcpy(mreq.mr_address, groups[i], ETH_ALEN);

        if (setsockopt(sfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            MSG("Could not join a multicast group on the EVENT socket!\n");
        }
    }

    return sfd;
}

/**
 * Open the packet socket that only sends (protocol 0: it receives nothing).
 */
static int open_raw_general_socket(void) {
    int sfd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) {
        MSG("Could not open the GENERAL socket!\n");
    }

    return sfd;
}

static void enable_timestamping(int sfd) {
    if (sfd < 0) {
        return;
    }

    // ask for timestamps on the socket
    // https://www.kernel.org/doc/html/latest/networking/timestamping.html#scm-timestamping-records
    int optval;
    if (sw_mode) {
        optval = SOF_TIMESTAMPING_RX_SOFTWARE |
                 SOF_TIMESTAMPING_TX_SOFTWARE |
                 SOF_TIMESTAMPING_SOFTWARE;
    } else {
        optval = SOF_TIMESTAMPING_RX_HARDWARE |
                 SOF_TIMESTAMPING_TX_HARDWARE |
                 SOF_TIMESTAMPING_RAW_HARDWARE;
    }

    if (setsockopt(sfd, SOL_SOCKET, SO_TIMESTAMPING, &optval, sizeof(optval)) < 0) {
        MSG("Failed to enable timestamping\n");
    }

    // turn on the hardware's TX and RX timestamping
    // https://www.kernel.org/doc/html/latest/networking/timestamping.html#hardware-timestamping-configuration-ethtool-msg-tsconfig-set-get
    if (!sw_mode) {
        // the PTP event filters first: some drivers (am65-cpsw) refuse FILTER_ALL
        const int filters[3] = {
            HWTSTAMP_FILTER_PTP_V2_EVENT,
            HWTSTAMP_FILTER_PTP_V2_L2_EVENT,
            HWTSTAMP_FILTER_ALL,
        };

        bool set = false;
        for (int i = 0; (i < 3) && !set; i++) {
            struct hwtstamp_config cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.tx_type = HWTSTAMP_TX_ON;
            cfg.rx_filter = filters[i];

            struct ifreq ifreq;
            memset(&ifreq, 0, sizeof(ifreq));
            set_ifr_name(&ifreq);
            ifreq.ifr_data = (void *)&cfg;

            set = (ioctl(sfd, SIOCSHWTSTAMP, &ifreq) == 0);
        }

        if (!set) {
            MSG("Failed to set timestamping settings.\n");
        }
    }

    // signal transmit timestamps on the error queue with POLLPRI
    // man 7 socket
    optval = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_SELECT_ERR_QUEUE, &optval, sizeof(optval)) < 0) {
        MSG("Could not enable TX timestamp communication through the error queue!\n");
    }
}

static void close_fd(int *pFd) {
    if (*pFd >= 0) {
        close(*pFd);
        *pFd = -1;
    }
}

// ---- the transceiver thread ----

/**
 * The kernel timestamp in a message's control data: ts[0] is the software
 * one, ts[2] the raw hardware one.
 */
static bool find_timestamp(struct msghdr *pMsg, struct timespec *pTs) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(pMsg); cm != NULL; cm = CMSG_NXTHDR(pMsg, cm)) {
        if ((cm->cmsg_level != SOL_SOCKET) || (cm->cmsg_type != SO_TIMESTAMPING)) {
            continue;
        }

        struct scm_timestamping tss;
        memcpy(&tss, CMSG_DATA(cm), sizeof(tss));

        *pTs = sw_mode ? tss.ts[0] : tss.ts[2];

        return (pTs->tv_sec != 0) || (pTs->tv_nsec != 0);
    }

    return false;
}

#define FRAME_BUF_SIZE (1600)
#define CTRL_BUF_SIZE (512)

/**
 * Hand every transmit timestamp waiting on a socket's error queue to the
 * message it belongs to.
 */
static void drain_error_queue(int fd) {
    uint8_t frame[FRAME_BUF_SIZE];
    char ctrl[CTRL_BUF_SIZE];

    for (;;) {
        struct iovec iov = {frame, sizeof(frame)};
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);

        ssize_t size = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (size < 0) {
            break;
        }

        struct timespec ts;
        if (!find_timestamp(&msg, &ts)) {
            continue;
        }

        // which message is it?
        uint32_t uid = 0;

        pthread_mutex_lock(&txts_lock);
        bool matched = txts_match(&txts, frame, (size_t)size, &uid);
        pthread_mutex_unlock(&txts_lock);

        if (!matched) {
            // late (already reported lost) or not ours
            CLILOG(LINUX_NSD_TS_DEBUG, "TX TS without a waiting message: %ld.%09ld\n", (long)ts.tv_sec, ts.tv_nsec);
            continue;
        }

        uint32_t sec;
        uint32_t ns;
        kernel_ts_to_core(&ts, &sec, &ns);

        CLILOG(LINUX_NSD_TS_DEBUG, "TX TS: (%u) %u.%09u\n", uid, sec, ns);

        ptp_transmit_timestamp_cb(uid, sec, ns);
    }
}

/**
 * Hand every received message on a socket to the core.
 *
 * @param fd socket
 * @param needTs forward only the messages that carry a timestamp
 */
static void receive_messages(int fd, bool needTs) {
    uint8_t buf[FRAME_BUF_SIZE];
    char ctrl[CTRL_BUF_SIZE];

    for (;;) {
        struct iovec iov = {buf, sizeof(buf)};
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);

        ssize_t size = recvmsg(fd, &msg, MSG_DONTWAIT);
        if (size < 0) {
            break;
        }

        struct timespec ts;
        memset(&ts, 0, sizeof(ts));
        bool tsFound = find_timestamp(&msg, &ts);

        if (needTs && !tsFound) {
            continue;
        }

        uint32_t sec = 0;
        uint32_t ns = 0;
        if (tsFound) {
            kernel_ts_to_core(&ts, &sec, &ns);
        }

        CLILOG(LINUX_NSD_TS_DEBUG, "RX TS: %u.%09u\n", sec, ns);

        ptp_receive_enqueue(buf, (uint32_t)size, sec, ns, TP);
    }
}

/**
 * Report the messages whose transmit timestamps are overdue as lost.
 */
static void expire_overdue(void) {
    uint32_t uids[TXTS_TABLE_SIZE];

    pthread_mutex_lock(&txts_lock);
    int n = txts_expire(&txts, clock_ns(CLOCK_MONOTONIC), uids, TXTS_TABLE_SIZE);
    pthread_mutex_unlock(&txts_lock);

    for (int i = 0; i < n; i++) {
        CLILOG(LINUX_NSD_TS_DEBUG, "TX TS lost: (%u)\n", uids[i]);
        ptp_transmit_timestamp_lost(uids[i]);
    }
}

/**
 * The poll timeout until the earliest transmit timestamp deadline.
 */
static int poll_timeout_ms(void) {
    int64_t deadline = 0;

    pthread_mutex_lock(&txts_lock);
    bool any = txts_next_deadline(&txts, &deadline);
    pthread_mutex_unlock(&txts_lock);

    if (!any) {
        return -1;
    }

    int64_t left_ns = deadline - clock_ns(CLOCK_MONOTONIC);
    if (left_ns <= 0) {
        return 0;
    }

    // round up: waking early would only poll again
    return (int)((left_ns + 999999) / 1000000);
}

/**
 * Handle the events of one socket: transmit timestamps (POLLPRI, and POLLERR
 * with SO_SELECT_ERR_QUEUE), received messages (POLLIN), and a pending socket
 * error (POLLERR alone, e.g. ENETDOWN after the link went away), which must be
 * consumed lest poll() keep returning at once.
 */
static void handle_socket(int fd, short revents, bool timestamped, bool needTs) {
    if (fd < 0) {
        return;
    }

    if (timestamped && (revents & (POLLPRI | POLLERR))) {
        drain_error_queue(fd);
    }

    if (revents & POLLERR) {
        int soErr = 0;
        socklen_t soLen = sizeof(soErr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soLen);
    }

    if (revents & POLLIN) {
        receive_messages(fd, needTs);
    }
}

static void *nsd_thread(void *arg) {
    (void)arg;

    enum {
        PFD_NOTIF,
        PFD_NETLINK,
        PFD_EVENT,
        PFD_GENERAL,
        PFD_PDEL_EVENT,
        PFD_PDEL_GENERAL,
        PFD_N
    };

    // the general sockets receive only over IPv4; the 802.3 one only sends
    bool ipv4 = (TP == PTP_TP_IPv4);

    bool run = true;
    while (run) {
        struct pollfd pfd[PFD_N] = {
            [PFD_NOTIF] = {.fd = notif_q[0], .events = POLLIN},
            [PFD_NETLINK] = {.fd = nl_fd, .events = POLLIN},
            [PFD_EVENT] = {.fd = event_fd, .events = POLLIN | POLLPRI},
            [PFD_GENERAL] = {.fd = ipv4 ? general_fd : -1, .events = POLLIN},
            [PFD_PDEL_EVENT] = {.fd = pdelay_event_fd, .events = POLLIN | POLLPRI},
            [PFD_PDEL_GENERAL] = {.fd = ipv4 ? pdelay_general_fd : -1, .events = POLLIN},
        };

        int pret = poll(pfd, PFD_N, poll_timeout_ms());

        if (pret > 0) {
            // notifications
            if (pfd[PFD_NOTIF].revents & POLLIN) {
                char c;
                while (read(notif_q[0], &c, sizeof(char)) == 1) {
                    if (c == NOTIF_QUIT_TRANSCEIVER_THREAD) {
                        run = false;
                    }
                }

                if (!run) {
                    continue;
                }
            }

            // link state
            if (pfd[PFD_NETLINK].revents & POLLIN) {
                receive_netlink();
            }

            // event messages: transmit timestamps and reception; over IPv4
            // only the timestamped ones are forwarded
            handle_socket(event_fd, pfd[PFD_EVENT].revents, true, ipv4);
            handle_socket(pdelay_event_fd, pfd[PFD_PDEL_EVENT].revents, true, ipv4);

            // general message reception (in IPv4 mode)
            if (ipv4) {
                handle_socket(general_fd, pfd[PFD_GENERAL].revents, false, false);
                handle_socket(pdelay_general_fd, pfd[PFD_PDEL_GENERAL].revents, false, false);
            }
        }

        // the timestamps that will not come
        expire_overdue();
    }

    return NULL;
}

/**
 * Start the transceiver thread, at the configured SCHED_FIFO priority if any.
 */
static void start_transceiver_thread(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    if (nsd_rt_priority > 0) {
        struct sched_param sp = {.sched_priority = nsd_rt_priority};

        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
    }

    int err = pthread_create(&transceiver_thread, &attr, nsd_thread, NULL);

    if ((err != 0) && (nsd_rt_priority > 0)) {
        // no permission for SCHED_FIFO: run it anyway
        MSG("Could not start the transceiver thread at SCHED_FIFO %d, using the default policy!\n", nsd_rt_priority);
        err = pthread_create(&transceiver_thread, NULL, nsd_thread, NULL);
    }

    pthread_attr_destroy(&attr);

    transceiver_running = (err == 0);
    if (!transceiver_running) {
        MSG("Failed to create the transceiver thread!\n");
    }
}

/**
 * Report every message still waiting for its timestamp as lost.
 */
static void flush_waiting(void) {
    uint32_t uids[TXTS_TABLE_SIZE];

    pthread_mutex_lock(&txts_lock);
    int n = txts_flush(&txts, uids, TXTS_TABLE_SIZE);
    pthread_mutex_unlock(&txts_lock);

    for (int i = 0; i < n; i++) {
        ptp_transmit_timestamp_lost(uids[i]);
    }
}

void ptp_nsd_init(const NsdInitSettings *init) {
    // leave current IGMP group if applicable
    ptp_nsd_igmp_join_leave(false);

    // first, stop the thread and close the sockets
    if (transceiver_running) {
        post_notification(NOTIF_QUIT_TRANSCEIVER_THREAD);
        pthread_join(transceiver_thread, NULL);
        transceiver_running = false;
    }

    close_fd(&event_fd);
    close_fd(&general_fd);
    close_fd(&pdelay_event_fd);
    close_fd(&pdelay_general_fd);

    // timestamps of the closed sockets will not come any more
    flush_waiting();

    // calling either parameter with -1 just closes connections
    if (((int)init->tp == -1) || ((int)init->dm == -1)) {
        // message transmission and reception is turned off
        TP = -1;
        DM = -1;
        return;
    }

    // the profile's 802.3 destinations, or the standard ones
    if (memcmp(init->primary_p2p_8023_dest, zero_mac, ETH_ALEN) != 0) {
        memcpy(primary_dest, init->primary_p2p_8023_dest, ETH_ALEN);
    } else {
        memcpy(primary_dest, PTP_ETHERNET_PRIMARY, ETH_ALEN);
    }

    if (memcmp(init->pdelay_p2p_8023_dest, zero_mac, ETH_ALEN) != 0) {
        memcpy(pdelay_dest, init->pdelay_p2p_8023_dest, ETH_ALEN);
    } else {
        memcpy(pdelay_dest, PTP_ETHERNET_PEER_DELAY, ETH_ALEN);
    }

    // store the configuration before the thread reads it
    TP = init->tp;
    DM = init->dm;

    // open event and general connections
    if (init->tp == PTP_TP_IPv4) {
        if (!if_has_ipv4) {
            MSG("The IPv4 transport needs an IPv4 address on %s!\n", if_name);
        }

        event_fd = open_udp_socket(PTP_IGMP_PRIMARY, PTP_PORT_EVENT, "EVENT_PRIMARY");
        general_fd = open_udp_socket(PTP_IGMP_PRIMARY, PTP_PORT_GENERAL, "GENERAL_PRIMARY");

        if (init->dm == PTP_DM_P2P) {
            pdelay_event_fd = open_udp_socket(PTP_IGMP_PEER_DELAY, PTP_PORT_EVENT, "EVENT_PDELAY");
            pdelay_general_fd = open_udp_socket(PTP_IGMP_PEER_DELAY, PTP_PORT_GENERAL, "GENERAL_PDELAY");
        }
    } else if (init->tp == PTP_TP_802_3) {
        event_fd = open_raw_event_socket();
        general_fd = open_raw_general_socket();
    }

    // timestamps on the event sockets
    enable_timestamping(event_fd);
    enable_timestamping(pdelay_event_fd);

    // create the transceiver thread
    start_transceiver_thread();

    // join new IGMP group
    ptp_nsd_igmp_join_leave(true);

    // tell the core where the link stands
    report_link_state(query_link_state(), true);
}

void ptp_nsd_transmit_msg(RawPtpMessage *pMsg, uint32_t uid) {
    if (pMsg == NULL) {
        return;
    }

    // get the message class
    PtpMessageClass mc = pMsg->tx_mc;
    PtpMessageType mt = pMsg->tx_mt;
    bool event = (mc == PTP_MC_EVENT);

    // is it a Peer Delay Mechanism related message?
    bool isPDel_ = (mt == PTP_MT_PDelay_Req) ||
                   (mt == PTP_MT_PDelay_Resp) ||
                   (mt == PTP_MT_PDelay_Resp_Follow_Up);

    // an event message waits for its timestamp: registered before it is
    // sent, so that a fast timestamp cannot overtake the registration
    if (event) {
        int64_t deadline = clock_ns(CLOCK_MONOTONIC) + (int64_t)txts_timeout_ms * 1000000;
        uint32_t evictedUid = 0;
        bool evicted = false;

        pthread_mutex_lock(&txts_lock);
        bool added = txts_add(&txts, uid, pMsg->data, pMsg->size, deadline, &evictedUid, &evicted);
        pthread_mutex_unlock(&txts_lock);

        if (evicted) {
            ptp_transmit_timestamp_lost(evictedUid);
        }

        if (!added) {
            ptp_transmit_timestamp_lost(uid);
            return;
        }

        // the thread recomputes its poll timeout
        post_notification(NOTIF_WAKE);
    }

    // indicates if the transmission was successful
    bool send_ok = false;
    int sfd = event ? event_fd : general_fd;

    // narrow down by transport type
    if (TP == PTP_TP_IPv4) {
        // configure the address: group by message, port by class
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = PF_INET;
        addr.sin_addr.s_addr = isPDel_ ? PTP_IGMP_PEER_DELAY : PTP_IGMP_PRIMARY;
        addr.sin_port = htons(event ? PTP_PORT_EVENT : PTP_PORT_GENERAL);

        // send packet
        ssize_t sent = sendto(sfd, pMsg->data, pMsg->size, 0, (struct sockaddr *)&addr, sizeof(addr));
        send_ok = (sent == (ssize_t)pMsg->size);
    } else if (TP == PTP_TP_802_3) {
        // destination by message: the peer delay group or the primary one
        struct sockaddr_ll addr;
        memset(&addr, 0, sizeof(addr));
        addr.sll_family = AF_PACKET;
        addr.sll_ifindex = if_idx;
        addr.sll_halen = ETH_ALEN;
        addr.sll_protocol = htons(PTP_ETHERTYPE);
        memcpy(addr.sll_addr, isPDel_ ? pdelay_dest : primary_dest, ETH_ALEN);

        ssize_t sent = sendto(sfd, pMsg->data, pMsg->size, 0, (struct sockaddr *)&addr, sizeof(addr));
        send_ok = (sent == (ssize_t)pMsg->size);
    }

    // a general message has no timestamp to wait for; a failed one has none either
    if (event && !send_ok) {
        pthread_mutex_lock(&txts_lock);
        txts_remove(&txts, uid);
        pthread_mutex_unlock(&txts_lock);

        ptp_transmit_timestamp_lost(uid);
    } else if (!event) {
        if (send_ok) {
            ptp_transmit_timestamp_cb(uid, 0, 0);
        } else {
            ptp_transmit_timestamp_lost(uid);
        }
    }
}

void ptp_nsd_get_interface_address(uint8_t *hwa) {
    memcpy(hwa, if_hwaddr, IFHWADDRLEN);
}

// ------------------------

/* man 2 clock_adjtime (ADJ_FREQUENCY) */
#define PPB_TO_TUNING_SCALER (((double)(1 << 16)) / 1000.0)

void linux_adjust_clock(double tuning_ppb) {
    if (sw_mode) {
        // rebase, so the virtual time stays continuous across the change
        pthread_mutex_lock(&vclock_lock);

        int64_t rt = clock_ns(CLOCK_REALTIME);
        vclock_v_base = vclock_at_locked(rt);
        vclock_rt_base = rt;
        vclock_tuning_ppb = tuning_ppb;

        pthread_mutex_unlock(&vclock_lock);
        return;
    }

    // keep within the PHC's limit, which the kernel would refuse
    if ((phc_max_adj_ppb > 0) && (fabs(tuning_ppb) > (double)phc_max_adj_ppb)) {
        tuning_ppb = (tuning_ppb > 0) ? (double)phc_max_adj_ppb : -(double)phc_max_adj_ppb;
    }

    struct timex tx;
    memset(&tx, 0, sizeof(struct timex));
    tx.modes = ADJ_FREQUENCY;
    tx.freq = (long)(tuning_ppb * PPB_TO_TUNING_SCALER);

    if (clock_adjtime(phc_clkid, &tx) < 0) {
        MSG("Failed to adjust PHC frequency!\n");
    }
}

void linux_set_time(uint32_t seconds, uint32_t nanoseconds) {
    if (sw_mode) {
        pthread_mutex_lock(&vclock_lock);
        vclock_rt_base = clock_ns(CLOCK_REALTIME);
        vclock_v_base = (int64_t)seconds * NS_PER_SEC + nanoseconds;
        pthread_mutex_unlock(&vclock_lock);
        return;
    }

    struct timespec ts = {.tv_sec = seconds, .tv_nsec = nanoseconds};
    if (clock_settime(phc_clkid, &ts) < 0) {
        MSG("Failed to set the PHC time!\n");
    }
}

void linux_get_time(TimestampU *pTime) {
    if (sw_mode) {
        int64_t v = vclock_at(clock_ns(CLOCK_REALTIME));

        pTime->sec = (uint64_t)(v / NS_PER_SEC);
        pTime->nanosec = (uint32_t)(v % NS_PER_SEC);
        return;
    }

    struct timespec ts;
    if (clock_gettime(phc_clkid, &ts) < 0) {
        MSG("Failed to get the PHC time!\n");
    }

    pTime->sec = ts.tv_sec;
    pTime->nanosec = ts.tv_nsec;
}
