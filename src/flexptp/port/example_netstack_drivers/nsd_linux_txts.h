/**
 ******************************************************************************
 * @file    nsd_linux_txts.h
 * @copyright Kebag-Logic, 2026
 * @brief   Matching of transmit timestamps to the messages they belong to,
 * for the Linux Network Stack Driver.
 *
 * The kernel returns a transmit timestamp on the socket's error queue together
 * with a copy of the frame it belongs to. A message is registered here before
 * it is sent, under its key (messageType, domainNumber, sequenceId, and for a
 * Pdelay_Resp the requestingPortIdentity too); the echoed frame's key finds it
 * again, whatever the order in which timestamps come back. A message whose
 * timestamp has not come back by its deadline expires.
 *
 * Pure logic, no sockets and no locking: the caller serializes the calls.
 ******************************************************************************
 */

#ifndef FLEXPTP_LINUX_NSD_LINUX_TXTS_H
#define FLEXPTP_LINUX_NSD_LINUX_TXTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TXTS_TABLE_SIZE (32) ///< messages waiting for their timestamp at most

#define TXTS_REQ_PORT_LENGTH (10) ///< length of a portIdentity

/**
 * @brief A message waiting for its transmit timestamp.
 */
typedef struct {
    bool used;                                ///< the entry holds a message
    uint32_t uid;                             ///< message UID
    uint8_t messageType;                      ///< messageType (low nibble of the first octet)
    uint8_t domainNumber;                     ///< domainNumber
    uint16_t sequenceId;                      ///< sequenceId
    bool hasReqPort;                          ///< the requestingPortIdentity is part of the key (Pdelay_Resp)
    uint8_t reqPort[TXTS_REQ_PORT_LENGTH];    ///< requestingPortIdentity
    int64_t deadline_ns;                      ///< expiry time (any monotonic time base)
    uint32_t order;                           ///< registration order: the oldest first among equal keys
} TxtsEntry;

/**
 * @brief The table of messages waiting for their transmit timestamp.
 */
typedef struct {
    TxtsEntry e[TXTS_TABLE_SIZE]; ///< entries
    uint32_t nextOrder;           ///< order number of the next registration
} TxtsTable;

/**
 * Empty the table.
 *
 * @param pTable table
 */
void txts_init(TxtsTable *pTable);

/**
 * Register a message before sending it.
 *
 * A full table gives up its oldest entry to make room; that message's
 * timestamp is then lost, and its UID is returned through pEvictedUid.
 *
 * @param pTable table
 * @param uid message UID
 * @param pPtp the PTP message (from its header on)
 * @param len its length
 * @param deadline_ns time by which its timestamp must have arrived
 * @param pEvictedUid set to the UID of an evicted message
 * @param pEvicted set to true if a message was evicted
 * @return false if the message is too short to carry a key (nothing registered)
 */
bool txts_add(TxtsTable *pTable, uint32_t uid, const uint8_t *pPtp, size_t len, int64_t deadline_ns,
              uint32_t *pEvictedUid, bool *pEvicted);

/**
 * Locate the PTP message in a frame echoed on the error queue: an Ethernet
 * frame (optionally VLAN-tagged) carrying PTP over 802.3 or over UDP/IPv4,
 * or a bare PTP message.
 *
 * @param pFrame the echoed bytes
 * @param len their length
 * @param ppPtp set to the start of the PTP message
 * @param pPtpLen set to its length
 * @return true if a PTP message was found
 */
bool txts_find_ptp_in_frame(const uint8_t *pFrame, size_t len, const uint8_t **ppPtp, size_t *pPtpLen);

/**
 * Find and remove the message an echoed frame belongs to.
 *
 * @param pTable table
 * @param pFrame the echoed bytes
 * @param len their length
 * @param pUid set to the message's UID
 * @return true if a waiting message matched
 */
bool txts_match(TxtsTable *pTable, const uint8_t *pFrame, size_t len, uint32_t *pUid);

/**
 * Remove a message by its UID (it was not sent after all).
 *
 * @param pTable table
 * @param uid message UID
 * @return true if it was waiting
 */
bool txts_remove(TxtsTable *pTable, uint32_t uid);

/**
 * Remove every message whose deadline has passed.
 *
 * @param pTable table
 * @param now_ns current time, in the time base of the deadlines
 * @param pUids receives the UIDs of the expired messages
 * @param maxUids room in pUids
 * @return the number of expired messages written to pUids
 */
int txts_expire(TxtsTable *pTable, int64_t now_ns, uint32_t *pUids, int maxUids);

/**
 * Remove every message, whatever its deadline.
 *
 * @param pTable table
 * @param pUids receives their UIDs
 * @param maxUids room in pUids
 * @return the number of messages written to pUids
 */
int txts_flush(TxtsTable *pTable, uint32_t *pUids, int maxUids);

/**
 * The earliest deadline in the table.
 *
 * @param pTable table
 * @param pDeadline_ns set to the earliest deadline
 * @return false if the table is empty
 */
bool txts_next_deadline(const TxtsTable *pTable, int64_t *pDeadline_ns);

#ifdef __cplusplus
}
#endif

#endif // FLEXPTP_LINUX_NSD_LINUX_TXTS_H
