/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception */
#define _POSIX_C_SOURCE 200809L
#include "sp1130n.h"
#include <assert.h>
#include <stdio.h>
#include <jpeglib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void
jpeg (unsigned value, unsigned width, unsigned height)
{
  assert (width && width <= 10000 && height && height <= 20000);
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr error;
  c.err = jpeg_std_error (&error);
  jpeg_create_compress (&c);
  unsigned char *data = NULL;
  unsigned long size = 0;
  jpeg_mem_dest (&c, &data, &size);
  c.image_width = width;
  c.image_height = height;
  c.input_components = 3;
  c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_set_quality (&c, 100, TRUE);
  jpeg_start_compress (&c, TRUE);
  unsigned char *row = malloc (width * 3);
  assert (row);
  memset (row, value, width * 3);
  while (c.next_scanline < c.image_height)
    {
      JSAMPROW rows[] = { row };
      jpeg_write_scanlines (&c, rows, 1);
    }
  jpeg_finish_compress (&c);
  assert (fwrite (data, 1, size, stdout) == size);
  free (data);
  free (row);
  jpeg_destroy_compress (&c);
}

static void *
cancel_later (void *flag)
{
  struct timespec t = { 0, 60000000 };
  nanosleep (&t, NULL);
  atomic_store ((atomic_bool *) flag, 1);
  return NULL;
}

int
main (int argc, char **argv)
{
  assert (argc == 3 || argc == 5);
  if (!strcmp (argv[1], "jpeg"))
    {
      assert (argc == 3);
      jpeg (!strcmp (argv[2], "back") ? 200 : 40, 4, 3);
      return 0;
    }
  if (!strcmp (argv[1], "jpeg-gray"))
    {
      char *end;
      unsigned long value = strtoul (argv[2], &end, 10);
      assert (end != argv[2] && !*end && value <= 255);
      unsigned width = 4, height = 3;
      if (argc == 5)
        {
          unsigned long n = strtoul (argv[3], &end, 10);
          assert (end != argv[3] && !*end && n && n <= 10000);
          width = (unsigned) n;
          n = strtoul (argv[4], &end, 10);
          assert (end != argv[4] && !*end && n && n <= 20000);
          height = (unsigned) n;
        }
      jpeg ((unsigned) value, width, height);
      return 0;
    }
  assert (argc == 3);
  if (!strcmp (argv[1], "parse"))
    {
      struct sanei_sp1130n_net *n = NULL;
      SANE_Status status = sanei_sp1130n_net_open (argv[2], &n);
      sanei_sp1130n_net_close (n);
      printf ("%d\n", status);
      return 0;
    }
  /* Timing fixtures are restricted to an explicitly supplied loopback address. */
  assert (!strncmp (argv[2], "tcp:127.0.0.1:", 14));
  struct sanei_sp1130n_net *n = NULL;
  assert (sanei_sp1130n_net_open (argv[2], &n) == SANE_STATUS_GOOD);
  atomic_bool cancelled;
  atomic_init (&cancelled, 0);
  struct sanei_sp1130n_io io = sanei_sp1130n_net_io (n, &cancelled);
  uint8_t reply[108] = { 0 };
  if (!strcmp (argv[1], "cancel"))
    {
      pthread_t thread;
      assert (!pthread_create (&thread, NULL, cancel_later, &cancelled));
      assert (sanei_sp1130n_probe (&io, reply) == SANE_STATUS_CANCELLED);
      pthread_join (thread, NULL);
    }
  else
    {
      assert (!strcmp (argv[1], "timeout"));
      assert (sanei_sp1130n_command (&io, 0x12, 0x7a, 108, 0, NULL, 0) ==
              SANE_STATUS_GOOD);
      assert (sanei_sp1130n_exact (&io, 1, reply, sizeof reply, 100) == SANE_STATUS_IO_ERROR);
    }
  sanei_sp1130n_net_close (n);
  return 0;
}
