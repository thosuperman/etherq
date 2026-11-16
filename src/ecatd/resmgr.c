/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ecatd resource manager: exposes the EtherCAT process image as files.
 *
 *   /dev/ecat/in      read, pread, poll/select (readable once per new cycle)
 *   /dev/ecat/out     write/pwrite claim bytes; read returns the staged outputs
 *   /dev/ecat/status  text status (cat /dev/ecat/status)
 *
 * QNX delivers a close (io_close_ocb) for every open file when a client process
 * exits or crashes. ecatd uses that to zero and release the outputs the client
 * owned: a deterministic safe stop, which the microkernel guarantees.
 */
/* Custom attribute/OCB types and the dispatch-based thread pool must be declared
 * before any QNX header: <sys/dispatch.h> pulls in <sys/resmgr.h>, which fixes the
 * handle/OCB types used by resmgr_io_funcs_t. */
struct ecat_attr;
struct ecat_ocb;
#define IOFUNC_ATTR_T       struct ecat_attr
#define IOFUNC_OCB_T        struct ecat_ocb
#define RESMGR_HANDLE_T     struct ecat_attr
#define RESMGR_OCB_T        struct ecat_ocb
#define THREAD_POOL_PARAM_T dispatch_context_t

#include "ecatd.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>

enum
{
   NODE_IN,
   NODE_OUT,
   NODE_STATUS
};

struct ecat_attr
{
   iofunc_attr_t attr; /* must be first */
   int node;
};

struct ecat_ocb
{
   iofunc_ocb_t ocb; /* must be first */
   uint64_t seen_cycle;
   char *text;
   size_t textlen;
};

static ecatd_t *g_d;
static dispatch_t *g_dpp;
static struct ecat_attr g_attr[3];
static iofunc_mount_t g_mount;
static iofunc_funcs_t g_ocb_funcs;
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t g_io_funcs;
static iofunc_notify_t g_notify[3];

/* ------------------------------------------------------------------ OCBs */
static struct ecat_ocb *ocb_calloc(resmgr_context_t *ctp, struct ecat_attr *attr)
{
   (void)ctp;
   (void)attr;
   return calloc(1, sizeof(struct ecat_ocb));
}

static void ocb_free(struct ecat_ocb *ocb)
{
   free(ocb->text);
   free(ocb);
}

static int io_open(resmgr_context_t *ctp, io_open_t *msg, struct ecat_attr *attr, void *extra)
{
   int status = iofunc_open_default(ctp, msg, &attr->attr, extra);
   if (status == EOK && attr->node == NODE_OUT)
   {
      pthread_mutex_lock(&g_d->lock);
      g_d->clients++;
      g_d->stats.clients = g_d->clients;
      pthread_mutex_unlock(&g_d->lock);
   }
   return status;
}

static int io_close_ocb(resmgr_context_t *ctp, void *reserved, struct ecat_ocb *ocb)
{
   int node = ocb->ocb.attr->node;
   size_t released;

   if (node == NODE_OUT)
   {
      pthread_mutex_lock(&g_d->lock);
      released = eq_outputs_release(&g_d->out, ocb);
      if (g_d->clients)
         g_d->clients--;
      g_d->stats.clients = g_d->clients;
      pthread_mutex_unlock(&g_d->lock);
      if (released)
      {
         fprintf(stderr, "ecatd: client closed /dev/ecat/out: %zu output bytes zeroed and released\n",
                 released);
      }
   }
   else if (node == NODE_IN)
   {
      iofunc_notify_remove(ctp, g_notify);
   }
   return iofunc_close_ocb_default(ctp, reserved, &ocb->ocb);
}

/* --------------------------------------------------------------- helpers */
static int get_offset(io_read_t *rmsg, io_write_t *wmsg, struct ecat_ocb *ocb, off64_t *off, int *explicit)
{
   uint32_t xtype = rmsg ? rmsg->i.xtype : wmsg->i.xtype;
   *explicit = 0;
   switch (xtype & _IO_XTYPE_MASK)
   {
   case _IO_XTYPE_NONE:
      *off = ocb->ocb.offset;
      return EOK;
   case _IO_XTYPE_OFFSET:
      *off = rmsg ? ((struct _xtype_offset *)(&rmsg->i + 1))->offset
                  : ((struct _xtype_offset *)(&wmsg->i + 1))->offset;
      *explicit = 1;
      return EOK;
   default:
      return ENOSYS;
   }
}

static void text_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
{
   va_list ap;
   int n;

   for (;;)
   {
      va_start(ap, fmt);
      n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
      va_end(ap);
      if (n < 0)
         return;
      if ((size_t)n < *cap - *len)
      {
         *len += (size_t)n;
         return;
      }
      {
         size_t ncap = (*cap + (size_t)n) * 2;
         char *nb = realloc(*buf, ncap);
         if (nb == NULL)
            return; /* keep the text built so far */
         *buf = nb;
         *cap = ncap;
      }
   }
}

static void build_status(struct ecat_ocb *ocb)
{
   ecatd_t *d = g_d;
   size_t len = 0, cap = 2048;
   char *b = malloc(cap);
   ecat_stats_t st;
   eq_stat_t wake, exec;
   ecatd_slave_state_t sl[ECATD_MAX_SLAVES + 1];
   uint16_t gs;
   int i;

   if (b == NULL)
      return;
   pthread_mutex_lock(&d->lock);
   st = d->stats;
   wake = d->wake;
   exec = d->exec;
   gs = d->group_state;
   memcpy(sl, d->slaves, sizeof(sl));
   pthread_mutex_unlock(&d->lock);

   text_append(&b, &len, &cap, "EtherQ ecatd %s (SOEM %s) on QNX\n", ECATD_VERSION, "v2.0.1");
   text_append(&b, &len, &cap, "interface %s  cycle %lld us  priority %d  bus state %s\n", d->ifname,
               (long long)(d->period_ns / 1000), d->cycle_prio, master_state_name(gs));
   text_append(&b, &len, &cap, "slaves %d  outputs %u B  inputs %u B  expected WKC %d  clients %u\n",
               d->ctx.slavecount, d->obytes, d->ibytes, d->expected_wkc, st.clients);
   text_append(&b, &len, &cap,
               "cycles %llu  wkc errors %llu  missed periods %llu  overruns %llu  recoveries %llu\n",
               (unsigned long long)st.cycles, (unsigned long long)st.wkc_errors,
               (unsigned long long)st.missed_periods, (unsigned long long)st.overruns,
               (unsigned long long)st.recoveries);
   if (wake.n)
   {
      text_append(&b, &len, &cap,
                  "wake latency us  min %.1f  mean %.1f  max %.1f   (measured on this target)\n",
                  wake.min / 1e3, eq_stat_mean(&wake) / 1e3, wake.max / 1e3);
      text_append(&b, &len, &cap, "cycle exec us    min %.1f  mean %.1f  max %.1f\n", exec.min / 1e3,
                  eq_stat_mean(&exec) / 1e3, exec.max / 1e3);
   }
   text_append(&b, &len, &cap, "\n  # state     AL code  address vendor     product    out  in   name\n");
   for (i = 1; i <= d->ctx.slavecount; i++)
   {
      ec_slavet *s = &d->ctx.slavelist[i];
      text_append(&b, &len, &cap, "%3d %-8s%s 0x%04x   0x%04x  0x%08x 0x%08x %4u %4u  %s\n", i,
                  master_state_name(sl[i].state), (sl[i].state & EC_STATE_ERROR) ? "!" : " ",
                  sl[i].al_status_code, s->configadr, s->eep_man, s->eep_id, (unsigned)s->Obytes,
                  (unsigned)s->Ibytes, s->name);
   }
   free(ocb->text);
   ocb->text = b;
   ocb->textlen = len;
}

/* ------------------------------------------------------------- handlers */
static int io_read(resmgr_context_t *ctp, io_read_t *msg, struct ecat_ocb *ocb)
{
   ecatd_t *d = g_d;
   uint8_t buf[ECATD_IOMAP_SIZE];
   size_t nbytes, avail, n = 0;
   off64_t off;
   int explicit, status, node = ocb->ocb.attr->node;
   const uint8_t *src = NULL;

   if ((status = iofunc_read_verify(ctp, msg, &ocb->ocb, NULL)) != EOK)
      return status;
   if ((status = get_offset(msg, NULL, ocb, &off, &explicit)) != EOK)
      return status;
   nbytes = _IO_READ_GET_NBYTES(msg);
   if (off < 0)
      return EINVAL;

   if (node == NODE_STATUS)
   {
      if (off == 0 || ocb->text == NULL)
         build_status(ocb);
      avail = ((size_t)off < ocb->textlen) ? ocb->textlen - (size_t)off : 0;
      n = nbytes < avail ? nbytes : avail;
      src = (const uint8_t *)ocb->text + off;
   }
   else
   {
      pthread_mutex_lock(&d->lock);
      if (node == NODE_IN)
      {
         avail = ((size_t)off < d->ibytes) ? d->ibytes - (size_t)off : 0;
         n = nbytes < avail ? nbytes : avail;
         memcpy(buf, d->in_snap + off, n);
         ocb->seen_cycle = d->cycle;
      }
      else
      {
         avail = ((size_t)off < d->obytes) ? d->obytes - (size_t)off : 0;
         n = nbytes < avail ? nbytes : avail;
         memcpy(buf, d->out.staged + off, n);
      }
      pthread_mutex_unlock(&d->lock);
      src = buf;
   }
   if (!explicit)
      ocb->ocb.offset += (off64_t)n;
   MsgReply(ctp->rcvid, (long)n, src, n);
   return _RESMGR_NOREPLY;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, struct ecat_ocb *ocb)
{
   ecatd_t *d = g_d;
   uint8_t buf[ECATD_IOMAP_SIZE];
   size_t nbytes;
   off64_t off;
   int explicit, status, rc;

   if ((status = iofunc_write_verify(ctp, msg, &ocb->ocb, NULL)) != EOK)
      return status;
   if (ocb->ocb.attr->node != NODE_OUT)
      return EROFS;
   if ((status = get_offset(NULL, msg, ocb, &off, &explicit)) != EOK)
      return status;
   nbytes = _IO_WRITE_GET_NBYTES(msg);
   if (off < 0 || nbytes > sizeof(buf))
      return EINVAL;
   if (resmgr_msgget(ctp, buf, nbytes, sizeof(msg->i) + (explicit ? sizeof(struct _xtype_offset) : 0)) !=
       (ssize_t)nbytes)
      return EFAULT;

   pthread_mutex_lock(&d->lock);
   rc = eq_outputs_write(&d->out, ocb, (size_t)off, buf, nbytes);
   pthread_mutex_unlock(&d->lock);
   if (rc == -EBUSY)
      return EBUSY;
   if (rc == -ERANGE)
      return ENOSPC;
   if (rc != 0)
      return EINVAL;
   if (!explicit)
      ocb->ocb.offset += (off64_t)nbytes;
   _IO_SET_WRITE_NBYTES(ctp, nbytes);
   return _RESMGR_NPARTS(0);
}

static int io_notify(resmgr_context_t *ctp, io_notify_t *msg, struct ecat_ocb *ocb)
{
   int trig = 0;

   if (ocb->ocb.attr->node != NODE_IN)
      return ENOSYS;
   if (ocb->seen_cycle != g_d->cycle)
      trig |= _NOTIFY_COND_INPUT;
   return iofunc_notify(ctp, msg, g_notify, trig, NULL, NULL);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, struct ecat_ocb *ocb)
{
   ecatd_t *d = g_d;
   void *data = _DEVCTL_DATA(msg->i);
   size_t nbytes = 0;
   int status, i;

   status = iofunc_devctl_default(ctp, msg, &ocb->ocb);
   if (status != _RESMGR_DEFAULT)
      return status;

   /* Commands that change the bus or the statistics need the same right as
    * commanding outputs: /dev/ecat/out opened for writing. */
   if ((msg->i.dcmd == DCMD_ECAT_SET_STATE || msg->i.dcmd == DCMD_ECAT_RESET_STATS) &&
       (ocb->ocb.attr->node != NODE_OUT || !(ocb->ocb.ioflag & _IO_FLAG_WR)))
      return EPERM;

   switch (msg->i.dcmd)
   {
   case DCMD_ECAT_GET_INFO:
   {
      ecat_info_t *info = data;
      if (msg->i.nbytes < sizeof(*info))
         return EINVAL;
      memset(info, 0, sizeof(*info));
      info->api_version = ECAT_API_VERSION;
      info->slave_count = (uint32_t)d->ctx.slavecount;
      info->expected_wkc = (uint32_t)d->expected_wkc;
      info->obytes = d->obytes;
      info->ibytes = d->ibytes;
      info->period_ns = (uint32_t)d->period_ns;
      info->cycle_priority = (uint32_t)d->cycle_prio;
      snprintf(info->ifname, sizeof(info->ifname), "%s", d->ifname);
      pthread_mutex_lock(&d->lock);
      info->group_state = d->group_state;
      pthread_mutex_unlock(&d->lock);
      nbytes = sizeof(*info);
      break;
   }
   case DCMD_ECAT_GET_SLAVE:
   {
      ecat_slave_t *out = data;
      ec_slavet *s;
      ec_groupt *grp = &d->ctx.grouplist[0];
      if (msg->i.nbytes < sizeof(*out))
         return EINVAL;
      i = (int)out->index;
      if (i < 1 || i > d->ctx.slavecount)
         return ENOENT;
      s = &d->ctx.slavelist[i];
      memset(out, 0, sizeof(*out));
      out->index = (uint32_t)i;
      pthread_mutex_lock(&d->lock);
      out->state = d->slaves[i].state;
      out->al_status_code = d->slaves[i].al_status_code;
      pthread_mutex_unlock(&d->lock);
      out->vendor_id = s->eep_man;
      out->product_code = s->eep_id;
      out->revision = s->eep_rev;
      out->configured_address = s->configadr;
      out->obits = s->Obits;
      out->ibits = s->Ibits;
      out->ooffset = (s->outputs && grp->outputs) ? (uint32_t)(s->outputs - grp->outputs) : 0;
      out->ioffset = (s->inputs && grp->inputs) ? (uint32_t)(s->inputs - grp->inputs) : 0;
      strncpy(out->name, s->name, sizeof(out->name) - 1);
      nbytes = sizeof(*out);
      break;
   }
   case DCMD_ECAT_GET_STATS:
   {
      ecat_stats_t *st = data;
      if (msg->i.nbytes < sizeof(*st))
         return EINVAL;
      pthread_mutex_lock(&d->lock);
      *st = d->stats;
      st->wake_min_ns = d->wake.min;
      st->wake_mean_ns = eq_stat_mean(&d->wake);
      st->wake_max_ns = d->wake.max;
      st->exec_min_ns = d->exec.min;
      st->exec_mean_ns = eq_stat_mean(&d->exec);
      st->exec_max_ns = d->exec.max;
      pthread_mutex_unlock(&d->lock);
      nbytes = sizeof(*st);
      break;
   }
   case DCMD_ECAT_RESET_STATS:
      pthread_mutex_lock(&d->lock);
      d->stats.cycles = d->stats.wkc_errors = d->stats.missed_periods = 0;
      d->stats.overruns = d->stats.recoveries = 0;
      eq_stat_reset(&d->wake);
      eq_stat_reset(&d->exec);
      pthread_mutex_unlock(&d->lock);
      break;
   case DCMD_ECAT_SET_STATE:
   {
      uint32_t req;
      if (msg->i.nbytes < sizeof(req))
         return EINVAL;
      memcpy(&req, data, sizeof(req));
      if (req != ECAT_STATE_INIT && req != ECAT_STATE_PREOP && req != ECAT_STATE_SAFEOP &&
          req != ECAT_STATE_OP)
         return EINVAL;
      master_request_state(d, (uint16_t)req);
      break;
   }
   default:
      return ENOSYS;
   }
   memset(&msg->o, 0, sizeof(msg->o));
   msg->o.nbytes = (uint32_t)nbytes;
   return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
}

/* Pulse from the cycle thread: new inputs are available. */
static int on_cycle_pulse(message_context_t *ctp, int code, unsigned flags, void *handle)
{
   (void)ctp;
   (void)code;
   (void)flags;
   (void)handle;
   /* io_notify/io_close_ocb modify the same list under the attribute lock */
   iofunc_attr_lock(&g_attr[NODE_IN].attr);
   iofunc_notify_trigger(g_notify, 1, IOFUNC_NOTIFY_INPUT);
   iofunc_attr_unlock(&g_attr[NODE_IN].attr);
   return 0;
}

/* ---------------------------------------------------------------- setup */
static int attach_node(const char *path, int node, mode_t mode)
{
   struct ecat_attr *a = &g_attr[node];
   resmgr_attr_t rattr;

   iofunc_attr_init(&a->attr, S_IFCHR | mode, NULL, NULL);
   a->attr.mount = &g_mount;
   a->node = node;
   memset(&rattr, 0, sizeof(rattr));
   rattr.nparts_max = 1;
   rattr.msg_max_size = 2048 + ECATD_IOMAP_SIZE;
   if (resmgr_attach(g_dpp, &rattr, path, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs, a) == -1)
   {
      fprintf(stderr, "ecatd: resmgr_attach(%s): %s\n", path, strerror(errno));
      return -1;
   }
   return 0;
}

int resmgr_setup(ecatd_t *d)
{
   g_d = d;
   d->resmgr_coid = -1;
   g_dpp = dispatch_create_channel(-1, 0);
   if (g_dpp == NULL)
   {
      perror("ecatd: dispatch_create_channel");
      return -1;
   }
   iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs, _RESMGR_IO_NFUNCS, &g_io_funcs);
   g_connect_funcs.open = io_open;
   g_io_funcs.read = io_read;
   g_io_funcs.write = io_write;
   g_io_funcs.devctl = io_devctl;
   g_io_funcs.notify = io_notify;
   g_io_funcs.close_ocb = io_close_ocb;

   memset(&g_ocb_funcs, 0, sizeof(g_ocb_funcs));
   g_ocb_funcs.nfuncs = _IOFUNC_NFUNCS;
   g_ocb_funcs.ocb_calloc = ocb_calloc;
   g_ocb_funcs.ocb_free = ocb_free;
   memset(&g_mount, 0, sizeof(g_mount));
   g_mount.funcs = &g_ocb_funcs;
   IOFUNC_NOTIFY_INIT(g_notify);

   if (attach_node(ECAT_PATH_IN, NODE_IN, 0444) || attach_node(ECAT_PATH_OUT, NODE_OUT, 0660) ||
       attach_node(ECAT_PATH_STATUS, NODE_STATUS, 0444))
      return -1;

   d->cycle_pulse_code = pulse_attach(g_dpp, MSG_FLAG_ALLOC_PULSE, 0, on_cycle_pulse, NULL);
   if (d->cycle_pulse_code == -1)
   {
      perror("ecatd: pulse_attach");
      return -1;
   }
   d->resmgr_coid = message_connect(g_dpp, MSG_FLAG_SIDE_CHANNEL);
   if (d->resmgr_coid == -1)
   {
      perror("ecatd: message_connect");
      return -1;
   }
   return 0;
}

int resmgr_run(ecatd_t *d)
{
   thread_pool_attr_t pool_attr;
   thread_pool_t *pool;

   (void)d;
   memset(&pool_attr, 0, sizeof(pool_attr));
   pool_attr.handle = g_dpp;
   pool_attr.context_alloc = dispatch_context_alloc;
   pool_attr.block_func = dispatch_block;
   pool_attr.unblock_func = dispatch_unblock;
   pool_attr.handler_func = dispatch_handler;
   pool_attr.context_free = dispatch_context_free;
   pool_attr.lo_water = 2;
   pool_attr.hi_water = 4;
   pool_attr.increment = 1;
   pool_attr.maximum = 8;
   pool = thread_pool_create(&pool_attr, POOL_FLAG_EXIT_SELF);
   if (pool == NULL)
   {
      perror("ecatd: thread_pool_create");
      return -1;
   }
   return thread_pool_start(pool); /* the calling thread joins the pool */
}
