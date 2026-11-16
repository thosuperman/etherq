/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 *
 * QNX OS 8.0 port: Copyright (C) 2026 EtherQ contributors (GPL-3.0-or-later).
 * Derived from the SOEM Linux OSAL.
 */

#include <osal.h>
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

void osal_get_monotonic_time(ec_timet *ts)
{
   /* CLOCK_MONOTONIC is never stepped by clock adjustments on QNX. */
   clock_gettime(CLOCK_MONOTONIC, ts);
}

ec_timet osal_current_time(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return ts;
}

void osal_time_diff(ec_timet *start, ec_timet *end, ec_timet *diff)
{
   osal_timespecsub(end, start, diff);
}

void osal_timer_start(osal_timert *self, uint32 timeout_usec)
{
   struct timespec start_time;
   struct timespec timeout;

   osal_get_monotonic_time(&start_time);
   osal_timespec_from_usec(timeout_usec, &timeout);
   osal_timespecadd(&start_time, &timeout, &self->stop_time);
}

boolean osal_timer_is_expired(osal_timert *self)
{
   struct timespec current_time;

   osal_get_monotonic_time(&current_time);
   return osal_timespeccmp(&current_time, &self->stop_time, <) ? FALSE : TRUE;
}

int osal_usleep(uint32 usec)
{
   struct timespec ts;
   int rc;

   osal_timespec_from_usec(usec, &ts);
   /* clock_nanosleep returns the error number directly; retry on EINTR */
   do
   {
      rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts);
   } while (rc == EINTR);
   return rc == 0 ? 0 : -1;
}

int osal_monotonic_sleep(ec_timet *ts)
{
   int rc;
   do
   {
      rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ts, NULL);
   } while (rc == EINTR);
   return rc == 0 ? 0 : -1;
}

void *osal_malloc(size_t size)
{
   return malloc(size);
}

void osal_free(void *ptr)
{
   free(ptr);
}

int osal_thread_create(void *thandle, int stacksize, void *func, void *param)
{
   pthread_attr_t attr;
   int rc;

   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, (size_t)stacksize);
   rc = pthread_create((pthread_t *)thandle, &attr, (void *(*)(void *))func, param);
   pthread_attr_destroy(&attr);
   return rc == 0 ? 1 : 0;
}

int osal_thread_create_rt(void *thandle, int stacksize, void *func, void *param)
{
   pthread_attr_t attr;
   struct sched_param sp;
   int rc;

   /* Create the thread directly with the real-time policy instead of changing it
    * afterwards, so it never runs at the creator's priority. */
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, (size_t)stacksize);
   pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
   pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
   memset(&sp, 0, sizeof(sp));
   sp.sched_priority = OSAL_QNX_RT_PRIORITY;
   pthread_attr_setschedparam(&attr, &sp);
   rc = pthread_create((pthread_t *)thandle, &attr, (void *(*)(void *))func, param);
   pthread_attr_destroy(&attr);
   return rc == 0 ? 1 : 0;
}

void *osal_mutex_create(void)
{
   pthread_mutexattr_t mutexattr;
   osal_mutext *mutex;

   mutex = (osal_mutext *)osal_malloc(sizeof(osal_mutext));
   if (mutex)
   {
      pthread_mutexattr_init(&mutexattr);
      /* Priority inheritance is the QNX default; set it explicitly for clarity. */
      pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
      if (pthread_mutex_init(mutex, &mutexattr) != 0)
      {
         osal_free(mutex);
         mutex = NULL;
      }
      pthread_mutexattr_destroy(&mutexattr);
   }
   return (void *)mutex;
}

void osal_mutex_destroy(void *mutex)
{
   pthread_mutex_destroy((osal_mutext *)mutex);
   osal_free(mutex);
}

void osal_mutex_lock(void *mutex)
{
   pthread_mutex_lock((osal_mutext *)mutex);
}

void osal_mutex_unlock(void *mutex)
{
   pthread_mutex_unlock((osal_mutext *)mutex);
}
