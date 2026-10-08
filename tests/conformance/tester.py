# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""tester.py - an IEEE 802.1AS tester on one Ethernet interface, with software
timestamps (SO_TIMESTAMPING) on CLOCK_REALTIME, which is the tester's clock.

An Endpoint receives every gPTP frame into a capture (and a pcap file, for
evidence), and sends frames with their transmit timestamps. Roles plug into
it and can be combined:

    GrandMaster      Announce (any priority vector, path trace) and two-step
                     Sync/Follow_Up with the information TLV
    PdelayResponder  answers Pdelay_Req: an extra link delay, a clock rate,
                     dropped responses, a second responder, a borrowed identity
    PdelayInitiator  measures the peer's responder
    ReverseSyncReceiver
                     pairs Sync/Follow_Up of the reverse sync domain and gives
                     the time error of the sender (Avnu egress method)
    send_signaling() a message interval request

The receive thread runs the roles' handlers; a handler may send.
"""

import os
import select
import socket
import struct
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

import gptp
from gptp import PortIdentity

# ---- socket constants (linux/net_tstamp.h, linux/if_packet.h) -------------

SO_TIMESTAMPING = 37
SOF_TIMESTAMPING_TX_SOFTWARE = 1 << 1
SOF_TIMESTAMPING_RX_SOFTWARE = 1 << 3
SOF_TIMESTAMPING_SOFTWARE = 1 << 4

SOL_PACKET = 263
PACKET_ADD_MEMBERSHIP = 1
PACKET_MR_MULTICAST = 0

MSG_ERRQUEUE = 0x2000

TX_TS_TIMEOUT_S = 0.020


def now_ns() -> int:
    return time.clock_gettime_ns(time.CLOCK_REALTIME)


def _scm_ts(ancdata) -> Optional[int]:
    """The software timestamp (ts[0]) of a SCM_TIMESTAMPING record."""
    for level, kind, data in ancdata:
        if level == socket.SOL_SOCKET and kind == SO_TIMESTAMPING and len(data) >= 16:
            sec, nsec = struct.unpack("qq", data[:16])
            return sec * 1_000_000_000 + nsec

    return None


# ---- capture ---------------------------------------------------------------

@dataclass
class Captured:
    ts: int                  # ns, CLOCK_REALTIME (software timestamp)
    direction: str           # "rx" or "tx"
    raw: bytes               # the PTP message
    msg: object              # parsed, or None if it did not parse
    error: str = ""

    @property
    def type(self) -> Optional[int]:
        return self.msg.hdr.msg_type if self.msg is not None else None

    @property
    def domain(self) -> Optional[int]:
        return self.msg.hdr.domain if self.msg is not None else None


class PcapWriter:
    """Nanosecond pcap, Ethernet link type."""

    def __init__(self, path: str):
        self.f = open(path, "wb")
        self.lock = threading.Lock()

        self.f.write(struct.pack("<IHHiIII", 0xA1B23C4D, 2, 4, 0, 0, 65535, 1))

    def write(self, ts: int, frame: bytes) -> None:
        with self.lock:
            sec, nsec = divmod(ts, 1_000_000_000)

            self.f.write(struct.pack("<IIII", sec, nsec, len(frame), len(frame)))
            self.f.write(frame)

    def close(self) -> None:
        with self.lock:
            self.f.close()


# ---- the endpoint ----------------------------------------------------------

class Endpoint:
    def __init__(self, ifname: str, pcap_path: Optional[str] = None):
        self.ifname = ifname

        # receive: every gPTP frame, with its software receive timestamp
        self.rx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(gptp.ETH_P_1588))
        self.rx.bind((ifname, gptp.ETH_P_1588))

        mreq = struct.pack(
            "iHH8s",
            socket.if_nametoindex(ifname),
            PACKET_MR_MULTICAST,
            6,
            gptp.GPTP_MC + b"\0\0",
        )
        self.rx.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)

        self.rx.setsockopt(
            socket.SOL_SOCKET,
            SO_TIMESTAMPING,
            SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE,
        )

        # transmit: no receive queue (protocol 0), software transmit timestamps
        self.tx = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, 0)
        self.tx.bind((ifname, 0))

        self.tx.setsockopt(
            socket.SOL_SOCKET,
            SO_TIMESTAMPING,
            SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE,
        )

        self.mac = self.rx.getsockname()[4]
        self.identity = gptp.clock_id_from_mac(self.mac)

        self.capture = []
        self.cap_lock = threading.Lock()
        self.cap_cond = threading.Condition(self.cap_lock)

        self.tx_lock = threading.Lock()
        self.handlers = []

        self.pcap = PcapWriter(pcap_path) if pcap_path else None

        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()

    # ---- life cycle ----

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)

        self.rx.close()
        self.tx.close()

        if self.pcap:
            self.pcap.close()

    def port(self, port: int = 1, identity: Optional[bytes] = None) -> PortIdentity:
        return PortIdentity(identity or self.identity, port)

    def add_handler(self, h: Callable) -> None:
        self.handlers.append(h)

    def remove_handler(self, h: Callable) -> None:
        if h in self.handlers:
            self.handlers.remove(h)

    # ---- capture ----

    def _record(self, ts: int, direction: str, frame: bytes) -> Captured:
        try:
            raw = gptp.strip_eth(frame)
            cap = Captured(ts, direction, raw, gptp.parse(raw))
        except (ValueError, struct.error) as e:
            cap = Captured(ts, direction, frame[14:], None, str(e))

        with self.cap_cond:
            self.capture.append(cap)
            self.cap_cond.notify_all()

        if self.pcap:
            self.pcap.write(ts, frame)

        return cap

    def _rx_loop(self) -> None:
        while not self._stop.is_set():
            r, _, _ = select.select([self.rx], [], [], 0.1)

            if not r:
                continue

            try:
                frame, anc, _, _ = self.rx.recvmsg(2048, 1024)
            except OSError:
                continue

            ts = _scm_ts(anc) or now_ns()
            cap = self._record(ts, "rx", frame)

            for h in list(self.handlers):
                try:
                    h(cap)
                except Exception as e:      # a role's bug must not kill the capture
                    print("tester: handler error: %r" % e)

    def frames(self, *, direction: str = "rx", types=None, domain: Optional[int] = None,
               since: Optional[int] = None, until: Optional[int] = None,
               source: Optional[bytes] = None) -> list:
        """Captured frames that match, in arrival order."""
        if isinstance(types, int):
            types = (types,)

        out = []

        with self.cap_lock:
            caps = list(self.capture)

        for c in caps:
            if c.direction != direction or c.msg is None:
                continue

            if types is not None and c.type not in types:
                continue

            if domain is not None and c.domain != domain:
                continue

            if since is not None and c.ts < since:
                continue

            if until is not None and c.ts > until:
                continue

            if source is not None and c.msg.hdr.source.clock != source:
                continue

            out.append(c)

        return out

    def wait_frame(self, pred: Callable, timeout: float, since: Optional[int] = None) -> Optional[Captured]:
        """The first received frame after `since` for which pred(cap) holds."""
        deadline = time.monotonic() + timeout

        if since is None:
            since = now_ns()

        with self.cap_cond:
            while True:
                for c in self.capture:
                    if c.direction == "rx" and c.ts >= since and c.msg is not None and pred(c):
                        return c

                left = deadline - time.monotonic()

                if left <= 0:
                    return None

                self.cap_cond.wait(min(left, 0.05))

    def clear(self) -> None:
        with self.cap_lock:
            self.capture.clear()

    # ---- transmit ----

    def send(self, msg: bytes, dst: bytes = gptp.GPTP_MC) -> Optional[int]:
        """Send one PTP message; its software transmit timestamp (ns), or None
        when it could not be sent (the link is down)."""
        frame = gptp.eth_frame(self.mac, msg, dst)

        with self.tx_lock:
            self._drain_errqueue()

            try:
                self.tx.send(frame)
            except OSError:
                return None

            ts = self._tx_timestamp(frame)

        if ts is None:
            ts = now_ns()

        self._record(ts, "tx", frame)
        return ts

    def send_raw_frame(self, frame: bytes) -> Optional[int]:
        with self.tx_lock:
            self._drain_errqueue()

            try:
                self.tx.send(frame)
            except OSError:
                return None

            ts = self._tx_timestamp(frame) or now_ns()

        self._record(ts, "tx", frame)
        return ts

    def _drain_errqueue(self) -> None:
        while True:
            try:
                self.tx.recvmsg(2048, 1024, MSG_ERRQUEUE | socket.MSG_DONTWAIT)
            except (BlockingIOError, OSError):
                return

    def _tx_timestamp(self, frame: bytes) -> Optional[int]:
        deadline = time.monotonic() + TX_TS_TIMEOUT_S

        while True:
            left = deadline - time.monotonic()

            if left <= 0:
                return None

            p = select.poll()
            p.register(self.tx, select.POLLERR)

            if not p.poll(left * 1000):
                continue

            try:
                echo, anc, _, _ = self.tx.recvmsg(2048, 1024, MSG_ERRQUEUE | socket.MSG_DONTWAIT)
            except (BlockingIOError, OSError):
                continue

            # the echo is the frame as sent: match it, a stale one is skipped
            if echo[:len(frame)] == frame[:len(echo)]:
                return _scm_ts(anc)


# ---- roles -----------------------------------------------------------------

class PdelayResponder:
    """Answers the peer's Pdelay_Req, two-step.

    extra_delay_ns   the link delay the peer computes grows by this much
    rate_ppm         the responder's clock runs this fast against CLOCK_REALTIME
                     (the peer's neighborRateRatio sees it)
    drop_next        ignore this many requests, then answer again
    twin             a second clock identity that answers every request too
    identity         answer as this clock identity (the peer's own, to fake a loop)
    """

    def __init__(self, ep: Endpoint, *, identity: Optional[bytes] = None, port: int = 1,
                 extra_delay_ns: int = 0, rate_ppm: float = 0.0, twin: Optional[bytes] = None,
                 domain: int = 0):
        self.ep = ep
        self.identity = identity or ep.identity
        self.port = port
        self.extra_delay_ns = extra_delay_ns
        self.rate_ppm = rate_ppm
        self.twin = twin
        self.domain = domain

        self.drop_next = 0
        self.active = True

        self.answered = 0
        self.dropped = 0
        self.requests = []               # (rx_ts, Captured) of every request seen

        self._ref = now_ns()
        ep.add_handler(self._on_frame)

    def stop(self) -> None:
        self.ep.remove_handler(self._on_frame)

    def clock(self, t: int) -> int:
        """The responder's clock at CLOCK_REALTIME t."""
        return t + int((t - self._ref) * self.rate_ppm * 1e-6)

    def _on_frame(self, cap: Captured) -> None:
        if cap.direction != "rx" or cap.type != gptp.PDELAY_REQ or cap.domain != self.domain:
            return

        self.requests.append((cap.ts, cap))

        if not self.active:
            return

        if self.drop_next > 0:
            self.drop_next -= 1
            self.dropped += 1
            return

        req = cap.msg

        ids = [self.identity]
        if self.twin is not None:
            ids.append(self.twin)

        for ident in ids:
            src = PortIdentity(ident, self.port)

            # the extra delay comes out of the turnaround the peer subtracts
            t2 = self.clock(cap.ts) + self.extra_delay_ns

            t3_real = self.ep.send(
                gptp.build_pdelay_resp(
                    src,
                    req.hdr.seq,
                    t2,
                    req.hdr.source,
                    domain=self.domain,
                )
            )

            if t3_real is None:
                return

            t3 = self.clock(t3_real) - self.extra_delay_ns

            self.ep.send(
                gptp.build_pdelay_resp_fu(
                    src,
                    req.hdr.seq,
                    t3,
                    req.hdr.source,
                    domain=self.domain,
                )
            )

        self.answered += 1


class GrandMaster:
    """Announce and two-step Sync/Follow_Up, from a thread.

    The grandmaster time is CLOCK_REALTIME + time_offset_ns. Every attribute
    can be changed while it runs: the priority vector, path_trace (None: just
    our identity; False: no TLV), steps_removed, sync_source (a PortIdentity
    to send Sync/Follow_Up from instead of ours), send_sync / send_announce.
    """

    def __init__(self, ep: Endpoint, *, identity: Optional[bytes] = None, port: int = 1,
                 domain: int = 0, log_sync: int = -3, log_announce: int = 0,
                 priority1: int = 246, clock_class: int = 248, clock_accuracy: int = 0xFE,
                 variance: int = 0x436A, priority2: int = 248, gm_identity: Optional[bytes] = None,
                 steps_removed: int = 0, path_trace=None, time_offset_ns: int = 0,
                 fu_info: Optional[gptp.FollowUpInfoTlv] = None):
        self.ep = ep
        self.identity = identity or ep.identity
        self.port = port
        self.domain = domain
        self.log_sync = log_sync
        self.log_announce = log_announce

        self.priority1 = priority1
        self.clock_class = clock_class
        self.clock_accuracy = clock_accuracy
        self.variance = variance
        self.priority2 = priority2
        self.gm_identity = gm_identity
        self.steps_removed = steps_removed
        self.path_trace = path_trace
        self.time_offset_ns = time_offset_ns
        self.fu_info = fu_info or gptp.FollowUpInfoTlv()

        self.sync_source: Optional[PortIdentity] = None
        self.send_sync = True
        self.send_announce = True

        self.sync_seq = 0
        self.announce_seq = 0
        self.syncs_sent = 0

        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)

    @property
    def source(self) -> PortIdentity:
        return PortIdentity(self.identity, self.port)

    def _announce(self) -> None:
        self.ep.send(
            gptp.build_announce(
                self.source,
                self.announce_seq,
                domain=self.domain,
                log_interval=self.log_announce,
                priority1=self.priority1,
                clock_class=self.clock_class,
                clock_accuracy=self.clock_accuracy,
                variance=self.variance,
                priority2=self.priority2,
                gm_identity=self.gm_identity or self.identity,
                steps_removed=self.steps_removed,
                path_trace=self.path_trace,
            )
        )

        self.announce_seq = (self.announce_seq + 1) & 0xFFFF

    def _sync(self) -> None:
        src = self.sync_source or self.source

        t1 = self.ep.send(
            gptp.build_sync(
                src,
                self.sync_seq,
                domain=self.domain,
                log_interval=self.log_sync,
            )
        )

        if t1 is None:
            return

        self.ep.send(
            gptp.build_follow_up(
                src,
                self.sync_seq,
                t1 + self.time_offset_ns,
                domain=self.domain,
                log_interval=self.log_sync,
                info=self.fu_info,
            )
        )

        self.sync_seq = (self.sync_seq + 1) & 0xFFFF
        self.syncs_sent += 1

    def _loop(self) -> None:
        next_sync = time.monotonic()
        next_ann = time.monotonic()

        while not self._stop.is_set():
            now = time.monotonic()

            try:
                if now >= next_ann:
                    if self.send_announce:
                        self._announce()

                    next_ann += gptp.log_interval_s(self.log_announce)

                if now >= next_sync:
                    if self.send_sync:
                        self._sync()

                    next_sync += gptp.log_interval_s(self.log_sync)

            except OSError:
                # the link went away under us: keep the schedule, try again
                next_ann = max(next_ann, now + 0.1)
                next_sync = max(next_sync, now + 0.1)

            wake = min(next_sync, next_ann)
            self._stop.wait(max(0.0, wake - time.monotonic()))


class PdelayInitiator:
    """Measures the peer's pdelay responder."""

    def __init__(self, ep: Endpoint, *, identity: Optional[bytes] = None, port: int = 1, domain: int = 0):
        self.ep = ep
        self.src = PortIdentity(identity or ep.identity, port)
        self.domain = domain
        self.seq = 0x4000

    def measure(self, timeout: float = 0.5) -> Optional[dict]:
        """One exchange: its timestamps, the turnaround and the link delay."""
        self.seq = (self.seq + 1) & 0xFFFF
        seq = self.seq

        t1 = self.ep.send(gptp.build_pdelay_req(self.src, seq, domain=self.domain))

        if t1 is None:
            return None

        def ours(c, t):
            return (c.type == t and c.msg.hdr.seq == seq and c.msg.requesting == self.src)

        resp = self.ep.wait_frame(lambda c: ours(c, gptp.PDELAY_RESP), timeout, since=t1)

        if resp is None:
            return None

        fu = self.ep.wait_frame(lambda c: ours(c, gptp.PDELAY_RESP_FU), timeout, since=t1)

        if fu is None:
            return {"t1": t1, "resp": resp, "fu": None}

        t2 = resp.msg.request_receipt
        t3 = fu.msg.response_origin
        t4 = resp.ts

        return {
            "t1": t1,
            "t2": t2,
            "t3": t3,
            "t4": t4,
            "resp": resp,
            "fu": fu,
            "turnaround_ns": t4 - t1,                  # request out to response in, our clock
            "responder_turnaround_ns": t3 - t2,        # the responder's own
            "link_delay_ns": ((t4 - t1) - (t3 - t2)) / 2.0,
        }


class ReverseSyncReceiver:
    """Pairs Sync/Follow_Up of one domain, with the Sync's receive time."""

    def __init__(self, ep: Endpoint, domain: int):
        self.ep = ep
        self.domain = domain

        self.samples = []                # (rx_ts, origin + corrections, Sync Captured, FU msg)
        self._pending = {}

        ep.add_handler(self._on_frame)

    def stop(self) -> None:
        self.ep.remove_handler(self._on_frame)

    def _on_frame(self, cap: Captured) -> None:
        if cap.direction != "rx" or cap.domain != self.domain:
            return

        key = (cap.msg.hdr.source.clock, cap.msg.hdr.source.port, cap.msg.hdr.seq)

        if cap.type == gptp.SYNC:
            self._pending[key] = cap
        elif cap.type == gptp.FOLLOW_UP and key in self._pending:
            sync = self._pending.pop(key)

            corr = (sync.msg.hdr.correction + cap.msg.hdr.correction) / 65536.0
            self.samples.append((sync.ts, cap.msg.precise_origin + corr, sync, cap.msg))

    def time_errors(self, link_delay_ns: float, since: Optional[int] = None) -> list:
        """The sender's clock minus ours, at each Sync (egress method).

        TE = (preciseOriginTimestamp + corrections + link delay) - receive time
        """
        out = []

        for rx_ts, origin, _, _ in self.samples:
            if since is not None and rx_ts < since:
                continue

            out.append(origin + link_delay_ns - rx_ts)

        return out


def send_signaling(ep: Endpoint, req: Optional[gptp.MsgIntervalReqTlv], *, seq: int = 0,
                   extra: bytes = b"", identity: Optional[bytes] = None, domain: int = 0) -> Optional[int]:
    return ep.send(
        gptp.build_signaling(
            PortIdentity(identity or ep.identity, 1),
            seq,
            domain=domain,
            interval_req=req,
            extra=extra,
        )
    )


# ---- small statistics ------------------------------------------------------

def intervals(caps: list) -> list:
    """Seconds between consecutive captures."""
    return [(b.ts - a.ts) / 1e9 for a, b in zip(caps, caps[1:])]


def median(xs: list) -> float:
    s = sorted(xs)

    if not s:
        return float("nan")

    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


def slope(points: list) -> float:
    """Least-squares slope of (x, y) points."""
    n = len(points)

    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n

    sxx = sum((p[0] - mx) ** 2 for p in points)
    sxy = sum((p[0] - mx) * (p[1] - my) for p in points)

    return sxy / sxx
