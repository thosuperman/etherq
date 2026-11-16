/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ecatcore - OS-independent parts of the EtherQ master daemon.
 *
 *  - Output ownership: every byte of the output process image belongs to at most one
 *    client. A client that disappears has its bytes zeroed and released (safe stop).
 *  - Cycle statistics: wake-up latency and execution time accumulators.
 *  - Monotonic deadline arithmetic.
 *
 * Built and unit-tested on the host. Linked into ecatd on QNX.
 */
#ifndef ETHERQ_ECATCORE_H
#define ETHERQ_ECATCORE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ output image */
typedef struct
{
   uint8_t *staged;     /* next output image to send */
   const void **owner;  /* owner per byte, NULL = free */
   size_t nbytes;
} eq_outputs_t;

int eq_outputs_init(eq_outputs_t *o, size_t nbytes);
void eq_outputs_free(eq_outputs_t *o);

/* Write len bytes at off on behalf of who.
 * Returns 0, -ERANGE if outside the image, -EBUSY if a byte is owned by someone else.
 * Nothing is written unless the whole range can be claimed. */
int eq_outputs_write(eq_outputs_t *o, const void *who, size_t off, const uint8_t *src, size_t len);

/* Zero and release every byte owned by who. Returns the number of bytes released. */
size_t eq_outputs_release(eq_outputs_t *o, const void *who);

/* Number of bytes currently owned by who (who == NULL counts all owned bytes). */
size_t eq_outputs_owned(const eq_outputs_t *o, const void *who);

/* ------------------------------------------------------------- statistics */
typedef struct
{
   uint64_t n;
   int64_t min;
   int64_t max;
   int64_t last;
   double sum;
} eq_stat_t;

void eq_stat_reset(eq_stat_t *s);
void eq_stat_add(eq_stat_t *s, int64_t v);
int64_t eq_stat_mean(const eq_stat_t *s);

/* --------------------------------------------------------- time helpers */
int64_t eq_ts_to_ns(const struct timespec *ts);
void eq_ns_to_ts(int64_t ns, struct timespec *ts);
/* Absolute deadline of cycle k for a schedule starting at start_ns with period_ns. */
int64_t eq_cycle_deadline(int64_t start_ns, int64_t period_ns, uint64_t k);
/* Number of whole periods missed if a cycle that should start at deadline_ns starts at now_ns. */
uint64_t eq_missed_periods(int64_t deadline_ns, int64_t now_ns, int64_t period_ns);

#ifdef __cplusplus
}
#endif

#endif
