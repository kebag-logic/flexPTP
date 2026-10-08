/* (C) Kebag-Logic, 2026 */

#include "gptp_settings.h"

#include "event.h"
#include "minmax.h"
#include "ptp_core.h"
#include "task_ptp.h"

///\cond 0
#define S (gPtpCoreState)
///\endcond

#define RSYNC_RATE_PCT_MIN (10)
#define RSYNC_RATE_PCT_MAX (1000)

/**
 * Have the core apply the settings in its own context.
 */
static void apply(void) {
    PtpCoreEvent event = {.code = PTP_CEV_GPTP_APPLY, .w = {.w = 0}, .dw = {.dw = 0}};
    ptp_event_enqueue(&event);
}

// ---- 802.1AS ----

void ptp_gptp_set_neighbor_prop_delay_thresh(int64_t ns) {
    S.gptp.neighborPropDelayThresh_ns = ns;
    apply();
}

int64_t ptp_gptp_get_neighbor_prop_delay_thresh(void) {
    return S.gptp.neighborPropDelayThresh_ns;
}

void ptp_gptp_set_allowed_lost_responses(uint16_t n) {
    S.gptp.allowedLostResponses = n;
    apply();
}

uint16_t ptp_gptp_get_allowed_lost_responses(void) {
    return S.gptp.allowedLostResponses;
}

void ptp_gptp_set_gm_capable(bool en) {
    S.gptp.gmCapable = en;
    apply();
}

bool ptp_gptp_get_gm_capable(void) {
    return S.gptp.gmCapable;
}

// ---- reverse sync ----

void ptp_rsync_enable(bool en) {
    S.gptp.rsync.enabled = en;
    apply();
}

bool ptp_rsync_is_enabled(void) {
    return S.gptp.rsync.enabled;
}

bool ptp_rsync_set_domain(uint8_t domain) {
    // the measured domain is not for the recovered clock
    if (domain == S.profile.domainNumber) {
        return false;
    }

    S.gptp.rsync.domain = domain;
    apply();

    return true;
}

uint8_t ptp_rsync_get_domain(void) {
    return S.gptp.rsync.domain;
}

void ptp_rsync_set_rate_pct(uint16_t pct) {
    S.gptp.rsync.ratePct = MIN(MAX(pct, RSYNC_RATE_PCT_MIN), RSYNC_RATE_PCT_MAX);
    S.gptp.rsync.interval_ms = 0;
    apply();
}

uint16_t ptp_rsync_get_rate_pct(void) {
    return S.gptp.rsync.ratePct;
}

void ptp_rsync_set_interval_ms(uint32_t ms) {
    S.gptp.rsync.interval_ms = ms;
    apply();
}

uint32_t ptp_rsync_get_interval_ms(void) {
    return S.gptp.rsync.interval_ms;
}
