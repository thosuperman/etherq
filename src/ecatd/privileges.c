/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Least privilege for ecatd (QNX procmgr abilities).
 *
 * ecatd needs root only to open /dev/bpf, attach /dev/ecat/... and create its
 * real-time threads. After that it keeps only the abilities the running cycle
 * still uses, locks the set so it can never be widened again, and switches to an
 * unprivileged user. A compromised client of /dev/ecat cannot escalate through
 * ecatd.
 */
#include "ecatd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <sys/procmgr.h>

int privileges_drop(uid_t uid, gid_t gid)
{
   int rc;

   /* drop root's supplementary groups while we still may */
   if (setgroups(1, &gid) == -1)
   {
      fprintf(stderr, "ecatd: setgroups: %s\n", strerror(errno));
      return -1;
   }
   /* - non-root: keep (and lock) priority control so real-time threads can be
    *   (re)prioritised above the unprivileged limit;
    * - root: allow setuid/setgid just for the switch below (the root domain is
    *   irrelevant once we are non-root);
    * - everything else: denied and locked in both domains. */
   rc = procmgr_ability(0,
                       PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_PRIORITY,
                       PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_SETUID,
                       PROCMGR_ADN_ROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_SETGID,
                       PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK |
                           PROCMGR_AID_EOL);
   if (rc != EOK) /* returns an error number, not -1/errno */
   {
      fprintf(stderr, "ecatd: procmgr_ability: %s\n", strerror(rc));
      return -1;
   }
   if (setgid(gid) == -1 || setuid(uid) == -1)
   {
      fprintf(stderr, "ecatd: cannot switch to uid %d gid %d: %s\n", (int)uid, (int)gid,
              strerror(errno));
      return -1;
   }
   return 0;
}
