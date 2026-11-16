/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * axisdemo - drive a CiA 402 servo axis through ecatd.
 *
 * A separate process from ecatd: it talks to the bus only through /dev/ecat.
 * Every bus cycle (poll() on /dev/ecat/in) it reads the statusword and actual
 * position, runs the CiA 402 enable sequence and then commands a smooth
 * sinusoidal position profile in cyclic synchronous position mode.
 *
 *   axisdemo [-s slave] [-a amplitude_counts] [-T period_s] [-n seconds]
 *
 * If this process is killed (even with SIGKILL), ecatd zeroes the outputs it
 * owned: the controlword becomes 0, so the drive disables (safe stop).
 */
#include "etherq/ecat_devctl.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EQ_VENDOR 0x45514E58u
#define EQ_SERVO  0x00000003u

static volatile sig_atomic_t g_stop;
static void on_sig(int s)
{
   (void)s;
   g_stop = 1;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int32_t rd32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, int32_t v)
{
   uint32_t u = (uint32_t)v;
   p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *drive_state(uint16_t sw)
{
   if ((sw & 0x4F) == 0x08) return "FAULT";
   if ((sw & 0x4F) == 0x40) return "SWITCH ON DISABLED";
   if ((sw & 0x6F) == 0x21) return "READY TO SWITCH ON";
   if ((sw & 0x6F) == 0x23) return "SWITCHED ON";
   if ((sw & 0x6F) == 0x27) return "OPERATION ENABLED";
   if ((sw & 0x6F) == 0x07) return "QUICK STOP ACTIVE";
   return "NOT READY";
}

static int find_servo(int fd, ecat_slave_t *out, int want)
{
   ecat_info_t info;
   uint32_t i;
   if (devctl(fd, DCMD_ECAT_GET_INFO, &info, sizeof(info), NULL) != EOK)
      return -1;
   for (i = 1; i <= info.slave_count; i++)
   {
      memset(out, 0, sizeof(*out));
      out->index = i;
      if (devctl(fd, DCMD_ECAT_GET_SLAVE, out, sizeof(*out), NULL) != EOK)
         return -1;
      if (want ? (int)i == want : (out->vendor_id == EQ_VENDOR && out->product_code == EQ_SERVO))
         return (out->obits >= 48 && out->ibits >= 80) ? 0 : -1; /* 6 bytes out, 10 bytes in */
   }
   return -1;
}

int main(int argc, char *argv[])
{
   double amplitude = 200000.0, period = 4.0, duration = 0.0, t0 = 0.0, last_print = 0.0, start;
   int want = 0, opt, in_fd, out_fd;
   int32_t origin = 0, target = 0, pos;
   uint16_t sw, cw = 0;
   uint8_t in[10], out[6];
   ecat_slave_t s;
   struct pollfd pfd;
   int enabled = 0;

   while ((opt = getopt(argc, argv, "s:a:T:n:h")) != -1)
   {
      switch (opt)
      {
      case 's': want = atoi(optarg); break;
      case 'a': amplitude = atof(optarg); break;
      case 'T': period = atof(optarg); break;
      case 'n': duration = atof(optarg); break;
      default:
         fprintf(stderr, "usage: axisdemo [-s slave] [-a amplitude_counts] [-T period_s] [-n seconds]\n");
         return opt == 'h' ? 0 : 2;
      }
   }
   in_fd = open(ECAT_PATH_IN, O_RDONLY | O_CLOEXEC);
   out_fd = open(ECAT_PATH_OUT, O_RDWR | O_CLOEXEC);
   if (in_fd == -1 || out_fd == -1)
   {
      fprintf(stderr, "axisdemo: cannot open /dev/ecat: %s (is ecatd running?)\n", strerror(errno));
      return 1;
   }
   if (find_servo(in_fd, &s, want) != 0)
   {
      fprintf(stderr, "axisdemo: no CiA 402 axis found%s\n", want ? " at that position" : "");
      return 1;
   }
   printf("axisdemo: slave %u '%s' (outputs @%u, inputs @%u), pid %d\n", s.index, s.name, s.ooffset,
          s.ioffset, (int)getpid());
   signal(SIGINT, on_sig);
   signal(SIGTERM, on_sig);
   pfd.fd = in_fd;
   pfd.events = POLLIN;
   start = now_s();

   while (!g_stop && (duration <= 0 || now_s() - start < duration))
   {
      if (poll(&pfd, 1, 1000) <= 0)
         continue; /* bus not cycling */
      if (pread(in_fd, in, sizeof(in), s.ioffset) != (ssize_t)sizeof(in))
         break;
      sw = rd16(in);
      pos = rd32(in + 2);

      if ((sw & 0x4F) == 0x08)
      {
         cw = (cw & 0x80) ? 0x00 : 0x80; /* rising edge of bit 7 = fault reset */
         enabled = 0;
      }
      else if ((sw & 0x4F) == 0x40)
      {
         cw = 0x06; /* shutdown -> ready to switch on */
         target = pos;
         enabled = 0;
      }
      else if ((sw & 0x6F) == 0x21)
      {
         cw = 0x07; /* switch on */
         target = pos;
      }
      else if ((sw & 0x6F) == 0x23)
      {
         cw = 0x0F; /* enable operation; target == actual avoids a following error */
         target = pos;
      }
      else if ((sw & 0x6F) == 0x27)
      {
         if (!enabled)
         {
            enabled = 1;
            origin = pos;
            t0 = now_s();
            printf("axisdemo: OPERATION ENABLED at %d counts, starting profile\n", pos);
         }
         cw = 0x0F;
         target = origin + (int32_t)lround(amplitude * sin(2.0 * M_PI * (now_s() - t0) / period));
      }
      wr16(out, cw);
      wr32(out + 2, target);
      if (pwrite(out_fd, out, sizeof(out), s.ooffset) != (ssize_t)sizeof(out))
      {
         fprintf(stderr, "axisdemo: pwrite: %s\n", strerror(errno));
         break;
      }
      if (now_s() - last_print > 0.5)
      {
         last_print = now_s();
         printf("  %-20s sw=0x%04x  actual=%9d  target=%9d\n", drive_state(sw), sw, pos, target);
         fflush(stdout);
      }
   }
   /* orderly stop: shutdown command for a few cycles, then close (ecatd zeroes) */
   wr16(out, 0x06);
   wr32(out + 2, target);
   for (int i = 0; i < 5; i++)
   {
      if (poll(&pfd, 1, 200) > 0)
      {
         (void)pread(in_fd, in, sizeof(in), s.ioffset);
         (void)pwrite(out_fd, out, sizeof(out), s.ooffset);
      }
   }
   printf("axisdemo: stopped\n");
   close(out_fd);
   close(in_fd);
   return 0;
}
