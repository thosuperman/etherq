/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 *
 * QNX OS 8.0 port: Copyright (C) 2026 EtherQ contributors (GPL-3.0-or-later).
 * The frame/index bookkeeping and redundancy logic are taken from the SOEM
 * Linux nicdrv.c; the transport underneath is QNX io-sock BPF.
 */

/** \file
 * \brief
 * EtherCAT raw frame driver for QNX OS 8.0, using the io-sock Berkeley Packet
 * Filter device (/dev/bpf). QNX has no AF_PACKET; BPF is the supported way to
 * send and receive raw Ethernet frames.
 *
 * - A kernel BPF program passes only EtherType 0x88A4 frames to us.
 * - BIOCIMMEDIATE returns each frame as soon as it arrives.
 * - BIOCSHDRCMPLT keeps SOEM's source MAC (used for redundancy routing).
 * - Outgoing frames are filtered out with BIOCSDIRECTION/BIOCSSEESENT.
 * - Optional: the QNX multi-packet write extension (BIOCSMMWRITE). All frames
 *   queued in one process-data cycle leave in a single writev() call. Enable
 *   it with the environment variable ETHERQ_BPF_MMWRITE=1.
 *
 * One read() from BPF may return several frames, each prefixed by a bpf_hdr.
 * They are staged in ec_bpfrxt and handed to SOEM one at a time.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/bpf.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "oshw.h"
#include "osal.h"

#ifndef EC_BPF_DEVICE
#define EC_BPF_DEVICE "/dev/bpf"
#endif

/** Redundancy modes */
enum
{
   /** No redundancy, single NIC mode */
   ECT_RED_NONE,
   /** Double redundant NIC connection */
   ECT_RED_DOUBLE
};

/** Primary source MAC address used for EtherCAT (see SOEM Linux nicdrv.c). */
const uint16 priMAC[3] = EC_PRIMARY_MAC_ARRAY;
/** Secondary source MAC address used for EtherCAT. */
const uint16 secMAC[3] = EC_SECONDARY_MAC_ARRAY;

/** second MAC word is used for identification */
#define RX_PRIM priMAC[1]
/** second MAC word is used for identification */
#define RX_SEC  secMAC[1]

/** Set when the kernel cannot hide our own transmitted frames from us. */
static int ec_bpf_filter_own_frames = 0;

static void ecx_clear_rxbufstat(int *rxbufstat)
{
   int i;
   for (i = 0; i < EC_MAXBUF; i++)
   {
      rxbufstat[i] = EC_BUF_EMPTY;
   }
}

/* Kernel filter: accept EtherType 0x88A4, drop everything else. */
static struct bpf_insn ec_bpf_prog[] = {
    BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 12),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ETH_P_ECAT, 0, 1),
    BPF_STMT(BPF_RET + BPF_K, (u_int)-1),
    BPF_STMT(BPF_RET + BPF_K, 0),
};

/** Open and configure one BPF descriptor bound to ifname. Returns fd or -1. */
static int ec_bpf_open(const char *ifname, ec_bpfrxt *rx, int want_mmwrite, int *mmwrite_on)
{
   struct ifreq ifr;
   struct bpf_program prog;
   u_int on = 1;
   u_int blen = EC_BPF_BUFSIZE;
   int fd;

   *mmwrite_on = 0;
   fd = open(EC_BPF_DEVICE, O_RDWR);
   if (fd < 0)
   {
      return -1;
   }
   /* buffer length and multi-write mode must be set before BIOCSETIF */
   if (ioctl(fd, BIOCSBLEN, &blen) < 0)
   {
      goto fail;
   }
#ifdef BIOCSMMWRITE
   if (want_mmwrite)
   {
      u_int mm = 1;
      if (ioctl(fd, BIOCSMMWRITE, &mm) == 0)
      {
         *mmwrite_on = 1;
         fprintf(stderr, "EtherQ: BPF multi-packet writes (BIOCSMMWRITE) enabled on %s\n", ifname);
      }
      else
      {
         fprintf(stderr, "EtherQ: BIOCSMMWRITE rejected on %s; using single-frame writes\n", ifname);
      }
   }
#else
   (void)want_mmwrite;
#endif
   memset(&ifr, 0, sizeof(ifr));
   strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
   if (ioctl(fd, BIOCSETIF, &ifr) < 0)
   {
      goto fail;
   }
   if (ioctl(fd, BIOCGBLEN, &blen) < 0 || blen > EC_BPF_BUFSIZE)
   {
      goto fail;
   }
   rx->cap = (int)blen;
   rx->len = 0;
   rx->pos = 0;
   if (ioctl(fd, BIOCIMMEDIATE, &on) < 0)
   {
      goto fail;
   }
   /* we supply the complete Ethernet header, including SOEM's source MAC */
   if (ioctl(fd, BIOCSHDRCMPLT, &on) < 0)
   {
      goto fail;
   }
   /* do not hand our own transmitted frames back to us */
#if defined(BIOCSDIRECTION) && defined(BPF_D_IN)
   {
      u_int dir = BPF_D_IN;
      if (ioctl(fd, BIOCSDIRECTION, &dir) < 0)
      {
         ec_bpf_filter_own_frames = 1;
      }
   }
#elif defined(BIOCSSEESENT)
   {
      u_int seesent = 0;
      if (ioctl(fd, BIOCSSEESENT, &seesent) < 0)
      {
         ec_bpf_filter_own_frames = 1;
      }
   }
#else
   ec_bpf_filter_own_frames = 1;
#endif
   /* promiscuous like the Linux driver; ignore failure (broadcast still works) */
   (void)ioctl(fd, BIOCPROMISC, NULL);
   prog.bf_len = sizeof(ec_bpf_prog) / sizeof(ec_bpf_prog[0]);
   prog.bf_insns = ec_bpf_prog;
   if (ioctl(fd, BIOCSETF, &prog) < 0)
   {
      goto fail;
   }
   if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
   {
      goto fail;
   }
   return fd;

fail:
   close(fd);
   return -1;
}

/** Basic setup to connect NIC to BPF.
 * @param[in] port        = port context struct
 * @param[in] ifname      = Name of NIC device, f.e. "vtnet0"
 * @param[in] secondary   = if >0 then use secondary stack instead of primary
 * @return >0 if succeeded
 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
   int i, mm_on = 0, want_mm;
   int *psock;
   ec_bpfrxt *rx;
   pthread_mutexattr_t mutexattr;
   const char *env = getenv("ETHERQ_BPF_MMWRITE");

   want_mm = (env != NULL && env[0] == '1');
   if (secondary)
   {
      if (!port->redport)
      {
         return 0;
      }
      psock = &(port->redport->sockhandle);
      *psock = -1;
      port->redstate = ECT_RED_DOUBLE;
      port->redport->stack.sock = &(port->redport->sockhandle);
      port->redport->stack.txbuf = &(port->txbuf);
      port->redport->stack.txbuflength = &(port->txbuflength);
      port->redport->stack.tempbuf = &(port->redport->tempinbuf);
      port->redport->stack.rxbuf = &(port->redport->rxbuf);
      port->redport->stack.rxbufstat = &(port->redport->rxbufstat);
      port->redport->stack.rxsa = &(port->redport->rxsa);
      port->redport->stack.bpfrx = &(port->redport->bpfrx);
      ecx_clear_rxbufstat(&(port->redport->rxbufstat[0]));
      rx = &(port->redport->bpfrx);
      want_mm = 0; /* batching is only used on the primary port */
   }
   else
   {
      pthread_mutexattr_init(&mutexattr);
      pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
      pthread_mutex_init(&(port->getindex_mutex), &mutexattr);
      pthread_mutex_init(&(port->tx_mutex), &mutexattr);
      pthread_mutex_init(&(port->rx_mutex), &mutexattr);
      pthread_mutexattr_destroy(&mutexattr);
      port->sockhandle = -1;
      port->lastidx = 0;
      port->redstate = ECT_RED_NONE;
      port->stack.sock = &(port->sockhandle);
      port->stack.txbuf = &(port->txbuf);
      port->stack.txbuflength = &(port->txbuflength);
      port->stack.tempbuf = &(port->tempinbuf);
      port->stack.rxbuf = &(port->rxbuf);
      port->stack.rxbufstat = &(port->rxbufstat);
      port->stack.rxsa = &(port->rxsa);
      port->stack.bpfrx = &(port->bpfrx);
      memset(&port->txbatch, 0, sizeof(port->txbatch));
      ecx_clear_rxbufstat(&(port->rxbufstat[0]));
      psock = &(port->sockhandle);
      rx = &(port->bpfrx);
   }

   *psock = ec_bpf_open(ifname, rx, want_mm, &mm_on);
   if (*psock < 0)
   {
      return 0;
   }
   if (!secondary)
   {
      port->txbatch.enabled = mm_on;
   }

   /* setup ethernet headers in tx buffers so we don't have to repeat it */
   for (i = 0; i < EC_MAXBUF; i++)
   {
      ec_setupheader(&(port->txbuf[i]));
      port->rxbufstat[i] = EC_BUF_EMPTY;
   }
   ec_setupheader(&(port->txbuf2));

   return 1;
}

/** Close BPF descriptors used
 * @param[in] port        = port context struct
 * @return 0
 */
int ecx_closenic(ecx_portt *port)
{
   ecx_flush_tx(port);
   if (port->sockhandle >= 0)
   {
      close(port->sockhandle);
      port->sockhandle = -1;
   }
   if ((port->redport) && (port->redport->sockhandle >= 0))
   {
      close(port->redport->sockhandle);
      port->redport->sockhandle = -1;
   }
   return 0;
}

/** Fill buffer with ethernet header structure.
 * Destination MAC is always broadcast.
 * Ethertype is always ETH_P_ECAT.
 * @param[out] p = buffer
 */
void ec_setupheader(void *p)
{
   ec_etherheadert *bp;
   bp = p;
   bp->da0 = htons(0xffff);
   bp->da1 = htons(0xffff);
   bp->da2 = htons(0xffff);
   bp->sa0 = htons(priMAC[0]);
   bp->sa1 = htons(priMAC[1]);
   bp->sa2 = htons(priMAC[2]);
   bp->etype = htons(ETH_P_ECAT);
}

/** Get new frame identifier index and allocate corresponding rx buffer.
 * @param[in] port        = port context struct
 * @return new index.
 */
uint8 ecx_getindex(ecx_portt *port)
{
   uint8 idx;
   uint8 cnt;

   pthread_mutex_lock(&(port->getindex_mutex));

   idx = port->lastidx + 1;
   if (idx >= EC_MAXBUF)
   {
      idx = 0;
   }
   cnt = 0;
   while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF))
   {
      idx++;
      cnt++;
      if (idx >= EC_MAXBUF)
      {
         idx = 0;
      }
   }
   port->rxbufstat[idx] = EC_BUF_ALLOC;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = EC_BUF_ALLOC;
   port->lastidx = idx;

   pthread_mutex_unlock(&(port->getindex_mutex));

   return idx;
}

/** Set rx buffer status.
 * @param[in] port        = port context struct
 * @param[in] idx      = index in buffer array
 * @param[in] bufstat  = status to set
 */
void ecx_setbufstat(ecx_portt *port, uint8 idx, int bufstat)
{
   port->rxbufstat[idx] = bufstat;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = bufstat;
}

/** Send all frames queued for a multi-packet write in one writev() call. */
static int ecx_flush_tx_locked(ecx_portt *port)
{
#ifdef BIOCSMMWRITE
   ec_bpftxbatcht *b = &port->txbatch;
   int i, rval = 0;

   if (b->count == 0)
   {
      return 0;
   }
   if (writev(port->sockhandle, b->iov, 2 * b->count) < 0)
   {
      rval = -1;
      for (i = 0; i < b->count; i++)
      {
         port->rxbufstat[b->idx[i]] = EC_BUF_EMPTY;
      }
   }
   b->count = 0;
   return rval;
#else
   (void)port;
   return 0;
#endif
}

int ecx_flush_tx(ecx_portt *port)
{
   int rval;
   if (!port->txbatch.enabled || port->txbatch.count == 0)
   {
      return 0;
   }
   pthread_mutex_lock(&(port->tx_mutex));
   rval = ecx_flush_tx_locked(port);
   pthread_mutex_unlock(&(port->tx_mutex));
   return rval;
}

#ifdef BIOCSMMWRITE
_Static_assert(sizeof(struct bpf_whdr) + sizeof(long) <= sizeof(((ec_bpftxbatcht *)0)->hdr[0]),
               "bpf_whdr plus alignment padding must fit the header slot");

/** Queue one frame for the next multi-packet write. Each packet is preceded by a
 * bpf_whdr whose bh_hdrlen includes the alignment padding (QNX bpf reference). */
static int ecx_queue_frame(ecx_portt *port, uint8 idx, int len)
{
   ec_bpftxbatcht *b = &port->txbatch;
   struct bpf_whdr wh;
   int pad, n, rval = len;

   pthread_mutex_lock(&(port->tx_mutex));
   if (b->count >= EC_BPF_MAXBATCH)
   {
      ecx_flush_tx_locked(port);
   }
   n = b->count;
   pad = (int)(BPF_WORDALIGN(len) - len);
   memset(&wh, 0, sizeof(wh));
   wh.bh_hdrlen = (uint32_t)(sizeof(wh) + pad);
   wh.bh_datalen = (uint32_t)len;
   memset(b->hdr[n], 0, sizeof(b->hdr[n]));
   memcpy(b->hdr[n], &wh, sizeof(wh));
   b->iov[2 * n].iov_base = b->hdr[n];
   b->iov[2 * n].iov_len = sizeof(wh) + pad;
   b->iov[2 * n + 1].iov_base = port->txbuf[idx];
   b->iov[2 * n + 1].iov_len = (size_t)len;
   b->idx[n] = idx;
   b->stacknumber[n] = 0;
   b->count++;
   pthread_mutex_unlock(&(port->tx_mutex));
   return rval;
}
#endif

/** Transmit buffer (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx         = index in tx buffer array
 * @param[in] stacknumber  = 0=Primary 1=Secondary stack
 * @return send result (bytes) or -1
 */
int ecx_outframe(ecx_portt *port, uint8 idx, int stacknumber)
{
   int lp, rval;
   ec_stackT *stack;

   stack = (!stacknumber) ? &(port->stack) : &(port->redport->stack);
   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;
#ifdef BIOCSMMWRITE
   if (!stacknumber && port->txbatch.enabled)
   {
      return ecx_queue_frame(port, idx, lp);
   }
#endif
   rval = (int)write(*stack->sock, (*stack->txbuf)[idx], (size_t)lp);
   if (rval == -1)
   {
      (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;
   }
   return rval;
}

/** Transmit buffer (non blocking), with a dummy frame on the secondary port in
 * redundant mode.
 * @param[in] port        = port context struct
 * @param[in] idx = index in tx buffer array
 * @return send result
 */
int ecx_outframe_red(ecx_portt *port, uint8 idx)
{
   ec_comt *datagramP;
   ec_etherheadert *ehp;
   int rval;

   ehp = (ec_etherheadert *)&(port->txbuf[idx]);
   ehp->sa1 = htons(priMAC[1]);
   rval = ecx_outframe(port, idx, 0);
   if (port->redstate != ECT_RED_NONE)
   {
      pthread_mutex_lock(&(port->tx_mutex));
      ehp = (ec_etherheadert *)&(port->txbuf2);
      datagramP = (ec_comt *)&(port->txbuf2[ETH_HEADERSIZE]);
      datagramP->index = idx;
      ehp->sa1 = htons(secMAC[1]);
      port->redport->rxbufstat[idx] = EC_BUF_TX;
      if (write(port->redport->sockhandle, &(port->txbuf2), (size_t)port->txbuflength2) == -1)
      {
         port->redport->rxbufstat[idx] = EC_BUF_EMPTY;
      }
      pthread_mutex_unlock(&(port->tx_mutex));
   }
   return rval;
}

/** True if staged BPF data is waiting to be parsed. */
static int ecx_rx_pending(ec_stackT *stack)
{
   return stack->bpfrx->pos < stack->bpfrx->len;
}

/** Non blocking read from BPF. Put one frame in the temporary buffer.
 * @param[in] port        = port context struct
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return >0 if frame is available and read
 */
static int ecx_recvpkt(ecx_portt *port, int stacknumber)
{
   ec_stackT *stack;
   ec_bpfrxt *rx;
   struct bpf_hdr bh;
   const ec_etherheadert *eh;
   ssize_t n;

   stack = (!stacknumber) ? &(port->stack) : &(port->redport->stack);
   rx = stack->bpfrx;
   port->tempinbufs = 0;
   for (;;)
   {
      if (rx->pos >= rx->len)
      {
         n = read(*stack->sock, rx->buf, (size_t)rx->cap);
         if (n <= 0)
         {
            rx->len = rx->pos = 0;
            return 0;
         }
         rx->len = (int)n;
         rx->pos = 0;
      }
      if (rx->pos + (int)sizeof(bh) > rx->len)
      {
         rx->len = rx->pos = 0;
         continue;
      }
      memcpy(&bh, rx->buf + rx->pos, sizeof(bh));
      {
         size_t hdrlen = bh.bh_hdrlen, caplen = bh.bh_caplen;
         size_t start = (size_t)rx->pos + hdrlen;
         size_t next = (size_t)rx->pos + BPF_WORDALIGN(hdrlen + caplen);
         /* bh_hdrlen is chosen so the payload is aligned and may be smaller than
          * sizeof(struct bpf_hdr) (trailing padding); the minimum is the unpadded
          * header, which ends after bh_hdrlen. */
         if (hdrlen < offsetof(struct bpf_hdr, bh_hdrlen) + sizeof(bh.bh_hdrlen) ||
             next <= (size_t)rx->pos || next > (size_t)rx->len + sizeof(long))
         {
            rx->len = rx->pos = 0; /* corrupt record: drop the rest of this buffer */
            continue;
         }
         rx->pos = (int)next;
         if (start + caplen > (size_t)rx->len || caplen > sizeof(*stack->tempbuf) ||
             caplen < ETH_HEADERSIZE)
         {
            continue; /* truncated or oversized capture: skip */
         }
         eh = (const ec_etherheadert *)(rx->buf + start);
         if (ec_bpf_filter_own_frames && eh->sa0 == htons(priMAC[0]) &&
             eh->sa1 == htons(priMAC[1]) && eh->sa2 == htons(priMAC[2]))
         {
            /* our own outgoing frame echoed back: a slave would have set the
             * locally administered bit in sa0, so this frame never left */
            continue;
         }
         memcpy(*stack->tempbuf, rx->buf + start, caplen);
         port->tempinbufs = (int)caplen;
         return 1;
      }
   }
}

/** Non blocking receive frame function. Uses RX buffer and index to combine
 * read frame with transmitted frame. See SOEM Linux nicdrv.c for details.
 *
 * @param[in] port        = port context struct
 * @param[in] idx         = requested index of frame
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME or EC_OTHERFRAME.
 */
int ecx_inframe(ecx_portt *port, uint8 idx, int stacknumber)
{
   uint16 l;
   int rval;
   uint8 idxf;
   ec_etherheadert *ehp;
   ec_comt *ecp;
   ec_stackT *stack;
   ec_bufT *rxbuf;

   stack = (!stacknumber) ? &(port->stack) : &(port->redport->stack);
   rval = EC_NOFRAME;
   rxbuf = &(*stack->rxbuf)[idx];
   if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
   {
      l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
      rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
      (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
   }
   else
   {
      pthread_mutex_lock(&(port->rx_mutex));
      if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
      {
         l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
         rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
         (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
      }
      else if (ecx_recvpkt(port, stacknumber))
      {
         rval = EC_OTHERFRAME;
         ehp = (ec_etherheadert *)(stack->tempbuf);
         if (ehp->etype == htons(ETH_P_ECAT))
         {
            stack->rxcnt++;
            ecp = (ec_comt *)(&(*stack->tempbuf)[ETH_HEADERSIZE]);
            l = etohs(ecp->elength) & 0x0fff;
            if (l <= EC_BUFSIZE - 2)
            {
               idxf = ecp->index;
               if (idxf == idx)
               {
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idx] - ETH_HEADERSIZE);
                  rval = ((*rxbuf)[l] + ((uint16)((*rxbuf)[l + 1]) << 8));
                  (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
                  (*stack->rxsa)[idx] = ntohs(ehp->sa1);
               }
               else if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX)
               {
                  rxbuf = &(*stack->rxbuf)[idxf];
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idxf] - ETH_HEADERSIZE);
                  (*stack->rxbufstat)[idxf] = EC_BUF_RCVD;
                  (*stack->rxsa)[idxf] = ntohs(ehp->sa1);
               }
            }
         }
      }
      pthread_mutex_unlock(&(port->rx_mutex));
   }
   return rval;
}

/** Wait (up to 1 ms) until a BPF descriptor becomes readable, unless frames are
 * already staged. poll() returns as soon as a frame arrives. */
static void ecx_wait_readable(ecx_portt *port, uint8 idx)
{
   struct pollfd fds[2];
   int n = 0;

   /* another thread may already have read our frame from BPF and parked it */
   if (port->rxbufstat[idx] == EC_BUF_RCVD ||
       (port->redstate != ECT_RED_NONE && port->redport->rxbufstat[idx] == EC_BUF_RCVD))
   {
      return;
   }
   if (ecx_rx_pending(&port->stack) ||
       (port->redstate != ECT_RED_NONE && ecx_rx_pending(&port->redport->stack)))
   {
      return;
   }
   fds[n].fd = port->sockhandle;
   fds[n].events = POLLIN;
   n++;
   if (port->redstate != ECT_RED_NONE)
   {
      fds[n].fd = port->redport->sockhandle;
      fds[n].events = POLLIN;
      n++;
   }
   (void)poll(fds, (nfds_t)n, 1);
}

/** Blocking redundant receive frame function (see SOEM Linux nicdrv.c).
 *
 * @param[in] port        = port context struct
 * @param[in] idx = requested index of frame
 * @param[in] timer = absolute timeout time
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
static int ecx_waitinframe_red(ecx_portt *port, uint8 idx, osal_timert *timer)
{
   osal_timert timer2;
   int wkc = EC_NOFRAME;
   int wkc2 = EC_NOFRAME;
   int primrx, secrx;

   /* frames queued for a multi-packet write must leave before we wait */
   ecx_flush_tx(port);
   if (port->redstate == ECT_RED_NONE)
      wkc2 = 0;
   do
   {
      if (wkc <= EC_NOFRAME)
         wkc = ecx_inframe(port, idx, 0);
      if (port->redstate != ECT_RED_NONE && wkc2 <= EC_NOFRAME)
         wkc2 = ecx_inframe(port, idx, 1);
      if ((wkc <= EC_NOFRAME) || (wkc2 <= EC_NOFRAME))
         ecx_wait_readable(port, idx);
   } while (((wkc <= EC_NOFRAME) || (wkc2 <= EC_NOFRAME)) && !osal_timer_is_expired(timer));
   if (port->redstate != ECT_RED_NONE)
   {
      primrx = 0;
      if (wkc > EC_NOFRAME) primrx = port->rxsa[idx];
      secrx = 0;
      if (wkc2 > EC_NOFRAME) secrx = port->redport->rxsa[idx];

      if (((primrx == RX_SEC) && (secrx == RX_PRIM)))
      {
         memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         wkc = wkc2;
      }
      if (((primrx == 0) && (secrx == RX_SEC)) ||
          ((primrx == RX_PRIM) && (secrx == RX_SEC)))
      {
         if ((primrx == RX_PRIM) && (secrx == RX_SEC))
         {
            memcpy(&(port->txbuf[idx][ETH_HEADERSIZE]), &(port->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         }
         osal_timer_start(&timer2, EC_TIMEOUTRET);
         ecx_outframe(port, idx, 1);
         do
         {
            wkc2 = ecx_inframe(port, idx, 1);
            if (wkc2 <= EC_NOFRAME)
               ecx_wait_readable(port, idx);
         } while ((wkc2 <= EC_NOFRAME) && !osal_timer_is_expired(&timer2));
         if (wkc2 > EC_NOFRAME)
         {
            memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
            wkc = wkc2;
         }
      }
   }
   return wkc;
}

/** Blocking receive frame function. Calls ecx_waitinframe_red().
 * @param[in] port        = port context struct
 * @param[in] idx       = requested index of frame
 * @param[in] timeout   = timeout in us
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
int ecx_waitinframe(ecx_portt *port, uint8 idx, int timeout)
{
   osal_timert timer;

   osal_timer_start(&timer, (uint32)timeout);
   return ecx_waitinframe_red(port, idx, &timer);
}

/** Blocking send and receive frame function. Used for non processdata frames.
 * Retries while time is left and the result is WKC=0 or no frame received.
 *
 * @param[in] port        = port context struct
 * @param[in] idx      = index of frame
 * @param[in] timeout  = timeout in us
 * @return Workcounter or EC_NOFRAME
 */
int ecx_srconfirm(ecx_portt *port, uint8 idx, int timeout)
{
   int wkc = EC_NOFRAME;
   osal_timert timer1, timer2;

   osal_timer_start(&timer1, (uint32)timeout);
   do
   {
      ecx_outframe_red(port, idx);
      if (timeout < EC_TIMEOUTRET)
      {
         osal_timer_start(&timer2, (uint32)timeout);
      }
      else
      {
         osal_timer_start(&timer2, EC_TIMEOUTRET);
      }
      wkc = ecx_waitinframe_red(port, idx, &timer2);
   } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired(&timer1));

   return wkc;
}
