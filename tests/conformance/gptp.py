# SPDX-License-Identifier: MIT
# Copyright Kebag-Logic, 2026
"""gptp.py - an IEEE 802.1AS-2020 (gPTP) frame library: build and parse every
message a time-aware end station exchanges on full-duplex 802.3.

Messages are dataclasses. build_*() return the PTP message (without the
Ethernet header); eth_frame() wraps one for a raw socket; parse() returns the
message dataclass matching the messageType, or raises ValueError.

Timestamps are integer nanoseconds (seconds * 1e9 + ns). The correctionField
and the ScaledNs fields are kept in their wire units (ns * 2^16) as integers.
Clock identities are 8-byte `bytes`, in wire order.

Run it directly for the round-trip self-tests:  python3 -I gptp.py
"""

import struct
from dataclasses import dataclass, field
from typing import Optional


# ---- link layer ----------------------------------------------------------

GPTP_MC = bytes.fromhex("0180c200000e")
ETH_P_1588 = 0x88F7

# ---- message types -------------------------------------------------------

SYNC = 0x0
PDELAY_REQ = 0x2
PDELAY_RESP = 0x3
FOLLOW_UP = 0x8
PDELAY_RESP_FU = 0xA
ANNOUNCE = 0xB
SIGNALING = 0xC

MSG_NAMES = {
    SYNC: "Sync",
    PDELAY_REQ: "Pdelay_Req",
    PDELAY_RESP: "Pdelay_Resp",
    FOLLOW_UP: "Follow_Up",
    PDELAY_RESP_FU: "Pdelay_Resp_Follow_Up",
    ANNOUNCE: "Announce",
    SIGNALING: "Signaling",
}

EVENT_TYPES = (SYNC, PDELAY_REQ, PDELAY_RESP)

# controlField (802.1AS-2020 table 10-9: 0 Sync, 2 Follow_Up, 5 all others)
CONTROL = {
    SYNC: 0,
    FOLLOW_UP: 2,
}
CONTROL_OTHER = 5

# ---- header flags (as the big-endian 16-bit flagField) -------------------

FLAG_ALTERNATE_MASTER = 0x0100
FLAG_TWO_STEP = 0x0200
FLAG_UNICAST = 0x0400
FLAG_PROFILE_SPECIFIC_1 = 0x2000
FLAG_PROFILE_SPECIFIC_2 = 0x4000
FLAG_LEAP61 = 0x0001
FLAG_LEAP59 = 0x0002
FLAG_UTC_OFFSET_VALID = 0x0004
FLAG_PTP_TIMESCALE = 0x0008
FLAG_TIME_TRACEABLE = 0x0010
FLAG_FREQUENCY_TRACEABLE = 0x0020

# ---- sizes ---------------------------------------------------------------

HEADER_LEN = 34
TIMESTAMP_LEN = 10
PORT_ID_LEN = 10

# logMessageInterval of the Pdelay_Resp(_Follow_Up), 802.1AS-2020 11.4.2.7
LOG_INTERVAL_PDELAY_RESP = 0x7F

# ---- TLVs ----------------------------------------------------------------

TLV_ORGANIZATION_EXTENSION = 0x0003
TLV_PATH_TRACE = 0x0008

ORG_IEEE_8021 = bytes.fromhex("0080c2")

ORG_SUB_FOLLOW_UP_INFO = 1
ORG_SUB_MSG_INTERVAL_REQ = 2
ORG_SUB_GPTP_CAPABLE = 4

# message interval request special values, 802.1AS-2020 10.6.4.3
INTERVAL_NO_CHANGE = -128
INTERVAL_INITIAL = 126
INTERVAL_STOP = 127

# message interval request flags
MIR_FLAG_COMPUTE_NRR = 0x02
MIR_FLAG_COMPUTE_MLD = 0x04


# ---- primitives ----------------------------------------------------------

def ts_pack(ns: int) -> bytes:
    """A PTP Timestamp: 48-bit seconds, 32-bit nanoseconds."""
    if ns < 0:
        raise ValueError("negative timestamp")

    sec = ns // 1_000_000_000
    nsec = ns % 1_000_000_000

    return struct.pack(">HIL", sec >> 32, sec & 0xFFFFFFFF, nsec)


def ts_unpack(b: bytes) -> int:
    sec_hi, sec_lo, nsec = struct.unpack(">HIL", b[:TIMESTAMP_LEN])

    if nsec >= 1_000_000_000:
        raise ValueError("nanoseconds field >= 1e9")

    return ((sec_hi << 32) | sec_lo) * 1_000_000_000 + nsec


def scaled_ns96_pack(v: int) -> bytes:
    """A ScaledNs (96-bit two's complement, ns * 2^16)."""
    return (v & ((1 << 96) - 1)).to_bytes(12, "big")


def scaled_ns96_unpack(b: bytes) -> int:
    v = int.from_bytes(b[:12], "big")

    if v >= 1 << 95:
        v -= 1 << 96

    return v


def clock_id_str(cid: bytes) -> str:
    """802.1AS notation: 3cc0c6.fffe.fe0210"""
    h = cid.hex()
    return f"{h[0:6]}.{h[6:10]}.{h[10:16]}"


def clock_id_from_mac(mac: bytes) -> bytes:
    return mac[:3] + b"\xff\xfe" + mac[3:6]


def clock_id_int(cid: bytes) -> int:
    """The clock identity as the unsigned number the BMCA compares."""
    return int.from_bytes(cid, "big")


@dataclass
class PortIdentity:
    clock: bytes = bytes(8)
    port: int = 1

    def pack(self) -> bytes:
        return self.clock + struct.pack(">H", self.port)

    @staticmethod
    def unpack(b: bytes) -> "PortIdentity":
        return PortIdentity(bytes(b[:8]), struct.unpack(">H", b[8:10])[0])

    def __str__(self) -> str:
        return f"{clock_id_str(self.clock)}-{self.port}"


# ---- header --------------------------------------------------------------

@dataclass
class Header:
    msg_type: int = SYNC
    major_sdo_id: int = 1
    version: int = 2
    minor_version: int = 1
    length: int = 0
    domain: int = 0
    minor_sdo_id: int = 0
    flags: int = 0
    correction: int = 0           # scaled ns (ns * 2^16)
    type_specific: int = 0
    source: PortIdentity = field(default_factory=PortIdentity)
    seq: int = 0
    control: int = CONTROL_OTHER
    log_interval: int = 0

    def pack(self) -> bytes:
        return struct.pack(
            ">BBHBBHqI10sHBb",
            (self.major_sdo_id << 4) | (self.msg_type & 0x0F),
            (self.minor_version << 4) | (self.version & 0x0F),
            self.length,
            self.domain,
            self.minor_sdo_id,
            self.flags,
            self.correction,
            self.type_specific,
            self.source.pack(),
            self.seq,
            self.control,
            self.log_interval,
        )

    @staticmethod
    def unpack(b: bytes) -> "Header":
        if len(b) < HEADER_LEN:
            raise ValueError("short PTP header")

        (b0, b1, length, domain, minor_sdo, flags, corr, tspec,
         src, seq, control, log_int) = struct.unpack(">BBHBBHqI10sHBb", b[:HEADER_LEN])

        return Header(
            msg_type=b0 & 0x0F,
            major_sdo_id=b0 >> 4,
            version=b1 & 0x0F,
            minor_version=b1 >> 4,
            length=length,
            domain=domain,
            minor_sdo_id=minor_sdo,
            flags=flags,
            correction=corr,
            type_specific=tspec,
            source=PortIdentity.unpack(src),
            seq=seq,
            control=control,
            log_interval=log_int,
        )

    @property
    def two_step(self) -> bool:
        return bool(self.flags & FLAG_TWO_STEP)

    @property
    def correction_ns(self) -> float:
        return self.correction / 65536.0


# ---- TLVs ----------------------------------------------------------------

@dataclass
class Tlv:
    """Any TLV, kept raw: type and value (the bytes after length)."""
    type: int
    value: bytes

    def pack(self) -> bytes:
        return struct.pack(">HH", self.type, len(self.value)) + self.value


@dataclass
class FollowUpInfoTlv:
    """802.1AS-2020 11.4.4.3"""
    cumulative_scaled_rate_offset: int = 0   # (rateRatio - 1) * 2^41
    gm_time_base_indicator: int = 0
    last_gm_phase_change: int = 0            # ScaledNs
    scaled_last_gm_freq_change: int = 0      # (fractional freq offset) * 2^41

    LENGTH = 28

    def pack(self) -> bytes:
        value = (
            ORG_IEEE_8021
            + ORG_SUB_FOLLOW_UP_INFO.to_bytes(3, "big")
            + struct.pack(">iH", self.cumulative_scaled_rate_offset, self.gm_time_base_indicator)
            + scaled_ns96_pack(self.last_gm_phase_change)
            + struct.pack(">i", self.scaled_last_gm_freq_change)
        )

        return struct.pack(">HH", TLV_ORGANIZATION_EXTENSION, len(value)) + value

    @staticmethod
    def from_value(v: bytes) -> "FollowUpInfoTlv":
        if len(v) != FollowUpInfoTlv.LENGTH:
            raise ValueError("Follow_Up information TLV length")

        csro, gmtbi = struct.unpack(">iH", v[6:12])

        return FollowUpInfoTlv(
            cumulative_scaled_rate_offset=csro,
            gm_time_base_indicator=gmtbi,
            last_gm_phase_change=scaled_ns96_unpack(v[12:24]),
            scaled_last_gm_freq_change=struct.unpack(">i", v[24:28])[0],
        )

    @property
    def rate_ratio(self) -> float:
        return 1.0 + self.cumulative_scaled_rate_offset / float(1 << 41)


@dataclass
class MsgIntervalReqTlv:
    """802.1AS-2020 10.6.4.3"""
    link_delay_interval: int = INTERVAL_NO_CHANGE
    time_sync_interval: int = INTERVAL_NO_CHANGE
    announce_interval: int = INTERVAL_NO_CHANGE
    flags: int = MIR_FLAG_COMPUTE_NRR | MIR_FLAG_COMPUTE_MLD

    LENGTH = 12

    def pack(self) -> bytes:
        value = (
            ORG_IEEE_8021
            + ORG_SUB_MSG_INTERVAL_REQ.to_bytes(3, "big")
            + struct.pack(
                ">bbbBH",
                self.link_delay_interval,
                self.time_sync_interval,
                self.announce_interval,
                self.flags,
                0,
            )
        )

        return struct.pack(">HH", TLV_ORGANIZATION_EXTENSION, len(value)) + value

    @staticmethod
    def from_value(v: bytes) -> "MsgIntervalReqTlv":
        if len(v) != MsgIntervalReqTlv.LENGTH:
            raise ValueError("message interval request TLV length")

        ldi, tsi, ai, flags, _ = struct.unpack(">bbbBH", v[6:12])

        return MsgIntervalReqTlv(ldi, tsi, ai, flags)


@dataclass
class GptpCapableTlv:
    """802.1AS-2020 10.6.4.4"""
    log_gptp_capable_interval: int = 3
    flags: int = 0

    LENGTH = 12

    def pack(self) -> bytes:
        value = (
            ORG_IEEE_8021
            + ORG_SUB_GPTP_CAPABLE.to_bytes(3, "big")
            + struct.pack(">bB", self.log_gptp_capable_interval, self.flags)
            + bytes(4)
        )

        return struct.pack(">HH", TLV_ORGANIZATION_EXTENSION, len(value)) + value

    @staticmethod
    def from_value(v: bytes) -> "GptpCapableTlv":
        if len(v) != GptpCapableTlv.LENGTH:
            raise ValueError("gPTP capable TLV length")

        interval, flags = struct.unpack(">bB", v[6:8])

        return GptpCapableTlv(interval, flags)


def path_trace_tlv(path: list) -> bytes:
    value = b"".join(path)
    return struct.pack(">HH", TLV_PATH_TRACE, len(value)) + value


def split_tlvs(b: bytes) -> list:
    """The TLVs of a message tail; a truncated last TLV raises ValueError."""
    tlvs = []
    i = 0

    while i + 4 <= len(b):
        t, n = struct.unpack(">HH", b[i:i + 4])

        if i + 4 + n > len(b):
            raise ValueError("truncated TLV")

        tlvs.append(Tlv(t, bytes(b[i + 4:i + 4 + n])))
        i += 4 + n

    return tlvs


def org_tlv(tlv: Tlv) -> Optional[tuple]:
    """(organizationId, organizationSubType, value) of an organization TLV."""
    if tlv.type != TLV_ORGANIZATION_EXTENSION or len(tlv.value) < 6:
        return None

    return tlv.value[:3], int.from_bytes(tlv.value[3:6], "big"), tlv.value


# ---- messages ------------------------------------------------------------

@dataclass
class Sync:
    hdr: Header
    origin: int = 0               # originTimestamp (reserved, 0, in two-step)


@dataclass
class FollowUp:
    hdr: Header
    precise_origin: int = 0
    info: Optional[FollowUpInfoTlv] = None
    tlvs: list = field(default_factory=list)


@dataclass
class PdelayReq:
    hdr: Header


@dataclass
class PdelayResp:
    hdr: Header
    request_receipt: int = 0      # t2
    requesting: PortIdentity = field(default_factory=PortIdentity)


@dataclass
class PdelayRespFollowUp:
    hdr: Header
    response_origin: int = 0      # t3
    requesting: PortIdentity = field(default_factory=PortIdentity)


@dataclass
class Announce:
    hdr: Header
    current_utc_offset: int = 37
    priority1: int = 248
    clock_class: int = 248
    clock_accuracy: int = 0xFE
    variance: int = 0x436A
    priority2: int = 248
    gm_identity: bytes = bytes(8)
    steps_removed: int = 0
    time_source: int = 0xA0
    path_trace: Optional[list] = None   # list of 8-byte identities, None: no TLV
    tlvs: list = field(default_factory=list)

    def vector(self) -> tuple:
        """The systemIdentity and stepsRemoved, as the BMCA compares them."""
        return (
            self.priority1,
            self.clock_class,
            self.clock_accuracy,
            self.variance,
            self.priority2,
            clock_id_int(self.gm_identity),
            self.steps_removed,
        )


@dataclass
class Signaling:
    hdr: Header
    target: PortIdentity = field(default_factory=lambda: PortIdentity(b"\xff" * 8, 0xFFFF))
    interval_req: Optional[MsgIntervalReqTlv] = None
    gptp_capable: Optional[GptpCapableTlv] = None
    tlvs: list = field(default_factory=list)


# ---- building ------------------------------------------------------------

def _finish(hdr: Header, body: bytes) -> bytes:
    hdr.length = HEADER_LEN + len(body)
    return hdr.pack() + body


def _hdr(msg_type: int, source: PortIdentity, seq: int, *, domain: int = 0, flags: int = 0,
         log_interval: int = 0, correction: int = 0, minor_version: int = 1,
         major_sdo_id: int = 1) -> Header:
    return Header(
        msg_type=msg_type,
        major_sdo_id=major_sdo_id,
        minor_version=minor_version,
        domain=domain,
        flags=flags,
        correction=correction,
        source=source,
        seq=seq,
        control=CONTROL.get(msg_type, CONTROL_OTHER),
        log_interval=log_interval,
    )


def build_sync(source: PortIdentity, seq: int, *, domain: int = 0, log_interval: int = -3,
               correction: int = 0, two_step: bool = True, origin: int = 0, **kw) -> bytes:
    flags = FLAG_TWO_STEP if two_step else 0

    hdr = _hdr(
        SYNC,
        source,
        seq,
        domain=domain,
        flags=flags,
        log_interval=log_interval,
        correction=correction,
        **kw,
    )

    return _finish(hdr, ts_pack(origin))


def build_follow_up(source: PortIdentity, seq: int, precise_origin: int, *, domain: int = 0,
                    log_interval: int = -3, correction: int = 0,
                    info: Optional[FollowUpInfoTlv] = FollowUpInfoTlv(), extra: bytes = b"",
                    **kw) -> bytes:
    hdr = _hdr(
        FOLLOW_UP,
        source,
        seq,
        domain=domain,
        log_interval=log_interval,
        correction=correction,
        **kw,
    )

    body = ts_pack(precise_origin)

    if info is not None:
        body += info.pack()

    return _finish(hdr, body + extra)


def build_pdelay_req(source: PortIdentity, seq: int, *, domain: int = 0, log_interval: int = 0,
                     **kw) -> bytes:
    hdr = _hdr(PDELAY_REQ, source, seq, domain=domain, log_interval=log_interval, **kw)

    return _finish(hdr, bytes(TIMESTAMP_LEN) + bytes(10))


def build_pdelay_resp(source: PortIdentity, seq: int, request_receipt: int, requesting: PortIdentity,
                      *, domain: int = 0, correction: int = 0, **kw) -> bytes:
    hdr = _hdr(
        PDELAY_RESP,
        source,
        seq,
        domain=domain,
        flags=FLAG_TWO_STEP,
        log_interval=LOG_INTERVAL_PDELAY_RESP,
        correction=correction,
        **kw,
    )

    return _finish(hdr, ts_pack(request_receipt) + requesting.pack())


def build_pdelay_resp_fu(source: PortIdentity, seq: int, response_origin: int,
                         requesting: PortIdentity, *, domain: int = 0, correction: int = 0,
                         **kw) -> bytes:
    hdr = _hdr(
        PDELAY_RESP_FU,
        source,
        seq,
        domain=domain,
        log_interval=LOG_INTERVAL_PDELAY_RESP,
        correction=correction,
        **kw,
    )

    return _finish(hdr, ts_pack(response_origin) + requesting.pack())


def build_announce(source: PortIdentity, seq: int, *, domain: int = 0, log_interval: int = 0,
                   priority1: int = 248, clock_class: int = 248, clock_accuracy: int = 0xFE,
                   variance: int = 0x436A, priority2: int = 248, gm_identity: Optional[bytes] = None,
                   steps_removed: int = 0, time_source: int = 0xA0, current_utc_offset: int = 37,
                   path_trace: Optional[list] = None, flags: int = FLAG_PTP_TIMESCALE,
                   extra: bytes = b"", **kw) -> bytes:
    if gm_identity is None:
        gm_identity = source.clock

    if path_trace is None:
        path_trace = [source.clock]

    hdr = _hdr(ANNOUNCE, source, seq, domain=domain, flags=flags, log_interval=log_interval, **kw)

    body = (
        bytes(TIMESTAMP_LEN)
        + struct.pack(
            ">hBBBBHB8sHB",
            current_utc_offset,
            0,
            priority1,
            clock_class,
            clock_accuracy,
            variance,
            priority2,
            gm_identity,
            steps_removed,
            time_source,
        )
    )

    # an empty list still sends the TLV (empty); False sends none
    if path_trace is not False:
        body += path_trace_tlv(path_trace)

    return _finish(hdr, body + extra)


def build_signaling(source: PortIdentity, seq: int, *, domain: int = 0,
                    target: Optional[PortIdentity] = None,
                    interval_req: Optional[MsgIntervalReqTlv] = None,
                    gptp_capable: Optional[GptpCapableTlv] = None, extra: bytes = b"",
                    log_interval: int = 0x7F, **kw) -> bytes:
    if target is None:
        target = PortIdentity(b"\xff" * 8, 0xFFFF)

    hdr = _hdr(SIGNALING, source, seq, domain=domain, log_interval=log_interval, **kw)

    body = target.pack()

    if interval_req is not None:
        body += interval_req.pack()

    if gptp_capable is not None:
        body += gptp_capable.pack()

    return _finish(hdr, body + extra)


def eth_frame(src_mac: bytes, msg: bytes, dst: bytes = GPTP_MC) -> bytes:
    frame = dst + src_mac + struct.pack(">H", ETH_P_1588) + msg

    # pad to the 60-byte Ethernet minimum
    return frame + bytes(max(0, 60 - len(frame)))


# ---- parsing -------------------------------------------------------------

def strip_eth(frame: bytes) -> bytes:
    """The PTP message of an Ethernet frame (an 802.1Q tag is skipped)."""
    etype = struct.unpack(">H", frame[12:14])[0]

    if etype == 0x8100:
        etype = struct.unpack(">H", frame[16:18])[0]
        payload = frame[18:]
    else:
        payload = frame[14:]

    if etype != ETH_P_1588:
        raise ValueError("not a PTP frame")

    return payload


def parse(msg: bytes):
    """A message dataclass. Trailing padding beyond messageLength is ignored."""
    hdr = Header.unpack(msg)

    if hdr.length < HEADER_LEN or hdr.length > len(msg):
        raise ValueError("messageLength %d, %d bytes" % (hdr.length, len(msg)))

    body = msg[HEADER_LEN:hdr.length]
    t = hdr.msg_type

    if t == SYNC:
        return Sync(hdr, ts_unpack(body))

    if t == FOLLOW_UP:
        tlvs = split_tlvs(body[TIMESTAMP_LEN:])
        info = None

        for tlv in tlvs:
            org = org_tlv(tlv)
            if org and org[0] == ORG_IEEE_8021 and org[1] == ORG_SUB_FOLLOW_UP_INFO:
                info = FollowUpInfoTlv.from_value(tlv.value)

        return FollowUp(hdr, ts_unpack(body), info, tlvs)

    if t == PDELAY_REQ:
        return PdelayReq(hdr)

    if t == PDELAY_RESP:
        return PdelayResp(hdr, ts_unpack(body), PortIdentity.unpack(body[10:20]))

    if t == PDELAY_RESP_FU:
        return PdelayRespFollowUp(hdr, ts_unpack(body), PortIdentity.unpack(body[10:20]))

    if t == ANNOUNCE:
        (utc, _, p1, cc, ca, var, p2, gm, steps, tsrc) = struct.unpack(">hBBBBHB8sHB", body[10:30])

        tlvs = split_tlvs(body[30:])
        path = None

        for tlv in tlvs:
            if tlv.type == TLV_PATH_TRACE:
                path = [tlv.value[i:i + 8] for i in range(0, len(tlv.value) - 7, 8)]

        return Announce(hdr, utc, p1, cc, ca, var, p2, bytes(gm), steps, tsrc, path, tlvs)

    if t == SIGNALING:
        target = PortIdentity.unpack(body[:10])
        tlvs = split_tlvs(body[10:])
        ireq = None
        cap = None

        for tlv in tlvs:
            org = org_tlv(tlv)
            if not org or org[0] != ORG_IEEE_8021:
                continue

            if org[1] == ORG_SUB_MSG_INTERVAL_REQ:
                ireq = MsgIntervalReqTlv.from_value(tlv.value)
            elif org[1] == ORG_SUB_GPTP_CAPABLE:
                cap = GptpCapableTlv.from_value(tlv.value)

        return Signaling(hdr, target, ireq, cap, tlvs)

    raise ValueError("message type 0x%x" % t)


def log_interval_s(log: int) -> float:
    return 2.0 ** log


# ---- self-tests ----------------------------------------------------------

def _selftest() -> int:
    failures = 0

    def check(name: str, ok: bool) -> None:
        nonlocal failures

        print(("ok    " if ok else "FAIL  ") + name)

        if not ok:
            failures += 1

    me = PortIdentity(bytes.fromhex("020000fffe000002"), 1)
    peer = PortIdentity(bytes.fromhex("3cc0c6fffefe0210"), 7)
    t = 1_791_471_005_566_095_405

    # timestamps and ScaledNs
    check("timestamp round trip", ts_unpack(ts_pack(t)) == t)
    check("timestamp 48-bit seconds", ts_unpack(ts_pack((1 << 40) * 1_000_000_000 + 5)) == (1 << 40) * 1_000_000_000 + 5)
    check("ScaledNs negative round trip", scaled_ns96_unpack(scaled_ns96_pack(-123456789 << 16)) == -123456789 << 16)
    check("clock id notation", clock_id_str(peer.clock) == "3cc0c6.fffe.fe0210")

    # Sync
    m = parse(build_sync(me, 17, log_interval=-3, correction=5 << 16))
    check("Sync fields", isinstance(m, Sync) and m.hdr.seq == 17 and m.hdr.two_step
          and m.hdr.log_interval == -3 and m.hdr.correction_ns == 5.0
          and m.hdr.major_sdo_id == 1 and m.hdr.minor_version == 1
          and m.hdr.control == 0 and m.hdr.length == 44 and m.hdr.source == me)

    # Follow_Up with its TLV
    info = FollowUpInfoTlv(cumulative_scaled_rate_offset=-2_000_000, gm_time_base_indicator=3,
                           last_gm_phase_change=-(5 << 16), scaled_last_gm_freq_change=42)
    m = parse(build_follow_up(me, 17, t, info=info))
    check("Follow_Up fields and TLV", isinstance(m, FollowUp) and m.precise_origin == t
          and m.info == info and m.hdr.length == 76 and m.hdr.control == 2)
    check("Follow_Up rate ratio", abs(m.info.rate_ratio - (1 - 2_000_000 / 2 ** 41)) < 1e-15)

    # Pdelay
    m = parse(build_pdelay_req(me, 9))
    check("Pdelay_Req length 54", isinstance(m, PdelayReq) and m.hdr.length == 54 and m.hdr.control == 5)

    m = parse(build_pdelay_resp(peer, 9, t, me))
    check("Pdelay_Resp fields", isinstance(m, PdelayResp) and m.request_receipt == t
          and m.requesting == me and m.hdr.two_step and (m.hdr.log_interval & 0xFF) == 0x7F)

    m = parse(build_pdelay_resp_fu(peer, 9, t + 1000, me))
    check("Pdelay_Resp_Follow_Up fields", isinstance(m, PdelayRespFollowUp)
          and m.response_origin == t + 1000 and m.requesting == me)

    # Announce
    m = parse(build_announce(peer, 4, priority1=246, gm_identity=peer.clock, steps_removed=1,
                             path_trace=[peer.clock, me.clock]))
    check("Announce vector", isinstance(m, Announce) and m.priority1 == 246 and m.steps_removed == 1
          and m.gm_identity == peer.clock and m.path_trace == [peer.clock, me.clock]
          and m.hdr.length == 64 + 4 + 16)
    check("Announce without path trace", parse(build_announce(peer, 4, path_trace=False)).path_trace is None)

    # Signaling
    req = MsgIntervalReqTlv(1, -2, INTERVAL_STOP, MIR_FLAG_COMPUTE_NRR)
    m = parse(build_signaling(me, 2, interval_req=req, gptp_capable=GptpCapableTlv(3, 0)))
    check("Signaling interval request", isinstance(m, Signaling) and m.interval_req == req
          and m.gptp_capable == GptpCapableTlv(3, 0) and m.target.port == 0xFFFF)

    # framing and rejections
    f = eth_frame(bytes.fromhex("020000000002"), build_pdelay_req(me, 1))
    check("Ethernet frame", f[:6] == GPTP_MC and parse(strip_eth(f)).hdr.seq == 1)

    bad = bytearray(build_follow_up(me, 1, t))
    bad[HEADER_LEN + TIMESTAMP_LEN + 3] = 99   # TLV length beyond the message
    try:
        parse(bytes(bad))
        check("truncated TLV rejected", False)
    except ValueError:
        check("truncated TLV rejected", True)

    print("SELFTEST: " + ("PASS" if failures == 0 else "FAIL (%d)" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_selftest())
