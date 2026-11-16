/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * EtherQ resource-manager interface (QNX).
 *
 *   /dev/ecat/in      read-only   input process image (slave -> master), poll()able per cycle
 *   /dev/ecat/out     read/write  output process image (master -> slave), byte ownership
 *   /dev/ecat/status  read-only   human-readable status text
 *
 * Offsets in /dev/ecat/in and /dev/ecat/out are relative to the start of the
 * group's input and output images. ecat_slave_t.ioffset/ooffset give each slave's
 * position in them. Writing to /dev/ecat/out claims the written bytes for the
 * calling open file. Other clients get EBUSY for those bytes. When the file is
 * closed, or the client process dies, ecatd zeroes and releases them.
 *
 * Ownership belongs to the open file, so a dup()'d or inherited descriptor shares
 * it: open /dev/ecat/out with O_CLOEXEC so child processes do not keep it alive.
 * DCMD_ECAT_SET_STATE and DCMD_ECAT_RESET_STATS require /dev/ecat/out opened for
 * writing (EPERM otherwise); the GET commands work on any node.
 */
#ifndef ETHERQ_ECAT_DEVCTL_H
#define ETHERQ_ECAT_DEVCTL_H

#include <stdint.h>
#include <devctl.h>

#define ECAT_PATH_IN     "/dev/ecat/in"
#define ECAT_PATH_OUT    "/dev/ecat/out"
#define ECAT_PATH_STATUS "/dev/ecat/status"

#define ECAT_API_VERSION 1
#define ECAT_NAME_LEN    41

/* EtherCAT AL states (same encoding as the AL status register) */
#define ECAT_STATE_INIT   0x01
#define ECAT_STATE_PREOP  0x02
#define ECAT_STATE_BOOT   0x03
#define ECAT_STATE_SAFEOP 0x04
#define ECAT_STATE_OP     0x08
#define ECAT_STATE_ERROR  0x10

typedef struct
{
   uint32_t api_version;
   uint32_t slave_count;
   uint32_t group_state;   /* lowest AL state of all slaves */
   uint32_t expected_wkc;
   uint32_t obytes;
   uint32_t ibytes;
   uint32_t period_ns;
   uint32_t cycle_priority;
   char ifname[32];
} ecat_info_t;

typedef struct
{
   uint32_t index;         /* in: slave number 1..slave_count */
   uint32_t state;         /* AL state incl. ECAT_STATE_ERROR */
   uint32_t al_status_code;
   uint32_t vendor_id;
   uint32_t product_code;
   uint32_t revision;
   uint32_t configured_address;
   uint32_t obits;
   uint32_t ibits;
   uint32_t ooffset;       /* byte offset in /dev/ecat/out */
   uint32_t ioffset;       /* byte offset in /dev/ecat/in */
   char name[ECAT_NAME_LEN];
} ecat_slave_t;

typedef struct
{
   uint64_t cycles;
   uint64_t wkc_errors;      /* cycles with wkc < expected */
   uint64_t missed_periods;  /* timer periods that elapsed without a cycle */
   uint64_t overruns;        /* cycles whose execution exceeded the period */
   uint64_t recoveries;      /* successful slave recovery actions */
   uint32_t last_wkc;
   uint32_t clients;         /* open files on /dev/ecat/out */
   /* Wake-up latency: actual cycle start minus scheduled start (ns), and cycle
    * execution time (ns). Measured on the target with CLOCK_MONOTONIC. */
   int64_t wake_min_ns, wake_mean_ns, wake_max_ns;
   int64_t exec_min_ns, exec_mean_ns, exec_max_ns;
} ecat_stats_t;

#define DCMD_ECAT_GET_INFO    __DIOF(_DCMD_MISC, 0x01, ecat_info_t)
#define DCMD_ECAT_GET_SLAVE   __DIOTF(_DCMD_MISC, 0x02, ecat_slave_t)
#define DCMD_ECAT_GET_STATS   __DIOF(_DCMD_MISC, 0x03, ecat_stats_t)
#define DCMD_ECAT_RESET_STATS __DION(_DCMD_MISC, 0x04)
#define DCMD_ECAT_SET_STATE   __DIOT(_DCMD_MISC, 0x05, uint32_t)

#endif
