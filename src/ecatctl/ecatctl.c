/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ecatctl - command line client for ecatd (/dev/ecat).
 *
 *   ecatctl info | slaves | stats [-r] | status
 *   ecatctl read in|out [offset [length]]
 *   ecatctl watch [cycles]             print inputs whenever they change (poll on /dev/ecat/in)
 *   ecatctl hold <offset> <byte>...    own output bytes until Ctrl-C (then ecatd zeroes them)
 *   ecatctl state init|preop|safeop|op
 */
#include "etherq/ecat_devctl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *state_name(uint32_t s)
{
   switch (s & 0x0F)
   {
   case ECAT_STATE_INIT:
      return "INIT";
   case ECAT_STATE_PREOP:
      return "PRE-OP";
   case ECAT_STATE_BOOT:
      return "BOOT";
   case ECAT_STATE_SAFEOP:
      return "SAFE-OP";
   case ECAT_STATE_OP:
      return "OP";
   default:
      return "NONE";
   }
}

static int open_or_die(const char *path, int flags)
{
   int fd = open(path, flags | O_CLOEXEC);
   if (fd == -1)
   {
      fprintf(stderr, "ecatctl: %s: %s (is ecatd running?)\n", path, strerror(errno));
      exit(1);
   }
   return fd;
}

static int dc(int fd, int cmd, void *data, size_t n)
{
   int rc = devctl(fd, cmd, data, n, NULL);
   if (rc != EOK)
   {
      fprintf(stderr, "ecatctl: devctl: %s\n", strerror(rc));
      exit(1);
   }
   return 0;
}

static void hexdump(const uint8_t *b, size_t n, size_t base)
{
   size_t i;
   for (i = 0; i < n; i++)
   {
      if (i % 16 == 0)
         printf("%s%04zx:", i ? "\n" : "", base + i);
      printf(" %02x", b[i]);
   }
   printf("\n");
}

static int cmd_info(void)
{
   ecat_info_t info;
   int fd = open_or_die(ECAT_PATH_IN, O_RDONLY);
   dc(fd, DCMD_ECAT_GET_INFO, &info, sizeof(info));
   printf("interface      %s\n", info.ifname);
   printf("slaves         %u\n", info.slave_count);
   printf("bus state      %s\n", state_name(info.group_state));
   printf("process image  %u output bytes, %u input bytes\n", info.obytes, info.ibytes);
   printf("expected WKC   %u\n", info.expected_wkc);
   printf("cycle          %u us at priority %u\n", info.period_ns / 1000, info.cycle_priority);
   close(fd);
   return 0;
}

static int cmd_slaves(void)
{
   ecat_info_t info;
   ecat_slave_t s;
   uint32_t i;
   int fd = open_or_die(ECAT_PATH_IN, O_RDONLY);

   dc(fd, DCMD_ECAT_GET_INFO, &info, sizeof(info));
   printf("  # state     AL    addr   vendor     product    obits ibits out@ in@  name\n");
   for (i = 1; i <= info.slave_count; i++)
   {
      memset(&s, 0, sizeof(s));
      s.index = i;
      dc(fd, DCMD_ECAT_GET_SLAVE, &s, sizeof(s));
      printf("%3u %-8s%c 0x%04x 0x%04x 0x%08x 0x%08x %5u %5u %4u %4u  %s\n", s.index, state_name(s.state),
             (s.state & ECAT_STATE_ERROR) ? '!' : ' ', s.al_status_code, s.configured_address,
             s.vendor_id, s.product_code, s.obits, s.ibits, s.ooffset, s.ioffset, s.name);
   }
   close(fd);
   return 0;
}

static int cmd_stats(int reset)
{
   ecat_stats_t st;
   int fd = open_or_die(reset ? ECAT_PATH_OUT : ECAT_PATH_IN, reset ? O_RDWR : O_RDONLY);
   if (reset)
   {
      dc(fd, DCMD_ECAT_RESET_STATS, NULL, 0);
      printf("statistics reset\n");
      close(fd);
      return 0;
   }
   dc(fd, DCMD_ECAT_GET_STATS, &st, sizeof(st));
   printf("cycles          %llu\n", (unsigned long long)st.cycles);
   printf("last WKC        %u\n", st.last_wkc);
   printf("WKC errors      %llu\n", (unsigned long long)st.wkc_errors);
   printf("missed periods  %llu\n", (unsigned long long)st.missed_periods);
   printf("overruns        %llu\n", (unsigned long long)st.overruns);
   printf("recoveries      %llu\n", (unsigned long long)st.recoveries);
   printf("output clients  %u\n", st.clients);
   printf("wake latency    min %.1f us  mean %.1f us  max %.1f us  (measured on this target)\n",
          st.wake_min_ns / 1e3, st.wake_mean_ns / 1e3, st.wake_max_ns / 1e3);
   printf("cycle exec      min %.1f us  mean %.1f us  max %.1f us\n", st.exec_min_ns / 1e3,
          st.exec_mean_ns / 1e3, st.exec_max_ns / 1e3);
   close(fd);
   return 0;
}

static int cmd_status(void)
{
   char buf[4096];
   ssize_t n;
   int fd = open_or_die(ECAT_PATH_STATUS, O_RDONLY);
   while ((n = read(fd, buf, sizeof(buf))) > 0)
      fwrite(buf, 1, (size_t)n, stdout);
   close(fd);
   return 0;
}

static int cmd_read(const char *which, size_t off, long len)
{
   ecat_info_t info;
   uint8_t buf[4096];
   ssize_t n;
   int out = strcmp(which, "out") == 0;
   int fd = open_or_die(out ? ECAT_PATH_OUT : ECAT_PATH_IN, O_RDONLY);

   dc(fd, DCMD_ECAT_GET_INFO, &info, sizeof(info));
   if (len < 0)
      len = (long)(out ? info.obytes : info.ibytes) - (long)off;
   if (len <= 0)
   {
      close(fd);
      return 0;
   }
   if ((size_t)len > sizeof(buf))
      len = sizeof(buf);
   n = pread(fd, buf, (size_t)len, (off_t)off);
   if (n < 0)
   {
      perror("ecatctl: pread");
      return 1;
   }
   hexdump(buf, (size_t)n, off);
   close(fd);
   return 0;
}

static int cmd_watch(long cycles)
{
   ecat_info_t info;
   uint8_t cur[4096], prev[4096];
   struct pollfd pfd;
   long seen = 0;
   ssize_t n;
   int first = 1;
   int fd = open_or_die(ECAT_PATH_IN, O_RDONLY);

   dc(fd, DCMD_ECAT_GET_INFO, &info, sizeof(info));
   if (info.ibytes > sizeof(cur))
      info.ibytes = sizeof(cur);
   pfd.fd = fd;
   pfd.events = POLLIN;
   while (cycles <= 0 || seen < cycles)
   {
      /* poll() wakes once per completed bus cycle (ecatd ionotify) */
      if (poll(&pfd, 1, 2000) <= 0)
      {
         fprintf(stderr, "ecatctl: no cycle for 2 s\n");
         continue;
      }
      n = pread(fd, cur, info.ibytes, 0);
      if (n < 0)
         break;
      seen++;
      if (first || memcmp(cur, prev, (size_t)n) != 0)
      {
         printf("cycle %-8ld in:", seen);
         for (ssize_t i = 0; i < n; i++)
            printf(" %02x", cur[i]);
         printf("\n");
         fflush(stdout);
         memcpy(prev, cur, (size_t)n);
         first = 0;
      }
   }
   close(fd);
   return 0;
}

static volatile sig_atomic_t g_stop;
static void on_sig(int s)
{
   (void)s;
   g_stop = 1;
}

static int cmd_hold(size_t off, int argc, char **argv)
{
   uint8_t buf[256];
   int i, fd;

   if (argc <= 0 || argc > (int)sizeof(buf))
      return 2;
   for (i = 0; i < argc; i++)
      buf[i] = (uint8_t)strtoul(argv[i], NULL, 0);
   fd = open_or_die(ECAT_PATH_OUT, O_RDWR);
   if (pwrite(fd, buf, (size_t)argc, (off_t)off) != argc)
   {
      fprintf(stderr, "ecatctl: pwrite: %s\n", strerror(errno));
      return 1;
   }
   printf("holding %d output byte(s) at offset %zu; Ctrl-C releases them (ecatd zeroes them)\n", argc, off);
   fflush(stdout);
   signal(SIGINT, on_sig);
   signal(SIGTERM, on_sig);
   while (!g_stop)
      pause();
   close(fd);
   return 0;
}

static int cmd_state(const char *name)
{
   uint32_t s;
   int fd;
   if (!strcmp(name, "init"))
      s = ECAT_STATE_INIT;
   else if (!strcmp(name, "preop"))
      s = ECAT_STATE_PREOP;
   else if (!strcmp(name, "safeop"))
      s = ECAT_STATE_SAFEOP;
   else if (!strcmp(name, "op"))
      s = ECAT_STATE_OP;
   else
      return 2;
   fd = open_or_die(ECAT_PATH_OUT, O_RDWR); /* changing the bus state needs write access */
   dc(fd, DCMD_ECAT_SET_STATE, &s, sizeof(s));
   printf("requested %s\n", state_name(s));
   close(fd);
   return 0;
}

static void usage(void)
{
   fprintf(stderr,
           "usage: ecatctl info | slaves | stats [-r] | status\n"
           "       ecatctl read in|out [offset [length]]\n"
           "       ecatctl watch [cycles]\n"
           "       ecatctl hold <offset> <byte>...\n"
           "       ecatctl state init|preop|safeop|op\n");
}

int main(int argc, char *argv[])
{
   int rc = 2;
   if (argc < 2)
   {
      usage();
      return 2;
   }
   if (!strcmp(argv[1], "info"))
      rc = cmd_info();
   else if (!strcmp(argv[1], "slaves"))
      rc = cmd_slaves();
   else if (!strcmp(argv[1], "stats"))
      rc = cmd_stats(argc > 2 && !strcmp(argv[2], "-r"));
   else if (!strcmp(argv[1], "status"))
      rc = cmd_status();
   else if (!strcmp(argv[1], "read") && argc >= 3)
      rc = cmd_read(argv[2], argc > 3 ? strtoul(argv[3], NULL, 0) : 0, argc > 4 ? strtol(argv[4], NULL, 0) : -1);
   else if (!strcmp(argv[1], "watch"))
      rc = cmd_watch(argc > 2 ? strtol(argv[2], NULL, 0) : 0);
   else if (!strcmp(argv[1], "hold") && argc >= 4)
      rc = cmd_hold(strtoul(argv[2], NULL, 0), argc - 3, argv + 3);
   else if (!strcmp(argv[1], "state") && argc >= 3)
      rc = cmd_state(argv[2]);
   if (rc == 2)
      usage();
   return rc;
}
