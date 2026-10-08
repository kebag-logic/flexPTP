/* (C) Kebag-Logic, 2026 */

#include "nsd_linux_txts.h"

#include <string.h>

// ---- PTP and frame layout ----

#define PTP_MIN_KEY_LENGTH (32)        ///< a header up to and including the sequenceId
#define PTP_REQ_PORT_OFFSET (44)       ///< requestingPortIdentity of a Pdelay_Resp
#define PTP_MT_PDELAY_RESP_CODE (3)    ///< messageType of a Pdelay_Resp

#define ETH_HEADER_LENGTH (14)
#define ETHERTYPE_VLAN_CODE (0x8100)
#define ETHERTYPE_IPV4_CODE (0x0800)
#define ETHERTYPE_PTP_CODE (0x88F7)

#define IPPROTO_UDP_CODE (17)
#define UDP_HEADER_LENGTH (8)
#define PTP_UDP_PORT_EVENT (319)
#define PTP_UDP_PORT_GENERAL (320)

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/**
 * Fill the key fields of an entry from a PTP message.
 */
static bool key_from_ptp(TxtsEntry *pKey, const uint8_t *pPtp, size_t len) {
    if (len < PTP_MIN_KEY_LENGTH) {
        return false;
    }

    pKey->messageType = pPtp[0] & 0x0F;
    pKey->domainNumber = pPtp[4];
    pKey->sequenceId = be16(pPtp + 30);

    // two requesters may use the same sequenceId: a Pdelay_Resp is told
    // apart by whom it answers
    pKey->hasReqPort = (pKey->messageType == PTP_MT_PDELAY_RESP_CODE) &&
                       (len >= PTP_REQ_PORT_OFFSET + TXTS_REQ_PORT_LENGTH);

    if (pKey->hasReqPort) {
        memcpy(pKey->reqPort, pPtp + PTP_REQ_PORT_OFFSET, TXTS_REQ_PORT_LENGTH);
    } else {
        memset(pKey->reqPort, 0, TXTS_REQ_PORT_LENGTH);
    }

    return true;
}

static bool same_key(const TxtsEntry *pA, const TxtsEntry *pB) {
    if ((pA->messageType != pB->messageType) ||
        (pA->domainNumber != pB->domainNumber) ||
        (pA->sequenceId != pB->sequenceId) ||
        (pA->hasReqPort != pB->hasReqPort)) {
        return false;
    }

    if (pA->hasReqPort && (memcmp(pA->reqPort, pB->reqPort, TXTS_REQ_PORT_LENGTH) != 0)) {
        return false;
    }

    return true;
}

/**
 * Does the buffer look like a PTP message of its own (PTPv2)?
 */
static bool looks_like_ptp(const uint8_t *p, size_t len) {
    if (len < PTP_MIN_KEY_LENGTH) {
        return false;
    }

    // versionPTP 2 in the low nibble, and a messageLength that fits
    uint16_t messageLength = be16(p + 2);

    return ((p[1] & 0x0F) == 2) &&
           (messageLength >= PTP_MIN_KEY_LENGTH) &&
           (messageLength <= len);
}

// ---------------------------

void txts_init(TxtsTable *pTable) {
    memset(pTable, 0, sizeof(TxtsTable));
}

bool txts_add(TxtsTable *pTable, uint32_t uid, const uint8_t *pPtp, size_t len, int64_t deadline_ns,
              uint32_t *pEvictedUid, bool *pEvicted) {
    *pEvicted = false;

    TxtsEntry key;
    if (!key_from_ptp(&key, pPtp, len)) {
        return false;
    }

    // a free entry, or else the oldest one
    TxtsEntry *pSlot = NULL;
    TxtsEntry *pOldest = NULL;

    for (int i = 0; i < TXTS_TABLE_SIZE; i++) {
        TxtsEntry *pE = &pTable->e[i];

        if (!pE->used) {
            pSlot = pE;
            break;
        }

        if ((pOldest == NULL) || ((int32_t)(pE->order - pOldest->order) < 0)) {
            pOldest = pE;
        }
    }

    if (pSlot == NULL) {
        *pEvicted = true;
        *pEvictedUid = pOldest->uid;
        pSlot = pOldest;
    }

    *pSlot = key;
    pSlot->used = true;
    pSlot->uid = uid;
    pSlot->deadline_ns = deadline_ns;
    pSlot->order = pTable->nextOrder;

    pTable->nextOrder++;

    return true;
}

bool txts_find_ptp_in_frame(const uint8_t *pFrame, size_t len, const uint8_t **ppPtp, size_t *pPtpLen) {
    // an Ethernet frame
    if (len >= ETH_HEADER_LENGTH + PTP_MIN_KEY_LENGTH) {
        size_t off = ETH_HEADER_LENGTH;
        uint16_t etherType = be16(pFrame + 12);

        if ((etherType == ETHERTYPE_VLAN_CODE) && (len >= off + 4)) {
            etherType = be16(pFrame + 16);
            off += 4;
        }

        if (etherType == ETHERTYPE_PTP_CODE) {
            if (looks_like_ptp(pFrame + off, len - off)) {
                *ppPtp = pFrame + off;
                *pPtpLen = len - off;
                return true;
            }
        } else if ((etherType == ETHERTYPE_IPV4_CODE) && (len >= off + 20)) {
            // IPv4, then UDP to one of the PTP ports
            const uint8_t *pIp = pFrame + off;
            size_t ihl = (size_t)(pIp[0] & 0x0F) * 4;

            if ((ihl >= 20) && (pIp[9] == IPPROTO_UDP_CODE) && (len >= off + ihl + UDP_HEADER_LENGTH)) {
                const uint8_t *pUdp = pIp + ihl;
                uint16_t dstPort = be16(pUdp + 2);
                size_t ptpOff = off + ihl + UDP_HEADER_LENGTH;

                bool ptpPort = (dstPort == PTP_UDP_PORT_EVENT) || (dstPort == PTP_UDP_PORT_GENERAL);

                if (ptpPort && looks_like_ptp(pFrame + ptpOff, len - ptpOff)) {
                    *ppPtp = pFrame + ptpOff;
                    *pPtpLen = len - ptpOff;
                    return true;
                }
            }
        }
    }

    // a bare PTP message
    if (looks_like_ptp(pFrame, len)) {
        *ppPtp = pFrame;
        *pPtpLen = len;
        return true;
    }

    return false;
}

bool txts_match(TxtsTable *pTable, const uint8_t *pFrame, size_t len, uint32_t *pUid) {
    const uint8_t *pPtp = NULL;
    size_t ptpLen = 0;

    if (!txts_find_ptp_in_frame(pFrame, len, &pPtp, &ptpLen)) {
        return false;
    }

    TxtsEntry key;
    if (!key_from_ptp(&key, pPtp, ptpLen)) {
        return false;
    }

    // the oldest waiting message with this key
    TxtsEntry *pFound = NULL;

    for (int i = 0; i < TXTS_TABLE_SIZE; i++) {
        TxtsEntry *pE = &pTable->e[i];

        if (!pE->used || !same_key(pE, &key)) {
            continue;
        }

        if ((pFound == NULL) || ((int32_t)(pE->order - pFound->order) < 0)) {
            pFound = pE;
        }
    }

    if (pFound == NULL) {
        return false;
    }

    *pUid = pFound->uid;
    pFound->used = false;

    return true;
}

bool txts_remove(TxtsTable *pTable, uint32_t uid) {
    for (int i = 0; i < TXTS_TABLE_SIZE; i++) {
        TxtsEntry *pE = &pTable->e[i];

        if (pE->used && (pE->uid == uid)) {
            pE->used = false;
            return true;
        }
    }

    return false;
}

int txts_expire(TxtsTable *pTable, int64_t now_ns, uint32_t *pUids, int maxUids) {
    int n = 0;

    for (int i = 0; (i < TXTS_TABLE_SIZE) && (n < maxUids); i++) {
        TxtsEntry *pE = &pTable->e[i];

        if (pE->used && (pE->deadline_ns <= now_ns)) {
            pUids[n] = pE->uid;
            n++;

            pE->used = false;
        }
    }

    return n;
}

int txts_flush(TxtsTable *pTable, uint32_t *pUids, int maxUids) {
    int n = 0;

    for (int i = 0; (i < TXTS_TABLE_SIZE) && (n < maxUids); i++) {
        TxtsEntry *pE = &pTable->e[i];

        if (pE->used) {
            pUids[n] = pE->uid;
            n++;

            pE->used = false;
        }
    }

    return n;
}

bool txts_next_deadline(const TxtsTable *pTable, int64_t *pDeadline_ns) {
    bool any = false;

    for (int i = 0; i < TXTS_TABLE_SIZE; i++) {
        const TxtsEntry *pE = &pTable->e[i];

        if (!pE->used) {
            continue;
        }

        if (!any || (pE->deadline_ns < *pDeadline_ns)) {
            *pDeadline_ns = pE->deadline_ns;
            any = true;
        }
    }

    return any;
}
