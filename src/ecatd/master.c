/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ecatd master: SOEM bring-up, the periodic process-data cycle, and the
 * supervisor that recovers slaves.
 *
 * The cycle thread is driven by a QNX timer that delivers a pulse to a private
 * channel (timer_create + SIGEV_PULSE_INIT, MsgReceivePulse). It runs SCHED_FIFO
 * at a fixed priority. Every cycle it:
 *   1. copies the staged outputs (written by clients via /dev/ecat/out) into the IOmap,
 *   2. exchanges process data (LRW) with SOEM,
 *   3. snapshots the inputs for /dev/ecat/in,
 *   4. sends a pulse to the resource manager so poll()/select() clients wake up.
 */
#include "ecatd.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 1)
#define SUPERVISOR_PERIOD_NS (100LL * 1000 * 1000)
#define ECATD_RECONFIG_TIMEOUT_US 500000
#define ECATD_LOST_PASSES 3

int64_t ecatd_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return eq_ts_to_ns(&ts);
}

const char *master_state_name(uint16_t state)
{
   switch (state & 0x0F)
   {
   case EC_STATE_INIT:
      return "INIT";
   case EC_STATE_PRE_OP:
      return "PRE-OP";
   case EC_STATE_BOOT:
      return "BOOT";
   case EC_STATE_SAFE_OP:
      return "SAFE-OP";
   case EC_STATE_OPERATIONAL:
      return "OP";
   default:
      return "NONE";
   }
}

static void refresh_slave_states(ecatd_t *d)
{
   int i;
   uint16_t lowest = 0xFF;

   ecx_readstate(&d->ctx);
   pthread_mutex_lock(&d->lock);
   for (i = 1; i <= d->ctx.slavecount && i <= ECATD_MAX_SLAVES; i++)
   {
      d->slaves[i].state = d->ctx.slavelist[i].state;
      d->slaves[i].al_status_code = d->ctx.slavelist[i].ALstatuscode;
      if ((d->slaves[i].state & 0x0F) < lowest)
      {
         lowest = d->slaves[i].state & 0x0F;
      }
   }
   d->group_state = (lowest == 0xFF) ? 0 : lowest;
   pthread_mutex_unlock(&d->lock);
}

int master_open(ecatd_t *d)
{
   ec_groupt *grp;
   int wkc, iosize;

   pthread_mutexattr_t ma;
   pthread_mutexattr_init(&ma);
   pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
   pthread_mutex_init(&d->lock, &ma);
   pthread_mutexattr_destroy(&ma);

   if (!ecx_init(&d->ctx, d->ifname))
   {
      fprintf(stderr, "ecatd: cannot open %s on BPF (does the interface exist? running as root?)\n",
              d->ifname);
      return -1;
   }
   wkc = ecx_config_init(&d->ctx);
   if (wkc <= 0 || d->ctx.slavecount <= 0)
   {
      fprintf(stderr, "ecatd: no EtherCAT slaves found on %s\n", d->ifname);
      return -1;
   }
   if (d->ctx.slavecount > ECATD_MAX_SLAVES)
   {
      fprintf(stderr, "ecatd: %d slaves found, at most %d supported\n", d->ctx.slavecount,
              ECATD_MAX_SLAVES);
      return -1;
   }
   /* SOEM never bounds-checks the IOmap: check the size it needs before the first
    * process-data exchange (mapping itself only computes pointers). */
   iosize = ecx_config_map_group(&d->ctx, d->iomap, 0);
   if (iosize <= 0 || iosize > ECATD_IOMAP_SIZE)
   {
      fprintf(stderr, "ecatd: process image needs %d bytes, ecatd supports at most %d\n", iosize,
              ECATD_IOMAP_SIZE);
      return -1;
   }
   grp = &d->ctx.grouplist[0];
   d->obytes = grp->Obytes;
   d->ibytes = grp->Ibytes;
   d->expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;
   ecx_configdc(&d->ctx);

   if (eq_outputs_init(&d->out, d->obytes) != 0 ||
       (d->in_snap = calloc(d->ibytes ? d->ibytes : 1, 1)) == NULL)
   {
      fprintf(stderr, "ecatd: out of memory\n");
      return -1;
   }
   ecx_statecheck(&d->ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   refresh_slave_states(d);
   printf("ecatd: %d slaves on %s, %u output bytes, %u input bytes, expected WKC %d\n",
          d->ctx.slavecount, d->ifname, d->obytes, d->ibytes, d->expected_wkc);
   return 0;
}

static void *cycle_thread(void *arg)
{
   ecatd_t *d = arg;
   ec_groupt *grp = &d->ctx.grouplist[0];
   struct sigevent ev;
   struct itimerspec its;
   struct _pulse pulse;
   timer_t tid;
   int chid, coid, wkc;
   int64_t start, now, deadline, exec;
   uint64_t k, last_k = 0;

   chid = ChannelCreate(_NTO_CHF_PRIVATE);
   if (chid == -1)
   {
      perror("ecatd: ChannelCreate");
      return NULL;
   }
   coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
   if (coid == -1)
   {
      perror("ecatd: ConnectAttach");
      ChannelDestroy(chid);
      return NULL;
   }
   /* the pulse carries the cycle priority, so the receiving thread runs at it */
   SIGEV_PULSE_INIT(&ev, coid, d->cycle_prio, PULSE_CODE_TICK, 0);
   if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1)
   {
      perror("ecatd: timer_create");
      ConnectDetach(coid);
      ChannelDestroy(chid);
      return NULL;
   }
   start = ecatd_now_ns() + d->period_ns;
   eq_ns_to_ts(start, &its.it_value);
   eq_ns_to_ts(d->period_ns, &its.it_interval);
   if (timer_settime(tid, TIMER_ABSTIME, &its, NULL) == -1)
   {
      perror("ecatd: timer_settime");
      timer_delete(tid);
      ConnectDetach(coid);
      ChannelDestroy(chid);
      return NULL;
   }

   while (d->running)
   {
      if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) == -1)
      {
         if (errno == EINTR)
            continue;
         perror("ecatd: MsgReceivePulse");
         break;
      }
      if (pulse.code != PULSE_CODE_TICK)
         continue;
      now = ecatd_now_ns();
      /* which period are we in? Derived from time, so lost/merged pulses are counted. */
      k = (uint64_t)((now - start) / d->period_ns);
      deadline = eq_cycle_deadline(start, d->period_ns, k);

      pthread_mutex_lock(&d->lock);
      if (d->obytes)
         memcpy(grp->outputs, d->out.staged, d->obytes);
      pthread_mutex_unlock(&d->lock);

      ecx_send_processdata(&d->ctx);
      wkc = ecx_receive_processdata(&d->ctx, EC_TIMEOUTRET);

      exec = ecatd_now_ns() - now;
      pthread_mutex_lock(&d->lock);
      if (d->ibytes)
         memcpy(d->in_snap, grp->inputs, d->ibytes);
      d->cycle++;
      d->stats.cycles++;
      d->stats.last_wkc = (uint32_t)(wkc < 0 ? 0 : wkc);
      /* below OP a short WKC is expected (outputs not yet valid), so only count it in OP */
      if (wkc < d->expected_wkc && d->group_state == EC_STATE_OPERATIONAL)
      {
         d->stats.wkc_errors++;
         d->need_check = 1;
      }
      if (last_k && k > last_k + 1)
         d->stats.missed_periods += k - last_k - 1;
      eq_stat_add(&d->wake, now - deadline);
      eq_stat_add(&d->exec, exec);
      if (exec > d->period_ns)
         d->stats.overruns++;
      pthread_mutex_unlock(&d->lock);
      last_k = k;

      if (d->resmgr_coid != -1)
      {
         /* non-blocking: wake clients waiting in poll()/select() on /dev/ecat/in */
         (void)MsgSendPulse(d->resmgr_coid, d->supervisor_prio, d->cycle_pulse_code, 0);
      }
   }
   timer_delete(tid);
   ConnectDetach(coid);
   ChannelDestroy(chid);
   return NULL;
}

static void write_slave_state(ecatd_t *d, int i, uint16 state)
{
   d->ctx.slavelist[i].state = state;
   ecx_writestate(&d->ctx, (uint16)i);
}

static void count_recovery(ecatd_t *d)
{
   pthread_mutex_lock(&d->lock);
   d->stats.recoveries++;
   pthread_mutex_unlock(&d->lock);
}

/* Drive every slave towards target (snapshot), one valid AL transition at a time.
 * An error indication is always acknowledged (EC_STATE_ACK) together with the
 * request, otherwise a slave ignores further requests. */
static void supervise(ecatd_t *d, uint16 target)
{
   int i;
   ec_slavet *s;
   uint16 st, err;

   for (i = 1; i <= d->ctx.slavecount; i++)
   {
      s = &d->ctx.slavelist[i];
      st = s->state & 0x0F;
      err = s->state & EC_STATE_ERROR;
      if (s->state == EC_STATE_NONE)
      {
         /* No answer. A single missed state poll (late frame) is not a lost slave:
          * only act after ECATD_LOST_PASSES consecutive passes without a reply. */
         if (d->no_answer[i] < 255)
            d->no_answer[i]++;
         if (d->no_answer[i] < ECATD_LOST_PASSES)
            continue;
         if (!s->islost)
         {
            ecx_statecheck(&d->ctx, (uint16)i, target, EC_TIMEOUTRET);
            if (s->state == EC_STATE_NONE)
            {
               s->islost = TRUE;
               fprintf(stderr, "ecatd: slave %d lost\n", i);
            }
         }
         else if (ecx_recover_slave(&d->ctx, (uint16)i, EC_TIMEOUTRET) > 0)
         {
            s->islost = FALSE;
            count_recovery(d);
            fprintf(stderr, "ecatd: slave %d recovered\n", i);
         }
         continue;
      }
      d->no_answer[i] = 0;
      s->islost = FALSE;
      if (st == target && !err)
         continue;
      if (st == EC_STATE_SAFE_OP && err && target == EC_STATE_OPERATIONAL)
      {
         /* typical after a watchdog trip: acknowledge first, OP on the next pass */
         fprintf(stderr, "ecatd: slave %d in SAFE-OP+ERROR (AL 0x%04x), acknowledging\n", i,
                 s->ALstatuscode);
         write_slave_state(d, i, EC_STATE_SAFE_OP + EC_STATE_ACK);
      }
      else if (st == EC_STATE_SAFE_OP && target == EC_STATE_OPERATIONAL)
      {
         write_slave_state(d, i, EC_STATE_OPERATIONAL);
         count_recovery(d);
      }
      else if (st >= target)
      {
         /* going down (or acknowledging at the target state) is always a valid request */
         write_slave_state(d, i, target + (err ? EC_STATE_ACK : 0));
      }
      else if (target >= EC_STATE_SAFE_OP && st < EC_STATE_SAFE_OP)
      {
         /* INIT/PRE-OP -> SAFE-OP: acknowledge, then let SOEM reprogram SMs/FMMUs */
         if (err)
            write_slave_state(d, i, EC_STATE_INIT + EC_STATE_ACK);
         if (ecx_reconfig_slave(&d->ctx, (uint16)i, ECATD_RECONFIG_TIMEOUT_US) == EC_STATE_SAFE_OP)
         {
            count_recovery(d);
            fprintf(stderr, "ecatd: slave %d reconfigured to SAFE-OP\n", i);
         }
      }
      else if (target == EC_STATE_PRE_OP && st == EC_STATE_INIT)
      {
         write_slave_state(d, i, EC_STATE_PRE_OP + (err ? EC_STATE_ACK : 0));
      }
   }
}

static void *supervisor_thread(void *arg)
{
   ecatd_t *d = arg;
   struct timespec next;
   int64_t t = ecatd_now_ns(), now;
   uint16 target;
   int requested;

   while (d->running)
   {
      pthread_mutex_lock(&d->lock);
      target = d->target_state;
      requested = d->state_request;
      d->state_request = 0;
      pthread_mutex_unlock(&d->lock);

      refresh_slave_states(d);
      if (requested || d->need_check || d->group_state != target)
      {
         d->need_check = 0;
         supervise(d, target);
      }
      t += SUPERVISOR_PERIOD_NS;
      now = ecatd_now_ns();
      if (t < now)
         t = now; /* a slow recovery pass must not cause a burst of catch-up passes */
      eq_ns_to_ts(t, &next);
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
         ;
   }
   return NULL;
}

static int start_fifo_thread(pthread_t *tid, int prio, void *(*fn)(void *), void *arg)
{
   pthread_attr_t attr;
   struct sched_param sp;
   int rc;

   pthread_attr_init(&attr);
   pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
   pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
   memset(&sp, 0, sizeof(sp));
   sp.sched_priority = prio;
   pthread_attr_setschedparam(&attr, &sp);
   rc = pthread_create(tid, &attr, fn, arg);
   pthread_attr_destroy(&attr);
   return rc;
}

int master_start(ecatd_t *d)
{
   int rc;

   d->running = 1;
   d->target_state = EC_STATE_OPERATIONAL;
   d->state_request = 1;
   rc = start_fifo_thread(&d->cycle_tid, d->cycle_prio, cycle_thread, d);
   if (rc != 0)
   {
      fprintf(stderr, "ecatd: cannot start cycle thread at priority %d: %s\n", d->cycle_prio,
              strerror(rc));
      return -1;
   }
   rc = start_fifo_thread(&d->supervisor_tid, d->supervisor_prio, supervisor_thread, d);
   if (rc != 0)
   {
      fprintf(stderr, "ecatd: cannot start supervisor thread: %s\n", strerror(rc));
      return -1;
   }
   return 0;
}

void master_request_state(ecatd_t *d, uint16_t state)
{
   pthread_mutex_lock(&d->lock);
   d->target_state = state;
   d->state_request = 1;
   pthread_mutex_unlock(&d->lock);
}

void master_shutdown(ecatd_t *d)
{
   static int done;

   /* may be reached from the signal thread and from main's error paths */
   if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
      return;
   d->running = 0;
   if (d->supervisor_tid)
      pthread_join(d->supervisor_tid, NULL);
   if (d->cycle_tid)
      pthread_join(d->cycle_tid, NULL);
   d->ctx.slavelist[0].state = EC_STATE_INIT;
   ecx_writestate(&d->ctx, 0);
   ecx_close(&d->ctx);
}
