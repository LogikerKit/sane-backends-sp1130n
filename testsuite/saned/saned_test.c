/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE Project

   This file is part of the SANE package and is distributed under the
   terms of the GNU General Public License version 3 or later.

   saned tests. */

#define main saned_main
#include "../../frontend/saned.c"
#undef main

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static int failures;

static int cancel_count;

typedef struct
{
  SANE_Status status;
  const SANE_Byte *payload;
  SANE_Int len;
}
ReadEntry;

static ReadEntry read_script[8];
static int read_script_len;
static int read_script_pos;

SANE_Status
sane_init (SANE_Word * version_code, SANE_Auth_Callback authorize)
{
  (void) authorize;
  if (version_code)
    *version_code = SANE_VERSION_CODE (V_MAJOR, V_MINOR, 0);
  return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
}

void
sane_set_auth_callback (SANE_Auth_Callback authorize)
{
  (void) authorize;
}

SANE_Status
sane_get_devices (const SANE_Device *** device_list, SANE_Bool local_only)
{
  (void) local_only;
  *device_list = NULL;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const name, SANE_Handle * handle)
{
  (void) name;
  (void) handle;
  return SANE_STATUS_UNSUPPORTED;
}

void
sane_close (SANE_Handle handle)
{
  (void) handle;
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
  (void) handle;
  (void) option;
  return NULL;
}

SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option,
		     SANE_Action action, void *value, SANE_Int * info)
{
  (void) handle;
  (void) option;
  (void) action;
  (void) value;
  (void) info;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters * params)
{
  (void) handle;
  (void) params;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_start (SANE_Handle handle)
{
  (void) handle;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_read (SANE_Handle handle, SANE_Byte * buffer,
	   SANE_Int max_length, SANE_Int * length)
{
  const ReadEntry *entry;

  (void) handle;
  if (read_script_pos >= read_script_len)
    {
      *length = 0;
      return SANE_STATUS_EOF;
    }
  entry = &read_script[read_script_pos++];
  if (entry->len > max_length)
    *length = max_length;
  else
    *length = entry->len;
  if (entry->status == SANE_STATUS_GOOD && *length > 0)
    memcpy (buffer, entry->payload, *length);
  return entry->status;
}

void
sane_cancel (SANE_Handle handle)
{
  (void) handle;
  cancel_count++;
}

SANE_Status
sane_set_io_mode (SANE_Handle handle, SANE_Bool non_blocking)
{
  (void) handle;
  (void) non_blocking;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_get_select_fd (SANE_Handle handle, SANE_Int * fd)
{
  (void) handle;
  *fd = -1;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_String_Const
sane_strstatus (SANE_Status status)
{
  (void) status;
  return "";
}

static void
test_check_v4_in_range_case (const char *name, const char *peer_ip,
			     const char *base_ip, const char *netmask,
			     SANE_Bool expected)
{
  SANE_Bool ret;

#ifdef SANED_USES_AF_INDEP
  struct sockaddr_in sin;

  memset (&sin, 0, sizeof (sin));
  sin.sin_family = AF_INET;
  if (inet_pton (AF_INET, peer_ip, &sin.sin_addr) != 1)
    {
      fprintf (stderr, "%s: bad peer_ip %s\n", name, peer_ip);
      failures++;
      return;
    }

  ret = check_v4_in_range (&sin, (char *) base_ip, (char *) netmask);
#else /* !SANED_USES_AF_INDEP */
  struct in_addr peer, base;

  if (inet_pton (AF_INET, peer_ip, &peer) != 1
      || inet_pton (AF_INET, base_ip, &base) != 1)
    {
      fprintf (stderr, "%s: bad address %s/%s\n", name, base_ip, peer_ip);
      failures++;
      return;
    }

  ret = check_v4_in_range (&peer, &base, (char *) netmask);
#endif /* SANED_USES_AF_INDEP */

  if (ret != expected)
    {
      fprintf (stderr, "%s: %s/%s contains %s: expected %d, got %d\n",
	       name, base_ip, netmask, peer_ip, expected, ret);
      failures++;
    }
}

static void
test_check_v4_in_range (void)
{
  test_check_v4_in_range_case ("v4 /24 in", "192.168.1.77",
			       "192.168.1.0", "24", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /24 out", "192.168.2.77",
			       "192.168.1.0", "24", SANE_FALSE);
  test_check_v4_in_range_case ("v4 /25 in", "10.0.0.127",
			       "10.0.0.0", "25", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /25 out", "10.0.0.128",
			       "10.0.0.0", "25", SANE_FALSE);
  test_check_v4_in_range_case ("v4 network addr", "10.0.0.0",
			       "10.0.0.0", "24", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /0 in", "1.2.3.4",
			       "0.0.0.0", "0", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /32 in", "10.0.0.1",
			       "10.0.0.1", "32", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /32 out", "10.0.0.2",
			       "10.0.0.1", "32", SANE_FALSE);
  test_check_v4_in_range_case ("v4 alpha netmask", "10.0.0.1",
			       "10.0.0.0", "abc", SANE_FALSE);
  test_check_v4_in_range_case ("v4 netmask 33", "10.0.0.1",
			       "10.0.0.0", "33", SANE_FALSE);
  test_check_v4_in_range_case ("v4 netmask -1", "10.0.0.1",
			       "10.0.0.0", "-1", SANE_FALSE);
  test_check_v4_in_range_case ("v4 netmask 8x", "10.0.0.1",
			       "10.0.0.0", "8x", SANE_FALSE);
}

#ifdef ENABLE_IPV6
static void
test_check_v6_in_range_case (const char *name, const char *peer_ip,
			     const char *base_ip, const char *netmask,
			     SANE_Bool expected)
{
  struct sockaddr_in6 sin6;
  SANE_Bool ret;

  memset (&sin6, 0, sizeof (sin6));
  sin6.sin6_family = AF_INET6;
  if (inet_pton (AF_INET6, peer_ip, &sin6.sin6_addr) != 1)
    {
      fprintf (stderr, "%s: bad peer_ip %s\n", name, peer_ip);
      failures++;
      return;
    }

  ret = check_v6_in_range (&sin6, (char *) base_ip, (char *) netmask);

  if (ret != expected)
    {
      fprintf (stderr, "%s: %s/%s contains %s: expected %d, got %d\n",
	       name, base_ip, netmask, peer_ip, expected, ret);
      failures++;
    }
}

static void
test_check_v6_in_range (void)
{
  test_check_v6_in_range_case ("v6 /64 in", "2001:db8:0:0:77::",
			       "2001:db8::", "64", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /64 out", "2001:db9::77",
			       "2001:db8::", "64", SANE_FALSE);
  test_check_v6_in_range_case ("v6 /17 in", "2001:7fff::1",
			       "2001::", "17", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /17 out", "2001:8000::1",
			       "2001::", "17", SANE_FALSE);
  test_check_v6_in_range_case ("v6 /0 in", "2001:db8::1",
			       "::", "0", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /128 in", "2001:db8::1",
			       "2001:db8::1", "128", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /128 out", "2001:db8::2",
			       "2001:db8::1", "128", SANE_FALSE);
  test_check_v6_in_range_case ("v6 netmask 129", "2001:db8::1",
			       "2001:db8::", "129", SANE_FALSE);
  test_check_v6_in_range_case ("v6 empty netmask", "2001:db8::1",
			       "2001:db8::", "", SANE_FALSE);
  test_check_v6_in_range_case ("v6 netmask 64x", "2001:db8::1",
			       "2001:db8::", "64x", SANE_FALSE);
}
#endif /* ENABLE_IPV6 */

static SANE_Byte payload_a[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static SANE_Byte payload_b[10] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 };
static SANE_Byte payload_c[8] = { 20, 21, 22, 23, 24, 25, 26, 27 };

static void
script_reads (int n, SANE_Status final_status)
{
  int i;

  read_script[0].status = SANE_STATUS_GOOD;
  read_script[0].payload = payload_a;
  read_script[0].len = 10;
  read_script[1].status = SANE_STATUS_GOOD;
  read_script[1].payload = payload_b;
  read_script[1].len = 10;
  read_script[2].status = SANE_STATUS_GOOD;
  read_script[2].payload = payload_c;
  read_script[2].len = 8;
  read_script[3].status = final_status;
  read_script[3].payload = NULL;
  read_script[3].len = 0;
  read_script_len = n + 1;
  read_script_pos = 0;
  for (i = 4; i < 8; ++i)
    {
      read_script[i].status = SANE_STATUS_EOF;
      read_script[i].payload = NULL;
      read_script[i].len = 0;
    }
}

static int
check_expected_stream (const char *name, int fd, SANE_Status final_status)
{
  SANE_Byte expected[64];
  SANE_Byte got[64];
  size_t expect_len, total;
  ssize_t n;

  memset (expected, 0, sizeof (expected));
  total = 0;
  /* first record: reclen 10 + payload */
  expected[0] = 0;
  expected[1] = 0;
  expected[2] = 0;
  expected[3] = 10;
  memcpy (expected + 4, payload_a, 10);
  total = 14;
  /* second record: reclen 10 + payload */
  expected[total + 0] = 0;
  expected[total + 1] = 0;
  expected[total + 2] = 0;
  expected[total + 3] = 10;
  memcpy (expected + total + 4, payload_b, 10);
  total += 14;
  /* third record: reclen 8 + payload */
  expected[total + 0] = 0;
  expected[total + 1] = 0;
  expected[total + 2] = 0;
  expected[total + 3] = 8;
  memcpy (expected + total + 4, payload_c, 8);
  total += 12;
  /* terminal record: 0xffffffff + status byte */
  expected[total + 0] = 0xff;
  expected[total + 1] = 0xff;
  expected[total + 2] = 0xff;
  expected[total + 3] = 0xff;
  expected[total + 4] = (SANE_Byte) final_status;
  total += 5;

  memset (got, 0, sizeof (got));
  expect_len = 0;
  while (expect_len < total)
    {
      n = read (fd, got + expect_len, total - expect_len);
      if (n <= 0)
	{
	  fprintf (stderr, "%s: short read on data fd (%lu of %lu bytes)\n",
		   name, (unsigned long) expect_len, (unsigned long) total);
	  close (fd);
	  return -1;
	}
      expect_len += n;
    }
  if (memcmp (expected, got, total) != 0)
    {
      fprintf (stderr, "%s: data stream mismatch\n", name);
      close (fd);
      return -1;
    }
  return 0;
}

static int
make_handles (void)
{
  if (!handle)
    {
      handle = calloc (16, sizeof (handle[0]));
      if (!handle)
	return -1;
      num_handles = 16;
    }
  handle[0].inuse = 1;
  handle[0].handle = (SANE_Handle) &handle;
  handle[0].scanning = 1;
  handle[0].docancel = 0;
  return 0;
}

static void
init_plain_wire (Wire * w, int fd)
{
  memset (w, 0, sizeof (*w));
  w->io.fd = fd;
}

static void
init_codec_wire (Wire * w, int fd)
{
  sanei_w_init (w, sanei_codec_bin_init);
  w->io.read = read;
  w->io.write = write;
  w->io.fd = fd;
}

static int
setup_do_scan (const char *name, int n, SANE_Status final_status,
	       int *data_fds, int *ctl_fds)
{
  buffer_size = 32;
  cancel_count = 0;
  if (make_handles () < 0
      || socketpair (AF_UNIX, SOCK_STREAM, 0, data_fds) < 0
      || socketpair (AF_UNIX, SOCK_STREAM, 0, ctl_fds) < 0)
    {
      fprintf (stderr, "%s: setup failed\n", name);
      failures++;
      return -1;
    }
  script_reads (n, final_status);
  return 0;
}

static void
check_scan_result (const char *name, int expected_cancels)
{
  if (handle[0].scanning != 0 || handle[0].docancel != 0)
    {
      fprintf (stderr, "%s: scanning=%d docancel=%d\n",
	       name, handle[0].scanning, handle[0].docancel);
      failures++;
    }
  if (cancel_count != expected_cancels)
    {
      fprintf (stderr, "%s: cancel_count=%d\n", name, cancel_count);
      failures++;
    }
}

#define READ_FD 0
#define WRITE_FD 1

static void
test_do_scan_stream (const char *name, SANE_Status final_status)
{
  int data_fds[2];
  int ctl_fds[2];
  Wire w;

  if (setup_do_scan (name, 3, final_status, data_fds, ctl_fds) < 0)
    return;

  init_plain_wire (&w, ctl_fds[READ_FD]);
  do_scan (&w, 0, data_fds[WRITE_FD]);

  check_scan_result (name, 0);
  if (check_expected_stream (name, data_fds[READ_FD], final_status) < 0)
    failures++;
  close (data_fds[WRITE_FD]);
  close (ctl_fds[READ_FD]);
  close (ctl_fds[WRITE_FD]);
}

static void
test_do_scan_write_failure (void)
{
  int data_fds[2];
  int ctl_fds[2];
  Wire w;

  if (setup_do_scan ("do_scan_write_failure", 1, SANE_STATUS_EOF,
		     data_fds, ctl_fds) < 0)
    return;

  close (data_fds[READ_FD]);

  init_plain_wire (&w, ctl_fds[READ_FD]);
  do_scan (&w, 0, data_fds[WRITE_FD]);

  check_scan_result ("do_scan_write_failure", 1);
  close (data_fds[WRITE_FD]);
  close (ctl_fds[READ_FD]);
  close (ctl_fds[WRITE_FD]);
}

static void
test_do_scan_control_eof (void)
{
  int data_fds[2];
  int ctl_fds[2];
  Wire w;

  if (setup_do_scan ("do_scan_control_eof", 1, SANE_STATUS_EOF,
		     data_fds, ctl_fds) < 0)
    return;

  init_codec_wire (&w, ctl_fds[READ_FD]);
  close (ctl_fds[WRITE_FD]);
  do_scan (&w, 0, data_fds[WRITE_FD]);

  check_scan_result ("do_scan_control_eof", 1);
  sanei_w_exit (&w);
  close (data_fds[WRITE_FD]);
  close (data_fds[READ_FD]);
}

static void
test_do_scan_rpc_cancel (void)
{
  int data_fds[2];
  int ctl_fds[2];
  Wire w, peer;
  SANE_Word procnum, arg;

  if (setup_do_scan ("do_scan_rpc_cancel", 1, SANE_STATUS_EOF,
		     data_fds, ctl_fds) < 0)
    return;

  init_codec_wire (&peer, ctl_fds[WRITE_FD]);
  sanei_w_set_dir (&peer, WIRE_ENCODE);
  procnum = SANE_NET_CANCEL;
  sanei_w_word (&peer, &procnum);
  arg = 0;
  sanei_w_word (&peer, &arg);
  sanei_w_set_dir (&peer, WIRE_DECODE);
  sanei_w_exit (&peer);

  init_codec_wire (&w, ctl_fds[READ_FD]);
  do_scan (&w, 0, data_fds[WRITE_FD]);

  check_scan_result ("do_scan_rpc_cancel", 2);
  sanei_w_exit (&w);
  close (data_fds[WRITE_FD]);
  close (data_fds[READ_FD]);
  close (ctl_fds[READ_FD]);
}

int
main (void)
{
  debug = DBG_ERR;
  log_to_syslog = SANE_FALSE;
  prog_name = "saned_test";
  alarm (60);
  signal (SIGPIPE, SIG_IGN);

  test_check_v4_in_range ();
#ifdef ENABLE_IPV6
  test_check_v6_in_range ();
#endif
  test_do_scan_stream ("do_scan_data_eof", SANE_STATUS_EOF);
  test_do_scan_stream ("do_scan_error_status", SANE_STATUS_IO_ERROR);
  test_do_scan_write_failure ();
  test_do_scan_control_eof ();
  test_do_scan_rpc_cancel ();

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
