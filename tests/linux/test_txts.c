// SPDX-License-Identifier: MIT
// Copyright Kebag-Logic, 2026
//
// test_txts.c - the transmit timestamp matcher of the Linux network stack
// driver (nsd_linux_txts.c): timestamps out of order, duplicated, late after
// their timeout, lost, a full table, Pdelay_Resp to two requesters, and the
// frame shapes an error-queue echo comes in.

#include <stdio.h>
#include <string.h>

#include "flexptp/port/example_netstack_drivers/nsd_linux_txts.h"

static int failures = 0;

#define CHECK(cond, what)                                   \
    do {                                                    \
        if (cond) {                                         \
            printf("[PASS] %s\n", what);                    \
        } else {                                            \
            printf("[FAIL] %s (%s:%d)\n", what, __FILE__, __LINE__); \
            failures++;                                     \
        }                                                   \
    } while (0)

// ---- message and frame builders ----

/**
 * A PTP message of the given type, domain and sequenceId (54 bytes, enough
 * for a Pdelay_Resp's requestingPortIdentity).
 */
static size_t make_ptp(uint8_t *p, uint8_t type, uint8_t domain, uint16_t seq, uint8_t reqPortByte) {
    memset(p, 0, 54);

    p[0] = 0x10 | type; // majorSdoId 1
    p[1] = 0x02;        // versionPTP 2
    p[2] = 0;
    p[3] = 54;          // messageLength
    p[4] = domain;
    p[30] = (uint8_t)(seq >> 8);
    p[31] = (uint8_t)seq;

    // requestingPortIdentity of a Pdelay_Resp
    for (int i = 44; i < 54; i++) {
        p[i] = reqPortByte;
    }

    return 54;
}

/**
 * The same message as an error-queue echo: an Ethernet frame, optionally
 * VLAN-tagged.
 */
static size_t make_eth(uint8_t *f, const uint8_t *ptp, size_t len, int vlan) {
    memset(f, 0, 14);
    f[0] = 0x01;
    f[1] = 0x80;
    f[2] = 0xC2;
    f[5] = 0x0E;

    size_t off = 12;
    if (vlan) {
        f[off] = 0x81;
        f[off + 1] = 0x00;
        f[off + 2] = 0x60;
        f[off + 3] = 0x02;
        off += 4;
    }

    f[off] = 0x88;
    f[off + 1] = 0xF7;
    off += 2;

    memcpy(f + off, ptp, len);

    return off + len;
}

/**
 * The same message over UDP/IPv4 (to port 319).
 */
static size_t make_udp(uint8_t *f, const uint8_t *ptp, size_t len) {
    memset(f, 0, 42);
    f[12] = 0x08;
    f[13] = 0x00;

    uint8_t *ip = f + 14;
    ip[0] = 0x45;  // IPv4, IHL 5
    ip[9] = 17;    // UDP

    uint8_t *udp = ip + 20;
    udp[2] = 0x01; // destination port 319
    udp[3] = 0x3F;

    memcpy(f + 42, ptp, len);

    return 42 + len;
}

// ---- cases ----

static void test_out_of_order(void) {
    TxtsTable t;
    txts_init(&t);

    uint8_t a[64];
    uint8_t b[64];
    uint8_t c[64];
    size_t la = make_ptp(a, 0x0, 0, 100, 0); // Sync 100
    size_t lb = make_ptp(b, 0x2, 0, 7, 0);   // Pdelay_Req 7
    size_t lc = make_ptp(c, 0x0, 1, 100, 0); // Sync 100 on domain 1 (reverse sync)

    bool ev;
    uint32_t evUid;
    txts_add(&t, 11, a, la, 1000, &evUid, &ev);
    txts_add(&t, 22, b, lb, 1000, &evUid, &ev);
    txts_add(&t, 33, c, lc, 1000, &evUid, &ev);

    // the timestamps come back in the reverse order
    uint8_t f[128];
    uint32_t uid = 0;

    bool m3 = txts_match(&t, f, make_eth(f, c, lc, 0), &uid) && (uid == 33);
    bool m2 = txts_match(&t, f, make_eth(f, b, lb, 0), &uid) && (uid == 22);
    bool m1 = txts_match(&t, f, make_eth(f, a, la, 0), &uid) && (uid == 11);

    CHECK(m1 && m2 && m3, "out of order: each timestamp finds its own message (domain part of the key)");

    int64_t d;
    CHECK(!txts_next_deadline(&t, &d), "out of order: the table is empty afterwards");
}

static void test_duplicate(void) {
    TxtsTable t;
    txts_init(&t);

    uint8_t a[64];
    size_t la = make_ptp(a, 0x0, 0, 5, 0);

    bool ev;
    uint32_t evUid;
    txts_add(&t, 1, a, la, 1000, &evUid, &ev);

    uint8_t f[128];
    uint32_t uid = 0;

    bool first = txts_match(&t, f, make_eth(f, a, la, 0), &uid) && (uid == 1);
    bool second = txts_match(&t, f, make_eth(f, a, la, 0), &uid);

    CHECK(first && !second, "duplicate: a second echo of the same message matches nothing");
}

static void test_late_after_timeout(void) {
    TxtsTable t;
    txts_init(&t);

    uint8_t a[64];
    uint8_t b[64];
    size_t la = make_ptp(a, 0x0, 0, 9, 0);
    size_t lb = make_ptp(b, 0x0, 0, 10, 0);

    bool ev;
    uint32_t evUid;
    txts_add(&t, 1, a, la, 100, &evUid, &ev);
    txts_add(&t, 2, b, lb, 300, &evUid, &ev);

    int64_t d = 0;
    bool any = txts_next_deadline(&t, &d);
    CHECK(any && (d == 100), "timeout: the earliest deadline is the first message's");

    // at 200 the first has expired, the second not yet
    uint32_t uids[TXTS_TABLE_SIZE];
    int n = txts_expire(&t, 200, uids, TXTS_TABLE_SIZE);
    CHECK((n == 1) && (uids[0] == 1), "timeout: only the overdue message expires");

    // its timestamp arrives late: dropped, and it does not steal the other's
    uint8_t f[128];
    uint32_t uid = 0;
    bool late = txts_match(&t, f, make_eth(f, a, la, 0), &uid);
    CHECK(!late, "timeout: a late timestamp matches nothing");

    bool other = txts_match(&t, f, make_eth(f, b, lb, 0), &uid) && (uid == 2);
    CHECK(other, "timeout: the other message still gets its own timestamp");
}

static void test_lost(void) {
    TxtsTable t;
    txts_init(&t);

    uint8_t a[64];
    uint8_t b[64];
    size_t la = make_ptp(a, 0x2, 0, 1, 0);
    size_t lb = make_ptp(b, 0x2, 0, 2, 0);

    bool ev;
    uint32_t evUid;
    txts_add(&t, 1, a, la, 100, &evUid, &ev);
    txts_add(&t, 2, b, lb, 1100, &evUid, &ev);

    // the first timestamp never comes; the second does
    uint8_t f[128];
    uint32_t uid = 0;
    bool second = txts_match(&t, f, make_eth(f, b, lb, 0), &uid) && (uid == 2);

    uint32_t uids[TXTS_TABLE_SIZE];
    int n = txts_expire(&t, 150, uids, TXTS_TABLE_SIZE);

    CHECK(second && (n == 1) && (uids[0] == 1), "lost: the second gets its timestamp, the first expires");
}

static void test_removed(void) {
    TxtsTable t;
    txts_init(&t);

    uint8_t a[64];
    size_t la = make_ptp(a, 0x0, 0, 3, 0);

    bool ev;
    uint32_t evUid;
    txts_add(&t, 7, a, la, 100, &evUid, &ev);

    bool removed = txts_remove(&t, 7);

    uint8_t f[128];
    uint32_t uid = 0;
    bool matched = txts_match(&t, f, make_eth(f, a, la, 0), &uid);

    CHECK(removed && !matched, "not sent: a removed message matches nothing");
}

static void test_same_key_fifo(void) {
    TxtsTable t;
    txts_init(&t);

    // the same key twice (a sequenceId reused): the oldest first
    uint8_t a[64];
    size_t la = make_ptp(a, 0x0, 0, 42, 0);

    bool ev;
    uint32_t evUid;
    txts_add(&t, 1, a, la, 1000, &evUid, &ev);
    txts_add(&t, 2, a, la, 1000, &evUid, &ev);

    uint8_t f[128];
    uint32_t u1 = 0;
    uint32_t u2 = 0;
    bool ok = txts_match(&t, f, make_eth(f, a, la, 0), &u1) &&
              txts_match(&t, f, make_eth(f, a, la, 0), &u2);

    CHECK(ok && (u1 == 1) && (u2 == 2), "same key: matched oldest first");
}

static void test_pdelay_resp_requesters(void) {
    TxtsTable t;
    txts_init(&t);

    // two Pdelay_Resp with the same sequenceId, to two requesters
    uint8_t a[64];
    uint8_t b[64];
    size_t la = make_ptp(a, 0x3, 0, 77, 0xAA);
    size_t lb = make_ptp(b, 0x3, 0, 77, 0xBB);

    bool ev;
    uint32_t evUid;
    txts_add(&t, 1, a, la, 1000, &evUid, &ev);
    txts_add(&t, 2, b, lb, 1000, &evUid, &ev);

    uint8_t f[128];
    uint32_t uid = 0;
    bool second = txts_match(&t, f, make_eth(f, b, lb, 0), &uid) && (uid == 2);
    bool first = txts_match(&t, f, make_eth(f, a, la, 0), &uid) && (uid == 1);

    CHECK(first && second, "Pdelay_Resp: told apart by the requestingPortIdentity");
}

static void test_full_table(void) {
    TxtsTable t;
    txts_init(&t);

    uint8_t a[64];
    bool ev = false;
    uint32_t evUid = 0;

    for (uint32_t i = 0; i < TXTS_TABLE_SIZE; i++) {
        size_t la = make_ptp(a, 0x0, 0, (uint16_t)i, 0);
        txts_add(&t, 100 + i, a, la, 1000, &evUid, &ev);
    }

    CHECK(!ev, "full table: no eviction up to its size");

    size_t la = make_ptp(a, 0x0, 0, 999, 0);
    txts_add(&t, 999, a, la, 1000, &evUid, &ev);

    CHECK(ev && (evUid == 100), "full table: one more evicts the oldest");
}

static void test_frame_shapes(void) {
    uint8_t a[64];
    size_t la = make_ptp(a, 0x0, 0, 1, 0);

    uint8_t f[128];
    const uint8_t *p = NULL;
    size_t len = 0;

    bool eth = txts_find_ptp_in_frame(f, make_eth(f, a, la, 0), &p, &len) && (p == f + 14);
    bool vlan = txts_find_ptp_in_frame(f, make_eth(f, a, la, 1), &p, &len) && (p == f + 18);
    bool udp = txts_find_ptp_in_frame(f, make_udp(f, a, la), &p, &len) && (p == f + 42);
    bool bare = txts_find_ptp_in_frame(a, la, &p, &len) && (p == a);

    CHECK(eth, "frame: PTP over Ethernet");
    CHECK(vlan, "frame: PTP over VLAN-tagged Ethernet");
    CHECK(udp, "frame: PTP over UDP/IPv4");
    CHECK(bare, "frame: a bare PTP message");

    uint8_t junk[20] = {0};
    CHECK(!txts_find_ptp_in_frame(junk, sizeof(junk), &p, &len), "frame: too short to be PTP");
}

static void test_short_message(void) {
    TxtsTable t;
    txts_init(&t);

    uint8_t a[16] = {0};
    bool ev;
    uint32_t evUid;

    CHECK(!txts_add(&t, 1, a, sizeof(a), 1000, &evUid, &ev), "a message too short for a key is refused");
}

int main(void) {
    test_out_of_order();
    test_duplicate();
    test_late_after_timeout();
    test_lost();
    test_removed();
    test_same_key_fifo();
    test_pdelay_resp_requesters();
    test_full_table();
    test_frame_shapes();
    test_short_message();

    printf("test_txts: %s (%d failed)\n", failures ? "FAIL" : "PASS", failures);

    return failures ? 1 : 0;
}
