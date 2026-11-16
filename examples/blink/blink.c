/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * blink - the smallest useful /dev/ecat client (EtherQ codelab).
 *
 * Runs a "knight rider" pattern on the 8 digital outputs of the EQ-DO8 slave,
 * advancing one LED every N bus cycles. It wakes up with poll() on
 * /dev/ecat/in, which ecatd makes readable once per completed EtherCAT cycle,
 * and writes one byte to /dev/ecat/out at the DO8's offset.
 *
 *   blink [cycles_per_step]      (default 25)
 *
 * Kill it with SIGKILL and watch the LEDs go dark: ecatd zeroes the bytes this
 * process owned when QNX closes its file descriptors.
 */
#include "etherq/ecat_devctl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EQ_VENDOR 0x45514E58u
#define EQ_DO8    0x00000002u

int main(int argc, char *argv[])
{
   int every = argc > 1 ? atoi(argv[1]) : 25;
   int in = open(ECAT_PATH_IN, O_RDONLY | O_CLOEXEC);
   int out = open(ECAT_PATH_OUT, O_RDWR | O_CLOEXEC);
   ecat_info_t info;
   ecat_slave_t s;
   uint32_t i, off = 0;
   int found = 0, pos = 0, dir = 1;
   long cycle = 0;
   uint8_t in_byte, led;
   struct pollfd pfd = {.fd = in, .events = POLLIN};

   if (in == -1 || out == -1 || every < 1)
   {
      fprintf(stderr, "blink: cannot open /dev/ecat (%s) - is ecatd running?\n", strerror(errno));
      return 1;
   }
   /* 1. ask ecatd where the DO8 lives in the output image (devctl = a QNX message) */
   if (devctl(in, DCMD_ECAT_GET_INFO, &info, sizeof(info), NULL) != EOK)
   {
      fprintf(stderr, "blink: GET_INFO failed\n");
      return 1;
   }
   for (i = 1; i <= info.slave_count && !found; i++)
   {
      memset(&s, 0, sizeof(s));
      s.index = i;
      if (devctl(in, DCMD_ECAT_GET_SLAVE, &s, sizeof(s), NULL) == EOK && s.vendor_id == EQ_VENDOR &&
          s.product_code == EQ_DO8)
      {
         off = s.ooffset;
         found = 1;
      }
   }
   if (!found)
   {
      fprintf(stderr, "blink: no EQ-DO8 slave on the bus\n");
      return 1;
   }
   printf("blink: DO8 is slave %u, output byte %u; pid %d\n", s.index, off, (int)getpid());

   for (;;)
   {
      /* 2. sleep until ecatd finished the next bus cycle */
      if (poll(&pfd, 1, 1000) <= 0)
         continue;
      (void)pread(in, &in_byte, 1, 0); /* reading marks this cycle as seen */
      if (++cycle % every)
         continue;
      /* 3. move the light and write our byte - this claims it for this process */
      led = (uint8_t)(1u << pos);
      if (pwrite(out, &led, 1, off) != 1)
      {
         fprintf(stderr, "blink: pwrite: %s\n", strerror(errno)); /* EBUSY: someone else owns it */
         return 1;
      }
      pos += dir;
      if (pos == 7 || pos == 0)
         dir = -dir;
   }
}
