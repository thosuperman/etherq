/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 *
 * QNX OS 8.0 port: Copyright (C) 2026 EtherQ contributors (GPL-3.0-or-later).
 */

/** \file
 * \brief
 * Headerfile for nicdrv.c (QNX io-sock BPF transport)
 */

#ifndef _nicdrvh_
#define _nicdrvh_

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
#include <sys/uio.h>

/** BPF read buffer size (bytes). Must be set before BIOCSETIF. */
#ifndef EC_BPF_BUFSIZE
#define EC_BPF_BUFSIZE 32768
#endif

/** Maximum frames that can be batched into one multi-packet BPF write. */
#define EC_BPF_MAXBATCH EC_MAXBUF

/** Per-socket BPF receive state: one read() may return several packets. */
typedef struct
{
   /** bytes valid in buf after the last read() */
   int len;
   /** parse position of the next bpf_hdr */
   int pos;
   /** buffer size negotiated with the kernel (BIOCGBLEN) */
   int cap;
   uint8 buf[EC_BPF_BUFSIZE] __attribute__((aligned(8)));
} ec_bpfrxt;

/** Pending multi-packet write (QNX BIOCSMMWRITE extension). */
typedef struct
{
   /** set when BIOCSMMWRITE was accepted by io-sock */
   int enabled;
   int count;
   struct iovec iov[3 * EC_BPF_MAXBATCH];
   uint8 hdr[EC_BPF_MAXBATCH][32];
   uint8 idx[EC_BPF_MAXBATCH];
   int stacknumber[EC_BPF_MAXBATCH];
} ec_bpftxbatcht;

/** pointer structure to Tx and Rx stacks */
typedef struct
{
   /** BPF file descriptor used */
   int *sock;
   /** tx buffer */
   ec_bufT (*txbuf)[EC_MAXBUF];
   /** tx buffer lengths */
   int (*txbuflength)[EC_MAXBUF];
   /** temporary receive buffer */
   ec_bufT *tempbuf;
   /** rx buffers */
   ec_bufT (*rxbuf)[EC_MAXBUF];
   /** rx buffer status fields */
   int (*rxbufstat)[EC_MAXBUF];
   /** received MAC source address (middle word) */
   int (*rxsa)[EC_MAXBUF];
   /** BPF receive staging buffer */
   ec_bpfrxt *bpfrx;
   /** number of received frames */
   uint64 rxcnt;
} ec_stackT;

/** pointer structure to buffers for redundant port */
typedef struct
{
   ec_stackT stack;
   int sockhandle;
   /** rx buffers */
   ec_bufT rxbuf[EC_MAXBUF];
   /** rx buffer status */
   int rxbufstat[EC_MAXBUF];
   /** rx MAC source address */
   int rxsa[EC_MAXBUF];
   /** temporary rx buffer */
   ec_bufT tempinbuf;
   /** BPF receive staging buffer */
   ec_bpfrxt bpfrx;
} ecx_redportt;

/** pointer structure to buffers, vars and mutexes for port instantiation */
typedef struct
{
   ec_stackT stack;
   int sockhandle;
   /** rx buffers */
   ec_bufT rxbuf[EC_MAXBUF];
   /** rx buffer status */
   int rxbufstat[EC_MAXBUF];
   /** rx MAC source address */
   int rxsa[EC_MAXBUF];
   /** temporary rx buffer */
   ec_bufT tempinbuf;
   /** temporary rx buffer status */
   int tempinbufs;
   /** transmit buffers */
   ec_bufT txbuf[EC_MAXBUF];
   /** transmit buffer lengths */
   int txbuflength[EC_MAXBUF];
   /** temporary tx buffer */
   ec_bufT txbuf2;
   /** temporary tx buffer length */
   int txbuflength2;
   /** last used frame index */
   uint8 lastidx;
   /** current redundancy state */
   int redstate;
   /** pointer to redundancy port and buffers */
   ecx_redportt *redport;
   /** BPF receive staging buffer (primary) */
   ec_bpfrxt bpfrx;
   /** pending multi-packet write batch */
   ec_bpftxbatcht txbatch;
   pthread_mutex_t getindex_mutex;
   pthread_mutex_t tx_mutex;
   pthread_mutex_t rx_mutex;
} ecx_portt;

extern const uint16 priMAC[3];
extern const uint16 secMAC[3];

void ec_setupheader(void *p);
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary);
int ecx_closenic(ecx_portt *port);
void ecx_setbufstat(ecx_portt *port, uint8 idx, int bufstat);
uint8 ecx_getindex(ecx_portt *port);
int ecx_outframe(ecx_portt *port, uint8 idx, int sock);
int ecx_outframe_red(ecx_portt *port, uint8 idx);
int ecx_waitinframe(ecx_portt *port, uint8 idx, int timeout);
int ecx_srconfirm(ecx_portt *port, uint8 idx, int timeout);

/** Flush frames queued for a multi-packet BPF write (no-op when batching is off). */
int ecx_flush_tx(ecx_portt *port);

#ifdef __cplusplus
}
#endif

#endif
