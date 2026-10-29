/* SPDX-License-Identifier: MIT */
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
jpeg (unsigned side)
{
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr error;
  c.err = jpeg_std_error (&error);
  jpeg_create_compress (&c);
  unsigned char *data = NULL;
  unsigned long size = 0;
  jpeg_mem_dest (&c, &data, &size);
  c.image_width = 4;
  c.image_height = 3;
  c.input_components = 3;
  c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_set_quality (&c, 100, TRUE);
  jpeg_start_compress (&c, TRUE);
  unsigned char row[12];
  memset (row, side ? 200 : 40, sizeof row);
  while (c.next_scanline < c.image_height)
    {
      JSAMPROW rows[] = { row };
      jpeg_write_scanlines (&c, rows, 1);
    }
  jpeg_finish_compress (&c);
  assert (fwrite (data, 1, size, stdout) == size);
  free (data);
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
  assert (argc == 3);
  if (!strcmp (argv[1], "jpeg"))
    {
      jpeg (!strcmp (argv[2], "back"));
      return 0;
    }
  if (!strcmp (argv[1], "parse"))
    {
      struct sanei_sp1130n_net *n = NULL;
      int status = sanei_sp1130n_net_open (argv[2], &n);
      sanei_sp1130n_net_close (n);
      printf ("%d\n", status);
      return 0;
    }
  /* Timing fixtures are restricted to an explicitly supplied loopback address. */
  assert (!strncmp (argv[2], "tcp:127.0.0.1:", 14));
  struct sanei_sp1130n_net *n = NULL;
  assert (sanei_sp1130n_net_open (argv[2], &n) == SP_OK);
  atomic_bool cancelled;
  atomic_init (&cancelled, 0);
  struct sanei_sp1130n_io io = sanei_sp1130n_net_io (n, &cancelled);
  uint8_t reply[108] = { 0 };
  if (!strcmp (argv[1], "cancel"))
    {
      pthread_t thread;
      assert (!pthread_create (&thread, NULL, cancel_later, &cancelled));
      assert (sanei_sp1130n_probe (&io, reply) == SP_CANCELLED);
      pthread_join (thread, NULL);
    }
  else
    {
      assert (!strcmp (argv[1], "timeout"));
      assert (sanei_sp1130n_command (&io, 0x12, 0x7a, 108, 0, NULL, 0) ==
              SP_OK);
      assert (sanei_sp1130n_exact (&io, 1, reply, sizeof reply, 100) == SP_IO);
    }
  sanei_sp1130n_net_close (n);
  return 0;
}
