/* (C) Kebag-Logic, 2026 */

#include "gptp.h"

#include <string.h>

#include "msg_utils.h"
#include "ptp_core.h"
#include "ptp_defs.h"

///\cond 0
#define S (gPtpCoreState)
///\endcond

#define TLV_HEADER_LEN (4)            ///< tlvType and lengthField
#define ORG_EXT_HEADER_LEN (6)        ///< organizationId and organizationSubType
#define FOLLOW_UP_INFO_SUBTYPE (1)    ///< Follow_Up information TLV (IEEE 802.1AS-2020 11.4.4.3)

static const uint8_t IEEE_8021_OUI[3] = {0x00, 0x80, 0xC2}; ///< organizationId of IEEE 802.1

uint16_t ptp_gptp_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t ptp_gptp_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void ptp_gptp_fill_header(PtpHeader *pHeader, PtpMessageType type, uint16_t length, uint8_t domain, uint16_t seq, int8_t logInterval) {
    memset(pHeader, 0, sizeof(PtpHeader));

    pHeader->messageType = type;
    pHeader->transportSpecific = PTP_TSPEC_GPTP_8021AS;
    pHeader->versionPTP = 2;
    pHeader->minorVersionPTP = PTP_GPTP_MINOR_VERSION;
    pHeader->messageLength = length;
    pHeader->domainNumber = domain;

    pHeader->clockIdentity = S.hwoptions.clockIdentity;
    pHeader->sourcePortID = PTP_PORT_ID;
    pHeader->sequenceID = seq;
    pHeader->logMessagePeriod = logInterval;

    // controlField: kept for compatibility with IEEE 1588-2008 receivers
    switch (type) {
    case PTP_MT_Sync:
        pHeader->control = PTP_CON_Sync;
        break;
    case PTP_MT_Follow_Up:
        pHeader->control = PTP_CON_Follow_Up;
        break;
    default:
        pHeader->control = PTP_CON_Other;
        break;
    }
}

const uint8_t *ptp_gptp_find_tlv(const uint8_t *pData, uint32_t size, uint32_t from, uint16_t type, int32_t orgSubType, uint16_t *pLen) {
    uint32_t off = from;

    while (off + TLV_HEADER_LEN <= size) {
        uint16_t t = ptp_gptp_be16(pData + off);
        uint16_t len = ptp_gptp_be16(pData + off + 2);

        // a TLV that runs past the end of the message ends the search
        if (off + TLV_HEADER_LEN + len > size) {
            return NULL;
        }

        bool match = (t == type);

        if (match && (orgSubType >= 0)) {
            const uint8_t *org = pData + off + TLV_HEADER_LEN;

            match = (len >= ORG_EXT_HEADER_LEN) &&
                    (memcmp(org, IEEE_8021_OUI, 3) == 0) &&
                    ((((uint32_t)org[3] << 16) | ((uint32_t)org[4] << 8) | org[5]) == (uint32_t)orgSubType);
        }

        if (match) {
            *pLen = len;
            return pData + off;
        }

        off += TLV_HEADER_LEN + len;
    }

    return NULL;
}

void ptp_gptp_write_follow_up_info(uint8_t *p, const PtpGptpFollowUpInfo *pInfo) {
    put_be16(p, PTP_TLV_ORGANIZATION_EXTENSION);
    put_be16(p + 2, PTP_GPTP_FOLLOW_UP_INFO_TLV_LEN - TLV_HEADER_LEN);

    memcpy(p + 4, IEEE_8021_OUI, 3);
    p[7] = 0;
    p[8] = 0;
    p[9] = FOLLOW_UP_INFO_SUBTYPE;

    put_be32(p + 10, (uint32_t)pInfo->cumulativeScaledRateOffset);
    put_be16(p + 14, pInfo->gmTimeBaseIndicator);
    memcpy(p + 16, pInfo->lastGmPhaseChange, 12);
    put_be32(p + 28, (uint32_t)pInfo->scaledLastGmFreqChange);
}

bool ptp_gptp_read_follow_up_info(const uint8_t *pData, uint32_t size, PtpGptpFollowUpInfo *pInfo) {
    uint16_t len = 0;
    const uint8_t *p = ptp_gptp_find_tlv(pData,
                                         size,
                                         PTP_PCKT_SIZE_FOLLOW_UP,
                                         PTP_TLV_ORGANIZATION_EXTENSION,
                                         FOLLOW_UP_INFO_SUBTYPE,
                                         &len);

    if ((p == NULL) || (len < PTP_GPTP_FOLLOW_UP_INFO_TLV_LEN - TLV_HEADER_LEN)) {
        return false;
    }

    pInfo->cumulativeScaledRateOffset = (int32_t)ptp_gptp_be32(p + 10);
    pInfo->gmTimeBaseIndicator = ptp_gptp_be16(p + 14);
    memcpy(pInfo->lastGmPhaseChange, p + 16, 12);
    pInfo->scaledLastGmFreqChange = (int32_t)ptp_gptp_be32(p + 28);
    pInfo->valid = true;

    return true;
}
