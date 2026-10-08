#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""mock_flexptpd.py - a test double of flexptpd, to exercise the suite itself
while the daemon is being written. It follows the command-line and status
block contract and a simplified 802.1AS end station; it is not a reference
implementation, and passing against it proves the harness, not the standard.

usage: mock_flexptpd.py -i IF [-c CONFIG] [-s] [-O NS] [-D PPB] [-P PRIO] [-m NAME] [-q] [-v]
"""

import argparse
import fcntl
import os
import signal
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gptp                                         # noqa: E402
import status as stmod                              # noqa: E402
from gptp import PortIdentity                       # noqa: E402
from tester import Endpoint, now_ns                 # noqa: E402

IFF_RUNNING = 0x40
SIOCGIFFLAGS = 0x8913


def link_running(ifname: str) -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        req = struct.pack("16sH14x", ifname.encode(), 0)
        flags = struct.unpack("16sH14x", fcntl.ioctl(s.fileno(), SIOCGIFFLAGS, req))[1]
        return bool(flags & IFF_RUNNING)
    finally:
        s.close()


class Mock:
    def __init__(self, a):
        self.a = a
        self.lock = threading.RLock()

        # virtual clock over CLOCK_REALTIME
        self.t0 = now_ns()
        self.offset = a.O
        self.drift = a.D * 1e-9

        # configuration
        self.nbthresh = 800
        self.lostresp = 9
        self.gmcapable = True
        self.rsync = False
        self.rsync_domain = 1
        self.rsync_interval = None              # s, None: a share of the incoming Sync
        self.rsync_pct = 90.0

        self.load_config(a.c)

        self.ep = Endpoint(a.i)
        self.me = self.ep.port(1)

        self.vector = (248, 248, 0xFE, 0x436A, 248, gptp.clock_id_int(self.me.clock), 0)

        # intervals (log2 s); None: stopped
        self.log_pdelay = 0
        self.log_sync = -3
        self.log_ann = 0

        # pdelay
        self.pd_seq = 0
        self.pd = {}                            # seq -> dict of the exchange
        self.lost = 0
        self.nrr_pts = []
        self.mld = 0
        self.nrr = 1.0
        self.as_capable = False
        self.multiple = 0
        self.lost_total = 0

        # BMCA and slave
        self.state = stmod.LISTENING
        self.start_mono = time.monotonic()
        self.foreign = None                     # (vector, sender PortIdentity, expires, log_ann)
        self.last_sync = None
        self.pending_sync = {}
        self.slave_log_sync = -3
        self.gm = self.me.clock
        self.gm_changes = 0
        self.te = 0
        self.sync_rx = 0
        self.sync_to = 0
        self.ann_rx = 0
        self.ann_to = 0
        self.sig_rx = 0
        self.rsync_sent = 0

        self.link = link_running(a.i)
        self.updates = 0

        self.seq_sync = 0
        self.seq_ann = 0
        self.seq_rs = 0

        self.shm_path = "/dev/shm/" + (a.m or "flexptpd." + a.i)
        self.shm_seq = 0

        self.ep.add_handler(self.on_frame)

    # ---- configuration ----

    def load_config(self, path):
        if not path:
            return

        for line in open(path):
            w = line.split()

            if not w or w[0].startswith("#") or w[0] != "ptp":
                continue

            if w[1:3] == ["gptp", "nbthresh"]:
                self.nbthresh = int(w[3])
            elif w[1:3] == ["gptp", "lostresp"]:
                self.lostresp = int(w[3])
            elif w[1:3] == ["gptp", "gmcapable"]:
                self.gmcapable = w[3] == "on"
            elif w[1:2] == ["rsync"]:
                if w[2] in ("on", "off"):
                    self.rsync = w[2] == "on"
                elif w[2] == "domain":
                    self.rsync_domain = int(w[3])
                elif w[2] == "interval":
                    self.rsync_interval = int(w[3]) / 1000.0
                elif w[2] == "rate":
                    self.rsync_pct = float(w[3])

    # ---- the virtual clock ----

    def v(self, t_rt: int) -> int:
        return int(t_rt + self.offset + (t_rt - self.t0) * self.drift)

    # ---- receive ----

    def on_frame(self, cap):
        if cap.direction != "rx" or cap.msg is None:
            return

        m = cap.msg
        h = m.hdr

        if h.source.clock == self.me.clock and cap.type != gptp.PDELAY_RESP and cap.type != gptp.PDELAY_RESP_FU:
            return

        with self.lock:
            if not self.link:
                return

            t = cap.type

            if t == gptp.PDELAY_REQ and h.domain == 0:
                self.respond(cap)
            elif t in (gptp.PDELAY_RESP, gptp.PDELAY_RESP_FU) and m.requesting == self.me:
                ex = self.pd.setdefault(h.seq, {"responders": set()})
                ex["responders"].add(h.source.clock)

                if t == gptp.PDELAY_RESP:
                    ex["t2"] = m.request_receipt
                    ex["t4"] = self.v(cap.ts)
                else:
                    ex["t3"] = m.response_origin
            elif t == gptp.ANNOUNCE and h.domain == 0:
                self.on_announce(m)
            elif t == gptp.SYNC and h.domain == 0:
                self.pending_sync[(h.source.clock, h.source.port, h.seq)] = cap
            elif t == gptp.FOLLOW_UP and h.domain == 0:
                self.on_follow_up(m)
            elif t == gptp.SIGNALING and m.interval_req is not None:
                self.on_signaling(m.interval_req)

    def respond(self, cap):
        req = cap.msg
        t2 = self.v(cap.ts)
        t3 = self.ep.send(gptp.build_pdelay_resp(self.me, req.hdr.seq, t2, req.hdr.source))
        self.ep.send(gptp.build_pdelay_resp_fu(self.me, req.hdr.seq, self.v(t3), req.hdr.source))

    def on_announce(self, m):
        if m.steps_removed >= 255:
            return

        if m.path_trace and self.me.clock in m.path_trace:
            return

        if not self.as_capable:
            return

        self.ann_rx += 1

        vec = m.vector()
        expires = time.monotonic() + 3 * gptp.log_interval_s(m.hdr.log_interval)

        if self.foreign is None or vec <= self.foreign[0] or m.hdr.source == self.foreign[1]:
            self.foreign = (vec, m.hdr.source, expires, m.hdr.log_interval, m.gm_identity)

    def on_follow_up(self, m):
        key = (m.hdr.source.clock, m.hdr.source.port, m.hdr.seq)
        sync = self.pending_sync.pop(key, None)

        if sync is None or self.state != stmod.SLAVE or self.foreign is None:
            return

        if m.hdr.source != self.foreign[1]:
            return

        corr = (sync.msg.hdr.correction + m.hdr.correction) / 65536.0
        gm_time = m.precise_origin + corr + self.mld
        te = self.v(sync.ts) - gm_time

        # a servo that steps: the error is corrected at every Sync
        self.offset -= te
        self.te = int(te)
        self.sync_rx += 1
        self.last_sync = time.monotonic()
        self.slave_log_sync = sync.msg.hdr.log_interval

    def on_signaling(self, req):
        self.sig_rx += 1

        def apply(cur, val, initial):
            if val == gptp.INTERVAL_NO_CHANGE:
                return cur
            if val == gptp.INTERVAL_INITIAL:
                return initial
            if val == gptp.INTERVAL_STOP:
                return None
            return val

        self.log_pdelay = apply(self.log_pdelay, req.link_delay_interval, 0)
        self.log_sync = apply(self.log_sync, req.time_sync_interval, -3)
        self.log_ann = apply(self.log_ann, req.announce_interval, 0)

    # ---- periodic work ----

    def pdelay_round(self):
        # judge the previous exchange
        prev = self.pd.get(self.pd_seq)

        if prev is not None and self.pd_seq:
            complete = all(k in prev for k in ("t1", "t2", "t3", "t4"))
            multiple = len(prev["responders"]) > 1
            looped = self.me.clock in prev["responders"]

            if not complete:
                self.lost += 1
                self.lost_total += 1
            else:
                self.lost = 0
                self.nrr_pts = (self.nrr_pts + [(prev["t3"], prev["t4"])])[-8:]

                if len(self.nrr_pts) >= 2:
                    (a3, a4), (b3, b4) = self.nrr_pts[0], self.nrr_pts[-1]
                    self.nrr = (b3 - a3) / (b4 - a4)

                self.mld = ((prev["t4"] - prev["t1"]) * self.nrr - (prev["t3"] - prev["t2"])) / 2.0

            if multiple:
                self.multiple += 1

            # a complete exchange decides; a lost one only once too many are lost
            if complete:
                self.as_capable = not multiple and not looped and self.mld <= self.nbthresh
            elif self.lost > self.lostresp:
                self.as_capable = False

        self.pd.clear()
        self.pd_seq = (self.pd_seq + 1) & 0xFFFF

        tx = self.ep.send(gptp.build_pdelay_req(self.me, self.pd_seq))
        self.pd[self.pd_seq] = {"t1": self.v(tx), "responders": set()}

    def bmca(self):
        now = time.monotonic()

        if self.foreign is not None and now > self.foreign[2]:
            self.foreign = None
            self.ann_to += 1

        if (self.state == stmod.SLAVE and self.last_sync is not None
                and now - self.last_sync > 3 * gptp.log_interval_s(self.slave_log_sync)):
            self.foreign = None
            self.last_sync = None
            self.sync_to += 1

        if not self.link or not self.as_capable:
            new = stmod.LISTENING
        elif self.foreign is not None and self.foreign[0][:6] < self.vector[:6]:
            new = stmod.SLAVE
        elif now - self.start_mono < 3.0 or not self.gmcapable:
            new = stmod.LISTENING
        else:
            new = stmod.MASTER

        gm = self.foreign[4] if new == stmod.SLAVE else self.me.clock

        if new == stmod.SLAVE and self.state != stmod.SLAVE:
            self.last_sync = now

        if gm != self.gm:
            self.gm_changes += 1
            self.gm = gm

        self.state = new

    def master_sync(self):
        tx = self.ep.send(gptp.build_sync(self.me, self.seq_sync, log_interval=self.log_sync))
        self.ep.send(gptp.build_follow_up(self.me, self.seq_sync, self.v(tx), log_interval=self.log_sync))
        self.seq_sync = (self.seq_sync + 1) & 0xFFFF

    def master_announce(self):
        self.ep.send(gptp.build_announce(self.me, self.seq_ann, log_interval=self.log_ann,
                                         priority1=248, clock_class=248, clock_accuracy=0xFE,
                                         variance=0x436A, priority2=248, path_trace=[self.me.clock]))
        self.seq_ann = (self.seq_ann + 1) & 0xFFFF

    def reverse_sync(self):
        d = self.rsync_domain
        tx = self.ep.send(gptp.build_sync(self.me, self.seq_rs, domain=d))
        self.ep.send(gptp.build_follow_up(self.me, self.seq_rs, self.v(tx), domain=d))
        self.seq_rs = (self.seq_rs + 1) & 0xFFFF
        self.rsync_sent += 1

    def rsync_period(self):
        if self.rsync_interval is not None:
            return self.rsync_interval

        return self.rsync_pct / 100.0 * gptp.log_interval_s(self.slave_log_sync)

    # ---- status block ----

    def publish(self):
        st = stmod.PtpGptpStatus()
        st.version = stmod.LAYOUT
        self.updates += 1
        st.updates = self.updates
        st.portState = self.state
        st.linkUp = self.link
        st.asCapable = self.as_capable
        st.isMeasuringDelay = self.as_capable
        st.gmPresent = 1
        st.locked = self.state == stmod.SLAVE and abs(self.te) < 10_000
        st.ownIdentity = gptp.clock_id_int(self.me.clock)
        st.gmIdentity = gptp.clock_id_int(self.gm)
        st.gmChanges = self.gm_changes
        st.meanLinkDelay_ns = int(self.mld)
        st.neighborRateRatio = self.nrr
        st.rateRatio = 1.0
        st.timeError_ns = self.te
        st.syncRx = self.sync_rx
        st.syncTimeouts = self.sync_to
        st.announceRx = self.ann_rx
        st.announceTimeouts = self.ann_to
        st.pdelayLost = self.lost_total
        st.pdelayMultiple = self.multiple
        st.signalingRx = self.sig_rx
        st.rsyncEnabled = self.rsync
        st.rsyncDomain = self.rsync_domain
        st.rsyncSent = self.rsync_sent

        size = stmod.STATUS_OFFSET + len(bytes(st))

        if not os.path.exists(self.shm_path):
            with open(self.shm_path, "wb") as f:
                f.write(bytes(size))

        with open(self.shm_path, "r+b") as f:
            self.shm_seq += 1
            f.write(bytes(stmod.ShmHeader(stmod.MAGIC.rstrip(b"\0"), stmod.LAYOUT, self.shm_seq, len(bytes(st)), 0)))
            f.write(bytes(st))
            f.seek(0)
            self.shm_seq += 1
            f.write(bytes(stmod.ShmHeader(stmod.MAGIC.rstrip(b"\0"), stmod.LAYOUT, self.shm_seq, len(bytes(st)), 0)))

    # ---- the loop ----

    def run(self, stop: threading.Event):
        nxt = {"pd": 0.0, "sync": 0.0, "ann": 0.0, "rs": 0.0, "v": 0.0}

        while not stop.is_set():
            now = time.monotonic()

            with self.lock:
                link = link_running(self.a.i)

                if link != self.link:
                    self.link = link

                    if not link:
                        self.as_capable = False
                        self.foreign = None
                        self.lost = 0
                        self.nrr_pts = []

                if self.link and self.log_pdelay is not None and now >= nxt["pd"]:
                    self.pdelay_round()
                    nxt["pd"] = max(nxt["pd"] + gptp.log_interval_s(self.log_pdelay), now)

                self.bmca()

                if self.state == stmod.MASTER and self.link:
                    if self.log_sync is not None and now >= nxt["sync"]:
                        self.master_sync()
                        nxt["sync"] = max(nxt["sync"] + gptp.log_interval_s(self.log_sync), now)

                    if self.log_ann is not None and now >= nxt["ann"]:
                        self.master_announce()
                        nxt["ann"] = max(nxt["ann"] + gptp.log_interval_s(self.log_ann), now)

                if self.rsync and self.link and now >= nxt["rs"]:
                    self.reverse_sync()
                    nxt["rs"] = max(nxt["rs"] + self.rsync_period(), now)

                self.publish()

            if self.a.v and now >= nxt["v"]:
                t = now_ns()
                print("vclock-error-ns %d" % (self.v(t) - t), flush=True)
                nxt["v"] = now + 1.0

            stop.wait(0.005)

        self.ep.close()

        try:
            os.unlink(self.shm_path)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", required=True)
    ap.add_argument("-c")
    ap.add_argument("-s", action="store_true")
    ap.add_argument("-O", type=int, default=0)
    ap.add_argument("-D", type=int, default=0)
    ap.add_argument("-P", type=int)
    ap.add_argument("-m")
    ap.add_argument("-q", action="store_true")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    Mock(a).run(stop)


if __name__ == "__main__":
    main()
