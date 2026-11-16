/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ecatcore.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int eq_outputs_init(eq_outputs_t *o, size_t nbytes)
{
   memset(o, 0, sizeof(*o));
   o->nbytes = nbytes;
   if (nbytes == 0)
   {
      return 0;
   }
   o->staged = calloc(nbytes, 1);
   o->owner = calloc(nbytes, sizeof(*o->owner));
   if (o->staged == NULL || o->owner == NULL)
   {
      eq_outputs_free(o);
      return -ENOMEM;
   }
   return 0;
}

void eq_outputs_free(eq_outputs_t *o)
{
   free(o->staged);
   free((void *)o->owner);
   memset(o, 0, sizeof(*o));
}

int eq_outputs_write(eq_outputs_t *o, const void *who, size_t off, const uint8_t *src, size_t len)
{
   size_t i;

   if (who == NULL)
   {
      return -EINVAL;
   }
   if (off > o->nbytes || len > o->nbytes - off)
   {
      return -ERANGE;
   }
   for (i = off; i < off + len; i++)
   {
      if (o->owner[i] != NULL && o->owner[i] != who)
      {
         return -EBUSY;
      }
   }
   for (i = off; i < off + len; i++)
   {
      o->owner[i] = who;
   }
   memcpy(o->staged + off, src, len);
   return 0;
}

size_t eq_outputs_release(eq_outputs_t *o, const void *who)
{
   size_t i, n = 0;

   for (i = 0; i < o->nbytes; i++)
   {
      if (o->owner[i] == who && who != NULL)
      {
         o->owner[i] = NULL;
         o->staged[i] = 0;
         n++;
      }
   }
   return n;
}

size_t eq_outputs_owned(const eq_outputs_t *o, const void *who)
{
   size_t i, n = 0;

   for (i = 0; i < o->nbytes; i++)
   {
      if (who == NULL ? o->owner[i] != NULL : o->owner[i] == who)
      {
         n++;
      }
   }
   return n;
}

void eq_stat_reset(eq_stat_t *s)
{
   memset(s, 0, sizeof(*s));
}

void eq_stat_add(eq_stat_t *s, int64_t v)
{
   if (s->n == 0 || v < s->min)
   {
      s->min = v;
   }
   if (s->n == 0 || v > s->max)
   {
      s->max = v;
   }
   s->last = v;
   s->sum += (double)v;
   s->n++;
}

int64_t eq_stat_mean(const eq_stat_t *s)
{
   return s->n ? (int64_t)(s->sum / (double)s->n) : 0;
}

int64_t eq_ts_to_ns(const struct timespec *ts)
{
   return (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

void eq_ns_to_ts(int64_t ns, struct timespec *ts)
{
   ts->tv_sec = (time_t)(ns / 1000000000LL);
   ts->tv_nsec = (long)(ns % 1000000000LL);
   if (ts->tv_nsec < 0)
   {
      ts->tv_nsec += 1000000000L;
      ts->tv_sec -= 1;
   }
}

int64_t eq_cycle_deadline(int64_t start_ns, int64_t period_ns, uint64_t k)
{
   return start_ns + (int64_t)k * period_ns;
}

uint64_t eq_missed_periods(int64_t deadline_ns, int64_t now_ns, int64_t period_ns)
{
   if (period_ns <= 0 || now_ns <= deadline_ns)
   {
      return 0;
   }
   return (uint64_t)((now_ns - deadline_ns) / period_ns);
}
