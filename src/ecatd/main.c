/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ecatd - EtherCAT master daemon for QNX OS 8.0.
 *
 *   ecatd -i vtnet1 [-c cycle_us] [-P cycle_prio] [-S supervisor_prio] [-U uid[:gid]]
 *
 * Brings the EtherCAT segment on the given interface to OPERATIONAL, keeps it
 * there, and serves the process image under /dev/ecat (see ecat_devctl.h).
 */
#include "ecatd.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ecatd_t g_ecatd;

static void usage(const char *prog)
{
   fprintf(stderr,
           "usage: %s -i ifname [-c cycle_us] [-P cycle_prio] [-S supervisor_prio] [-U uid[:gid]] [-v]\n"
           "  -i  network interface connected to the EtherCAT segment (e.g. vtnet1)\n"
           "  -c  cycle period in microseconds (default 1000)\n"
           "  -P  SCHED_FIFO priority of the cycle thread (default 50)\n"
           "  -S  SCHED_FIFO priority of the supervisor thread (default 20)\n"
           "  -U  drop root after start-up and run as uid[:gid] with locked abilities\n",
           prog);
}

/* Orderly shutdown on SIGINT/SIGTERM: slaves to INIT, BPF closed. */
static void *signal_thread(void *arg)
{
   sigset_t *set = arg;
   int sig = 0;

   sigwait(set, &sig);
   fprintf(stderr, "ecatd: signal %d, requesting INIT and exiting\n", sig);
   master_shutdown(&g_ecatd);
   exit(0);
   return NULL;
}

int main(int argc, char *argv[])
{
   ecatd_t *d = &g_ecatd;
   static sigset_t sigs;
   pthread_t sig_tid;
   int opt, drop = 0;
   long uid = -1, gid = -1;
   char *colon;

   memset(d, 0, sizeof(*d));
   d->period_ns = 1000 * 1000;
   d->cycle_prio = 50;
   d->supervisor_prio = 20;

   while ((opt = getopt(argc, argv, "i:c:P:S:U:vh")) != -1)
   {
      switch (opt)
      {
      case 'i':
         strncpy(d->ifname, optarg, sizeof(d->ifname) - 1);
         break;
      case 'c':
         d->period_ns = strtoll(optarg, NULL, 10) * 1000;
         break;
      case 'P':
         d->cycle_prio = atoi(optarg);
         break;
      case 'S':
         d->supervisor_prio = atoi(optarg);
         break;
      case 'U':
         drop = 1;
         uid = strtol(optarg, &colon, 10);
         gid = (*colon == ':') ? strtol(colon + 1, NULL, 10) : uid;
         break;
      case 'v':
         d->verbose = 1;
         break;
      default:
         usage(argv[0]);
         return opt == 'h' ? 0 : 2;
      }
   }
   if (d->ifname[0] == '\0' || d->period_ns < 100000 || d->cycle_prio <= d->supervisor_prio ||
       (drop && (uid < 0 || gid < 0)))
   {
      usage(argv[0]);
      return 2;
   }

   /* signals are handled by one dedicated thread; block them everywhere else */
   /* a closed stdout/stderr pipe must not kill the master with the bus in OP */
   signal(SIGPIPE, SIG_IGN);
   sigemptyset(&sigs);
   sigaddset(&sigs, SIGINT);
   sigaddset(&sigs, SIGTERM);
   sigaddset(&sigs, SIGHUP);
   sigaddset(&sigs, SIGQUIT);
   pthread_sigmask(SIG_BLOCK, &sigs, NULL);

   if (resmgr_setup(d) != 0)
      return 1;
   if (master_open(d) != 0)
      return 1;
   if (master_start(d) != 0)
   {
      master_shutdown(d);
      return 1;
   }
   pthread_create(&sig_tid, NULL, signal_thread, &sigs);

   if (drop)
   {
      if (privileges_drop((uid_t)uid, (gid_t)gid) != 0)
      {
         master_shutdown(d);
         return 1;
      }
      printf("ecatd: running as uid %ld gid %ld with locked abilities\n", uid, gid);
   }
   printf("ecatd: cycle %lld us at priority %d; serving %s %s %s\n", (long long)(d->period_ns / 1000),
          d->cycle_prio, ECAT_PATH_IN, ECAT_PATH_OUT, ECAT_PATH_STATUS);
   fflush(stdout);
   if (resmgr_run(d) != 0)
   {
      master_shutdown(d);
      return 1;
   }
   return 0;
}
