/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ecatd - EtherCAT master daemon for QNX OS 8.0 (shared state). */
#ifndef ETHERQ_ECATD_H
#define ETHERQ_ECATD_H

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

#include "soem/soem.h"
#include "ecatcore.h"
#include "etherq/ecat_devctl.h"

#define ECATD_VERSION "0.1.0"
#define ECATD_IOMAP_SIZE 4096
#define ECATD_MAX_SLAVES 64

typedef struct
{
   uint16_t state;
   uint16_t al_status_code;
} ecatd_slave_state_t;

typedef struct
{
   /* configuration */
   char ifname[32];
   int64_t period_ns;
   int cycle_prio;
   int supervisor_prio;
   int verbose;

   /* SOEM */
   ecx_contextt ctx;
   uint8_t iomap[ECATD_IOMAP_SIZE];
   uint32_t obytes, ibytes;
   int expected_wkc;

   /* shared with resource manager threads, protected by lock (priority inheritance) */
   pthread_mutex_t lock;
   eq_outputs_t out;               /* staged outputs + byte ownership */
   uint8_t *in_snap;               /* inputs of the last completed cycle */
   volatile uint64_t cycle;        /* completed cycles */
   ecat_stats_t stats;
   eq_stat_t wake, exec;
   ecatd_slave_state_t slaves[ECATD_MAX_SLAVES + 1];
   uint8_t no_answer[ECATD_MAX_SLAVES + 1]; /* consecutive supervisor passes without a state reply */
   uint16_t group_state;
   uint32_t clients;

   /* control */
   volatile int running;
   volatile int need_check;        /* set by the cycle thread on a WKC error */
   volatile uint16_t target_state; /* state the supervisor drives the bus to */
   volatile int state_request;     /* new target_state pending */

   /* notification: cycle thread -> resource manager (pulse) */
   int resmgr_coid;
   int cycle_pulse_code;

   pthread_t cycle_tid, supervisor_tid;
} ecatd_t;

/* master.c */
int master_open(ecatd_t *d);
int master_start(ecatd_t *d);
void master_request_state(ecatd_t *d, uint16_t state);
void master_shutdown(ecatd_t *d);
const char *master_state_name(uint16_t state);

/* resmgr.c */
int resmgr_setup(ecatd_t *d);
int resmgr_run(ecatd_t *d); /* does not return on success */

/* privileges.c */
int privileges_drop(uid_t uid, gid_t gid);

int64_t ecatd_now_ns(void);

#endif
