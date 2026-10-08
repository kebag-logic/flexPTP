/**
 ******************************************************************************
 * @file    flexptpd_shm.h
 * @copyright Kebag-Logic, 2026
 * @brief   The status block flexptpd publishes in /dev/shm, and a reader for
 * it.
 *
 * The block is a PtpGptpStatus (gptp_status.h) behind a small header, under a
 * seqlock: the writer makes seq odd, writes the status, then makes it even
 * again. A reader copies the status between two reads of an equal, even seq.
 *
 * The block lives in /dev/shm/NAME (default "flexptpd.<interface>"). flexptpd
 * refreshes it whenever the core's snapshot changes (PtpGptpStatus.updates,
 * about once per heartbeat) and removes it on a clean exit. A reader that
 * still holds the mapping of a removed block sees `updates` stop moving.
 ******************************************************************************
 */

#ifndef FLEXPTPD_SHM_H
#define FLEXPTPD_SHM_H

#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <flexptp/gptp_status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEXPTPD_SHM_MAGIC "FLXPTPD" ///< 8 bytes with the terminating zero

/**
 * @brief The status block.
 */
typedef struct {
    char magic[8];         ///< FLEXPTPD_SHM_MAGIC
    uint32_t layout;       ///< PTP_GPTP_STATUS_VERSION of the status below
    _Atomic uint32_t seq;  ///< seqlock: odd while the status is being written
    uint32_t size;         ///< sizeof(PtpGptpStatus)
    uint32_t _r;           ///< reserved, 0
    PtpGptpStatus status;  ///< the gPTP status
} FlexptpdShm;

/**
 * Map a status block read-only and check its header.
 *
 * @param name the name in /dev/shm, without the leading '/'
 * @return the mapping, or NULL if missing or not a block of this layout
 */
static inline const FlexptpdShm *flexptpd_shm_open(const char *name) {
    char path[256];
    path[0] = '/';
    strncpy(path + 1, name, sizeof(path) - 2);
    path[sizeof(path) - 1] = '\0';

    int fd = shm_open(path, O_RDONLY, 0);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if ((fstat(fd, &st) < 0) || ((size_t)st.st_size < sizeof(FlexptpdShm))) {
        close(fd);
        return NULL;
    }

    void *p = mmap(NULL, sizeof(FlexptpdShm), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (p == MAP_FAILED) {
        return NULL;
    }

    const FlexptpdShm *pShm = (const FlexptpdShm *)p;

    bool ok = (memcmp(pShm->magic, FLEXPTPD_SHM_MAGIC, sizeof(FLEXPTPD_SHM_MAGIC)) == 0) &&
              (pShm->layout == PTP_GPTP_STATUS_VERSION) &&
              (pShm->size == sizeof(PtpGptpStatus));

    if (!ok) {
        munmap(p, sizeof(FlexptpdShm));
        return NULL;
    }

    return pShm;
}

/**
 * Release a mapping of flexptpd_shm_open().
 *
 * @param pShm the mapping
 */
static inline void flexptpd_shm_close(const FlexptpdShm *pShm) {
    if (pShm != NULL) {
        munmap((void *)pShm, sizeof(FlexptpdShm));
    }
}

/**
 * Copy the status out under the seqlock.
 *
 * @param pShm the mapping
 * @param pOut where to copy it
 * @return false if no consistent copy could be taken (a writer kept writing)
 */
static inline bool flexptpd_shm_read(const FlexptpdShm *pShm, PtpGptpStatus *pOut) {
    for (int tries = 0; tries < 1000; tries++) {
        uint32_t s1 = atomic_load_explicit(&pShm->seq, memory_order_acquire);

        if (s1 & 1u) {
            continue;
        }

        memcpy(pOut, (const void *)&pShm->status, sizeof(PtpGptpStatus));

        atomic_thread_fence(memory_order_acquire);

        uint32_t s2 = atomic_load_explicit(&pShm->seq, memory_order_relaxed);
        if (s1 == s2) {
            return true;
        }
    }

    return false;
}

#ifdef __cplusplus
}
#endif

#endif // FLEXPTPD_SHM_H
