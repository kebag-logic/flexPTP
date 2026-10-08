/* (C) Kebag-Logic, 2026 */

#include "gptp.h"

#include "ptp_core.h"

///\cond 0
#define S (gPtpCoreState)
///\endcond

void ptp_gptp_init(void) {
    S.gptp.linkUp = true;
}

void ptp_gptp_reset(void) {
    return;
}

void ptp_gptp_tick(void) {
    ptp_gptp_publish_status();
}

void ptp_gptp_link_state(bool up) {
    S.gptp.linkUp = up;

    ptp_gptp_publish_status();
}

void ptp_gptp_aux_timer(void) {
    return;
}
