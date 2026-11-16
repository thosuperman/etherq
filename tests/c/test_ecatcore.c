/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host unit tests for ecatcore (no framework: exit code = number of failures). */
#include "ecatcore.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond)                                                     \
   do                                                                   \
   {                                                                    \
      if (!(cond))                                                      \
      {                                                                 \
         fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
         failures++;                                                    \
      }                                                                 \
   } while (0)

static void test_ownership(void)
{
   eq_outputs_t o;
   int a = 0, b = 0; /* addresses used as client identities */
   const uint8_t cw[2] = {0x0F, 0x00};
   const uint8_t pos[4] = {1, 2, 3, 4};

   CHECK(eq_outputs_init(&o, 8) == 0);
   CHECK(eq_outputs_write(&o, &a, 0, cw, 2) == 0);
   CHECK(eq_outputs_write(&o, &a, 2, pos, 4) == 0);
   CHECK(o.staged[0] == 0x0F && o.staged[5] == 4);
   /* another client cannot take bytes that are owned */
   CHECK(eq_outputs_write(&o, &b, 1, cw, 2) == -EBUSY);
   CHECK(o.staged[1] == 0x00 && o.staged[2] == 1); /* unchanged: all or nothing */
   /* but can use free bytes */
   CHECK(eq_outputs_write(&o, &b, 6, cw, 2) == 0);
   CHECK(eq_outputs_owned(&o, &a) == 6);
   CHECK(eq_outputs_owned(&o, &b) == 2);
   CHECK(eq_outputs_owned(&o, NULL) == 8);
   /* the owner can rewrite its own bytes */
   CHECK(eq_outputs_write(&o, &a, 0, pos, 2) == 0);
   /* range checks */
   CHECK(eq_outputs_write(&o, &a, 7, pos, 2) == -ERANGE);
   CHECK(eq_outputs_write(&o, &a, 9, pos, 0) == -ERANGE);
   CHECK(eq_outputs_write(&o, NULL, 0, pos, 1) == -EINVAL);
   /* client a disappears: its bytes are zeroed and released, b keeps its own */
   CHECK(eq_outputs_release(&o, &a) == 6);
   CHECK(o.staged[0] == 0 && o.staged[5] == 0);
   CHECK(o.staged[6] == 0x0F);
   CHECK(eq_outputs_owned(&o, &a) == 0);
   /* released bytes can now be claimed by b */
   CHECK(eq_outputs_write(&o, &b, 0, cw, 2) == 0);
   CHECK(eq_outputs_release(&o, NULL) == 0);
   eq_outputs_free(&o);
}

static void test_empty_image(void)
{
   eq_outputs_t o;
   int a = 0;
   CHECK(eq_outputs_init(&o, 0) == 0);
   CHECK(eq_outputs_write(&o, &a, 0, NULL, 0) == 0);
   CHECK(eq_outputs_write(&o, &a, 0, (const uint8_t *)"x", 1) == -ERANGE);
   eq_outputs_free(&o);
}

static void test_stats(void)
{
   eq_stat_t s;
   eq_stat_reset(&s);
   CHECK(eq_stat_mean(&s) == 0);
   eq_stat_add(&s, 30);
   eq_stat_add(&s, 10);
   eq_stat_add(&s, 20);
   CHECK(s.n == 3 && s.min == 10 && s.max == 30 && s.last == 20);
   CHECK(eq_stat_mean(&s) == 20);
   eq_stat_add(&s, -5);
   CHECK(s.min == -5);
}

static void test_time(void)
{
   struct timespec ts;
   eq_ns_to_ts(1500000001LL, &ts);
   CHECK(ts.tv_sec == 1 && ts.tv_nsec == 500000001L);
   CHECK(eq_ts_to_ns(&ts) == 1500000001LL);
   eq_ns_to_ts(-1, &ts);
   CHECK(ts.tv_sec == -1 && ts.tv_nsec == 999999999L);
   CHECK(eq_cycle_deadline(1000, 250, 4) == 2000);
   CHECK(eq_missed_periods(2000, 1999, 250) == 0);
   CHECK(eq_missed_periods(2000, 2249, 250) == 0);
   CHECK(eq_missed_periods(2000, 2250, 250) == 1);
   CHECK(eq_missed_periods(2000, 3000, 250) == 4);
   CHECK(eq_missed_periods(2000, 3000, 0) == 0);
}

int main(void)
{
   test_ownership();
   test_empty_image();
   test_stats();
   test_time();
   if (failures == 0)
   {
      printf("ecatcore: all tests passed\n");
   }
   return failures;
}
