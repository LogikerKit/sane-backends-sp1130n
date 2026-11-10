/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception */
#define _POSIX_C_SOURCE 200809L
#include "sp1130n.h"
#include "sp1130n-image.h"
#include "../../../include/sane/sane.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef SP_BACKEND_TEST_SIGNAL
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

SANE_Status sane_sp1130n_init (SANE_Int *, SANE_Auth_Callback);
SANE_Status sane_sp1130n_get_devices (const SANE_Device ***, SANE_Bool);
SANE_Status sane_sp1130n_open (SANE_String_Const, SANE_Handle *);
SANE_Status sane_sp1130n_control_option (SANE_Handle, SANE_Int, SANE_Action,
                                         void *, SANE_Int *);
SANE_Status sane_sp1130n_start (SANE_Handle);
SANE_Status sane_sp1130n_read (SANE_Handle, SANE_Byte *, SANE_Int,
                               SANE_Int *);
SANE_Status sane_sp1130n_get_parameters (SANE_Handle, SANE_Parameters *);
const SANE_Option_Descriptor *sane_sp1130n_get_option_descriptor (SANE_Handle,
                                                                  SANE_Int);
void sane_sp1130n_cancel (SANE_Handle);
void sane_sp1130n_close (SANE_Handle);
void sane_sp1130n_exit (void);
static atomic_uint scans;
static int use_pattern, malformed;
static int keep_session;
static atomic_uint finished_sessions;
static unsigned blank_masks[32], blank_count, blank_index;
static unsigned numbered_count, numbered_index;
static unsigned fail_after, cancel_after;
static SANE_Handle drain_handle;
static size_t encoded_bytes;
static SANE_Status usb_list_status;
static int usb_absent;
static struct sanei_sp1130n_settings last_settings;
#ifdef SP_BACKEND_TEST_SIGNAL
static SANE_Handle signal_handle;
static volatile sig_atomic_t signal_armed;
static pthread_t signal_thread;

static void
cancel_signal (int number)
{
  (void) number;
  sane_sp1130n_cancel (signal_handle);
}

int __real_pthread_mutex_lock (pthread_mutex_t *);
int
__wrap_pthread_mutex_lock (pthread_mutex_t *mutex)
{
  int status = __real_pthread_mutex_lock (mutex);
  if (!status && pthread_equal (pthread_self (), signal_thread) &&
      signal_armed)
    {
      signal_armed = 0;
      assert (raise (SIGUSR1) == 0);
    }
  return status;
}

static void
signal_cancellation_test (SANE_Handle h)
{
  struct sigaction action = { .sa_handler = cancel_signal }, previous;
  sigemptyset (&action.sa_mask);
  assert (sigaction (SIGUSR1, &action, &previous) == 0);
  signal_handle = h;
  signal_armed = 1;
  alarm (3);                     /* A regression fails instead of hanging check. */
  uint8_t byte;
  SANE_Int length = 99;
  assert (sane_sp1130n_read (h, &byte, 1, &length) == SANE_STATUS_CANCELLED
          && !length);
  alarm (0);
  assert (sigaction (SIGUSR1, &previous, NULL) == 0);
  signal_handle = NULL;
}
#endif
enum
{
  TL_X = 10, TL_Y, BR_X, BR_Y, AUTOCROP, DESKEW, SKIP_BLANK,
  BLANK_FRACTION, CROP_MARGIN, UPRIGHT_LOADING, BATCH_SCAN
};
static const uint8_t front_pattern[12] = {
  255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128
};

static const uint8_t back_pattern[12] = {
  10, 10, 10, 20, 20, 20, 30, 30, 30, 40, 40, 40
};

struct sanei_sp1130n_usb
{
  int unused;
};

uint32_t
sanei_sp1130n_le32 (const uint8_t *p)
{
  return (uint32_t) p[0] | (uint32_t) p[1] << 8 |
    (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

SANE_Status
sanei_sp1130n_usb_list (struct sanei_sp1130n_device **out, size_t *count)
{
  *out = NULL;
  *count = 0;
  if (usb_list_status || usb_absent)
    return usb_list_status;
  *out = calloc (1, sizeof **out);
  assert (*out);
  strcpy ((*out)->name, "sp1130n:usb:001:002");
  (*out)->vendor = SP_FUJITSU_VID;
  *count = 1;
  return SANE_STATUS_GOOD;
}

static void
discovery_tests (void)
{
  char directory[] = "/tmp/sp1130n-config-XXXXXX";
  assert (mkdtemp (directory));
  assert (setenv ("SANE_CONFIG_DIR", directory, 1) == 0);
  const SANE_Device **devices;
  assert (sane_sp1130n_get_devices (&devices, SANE_FALSE) == SANE_STATUS_GOOD);
  assert (devices[0] && !devices[1]);
  char path[128];
  snprintf (path, sizeof path, "%s/sp1130n.conf", directory);
  FILE *file = fopen (path, "w");
  assert (file);
  assert (fputs (
    "# Authored configuration; no scanner connection is opened.\n\n"
    " tcp:192.0.2.1 # default port\n"
    "sp1130n:tcp:192.0.2.1:16888\n"
    "tcp:192.0.2.1:016888\n"
    "\"tcp:192.0.2.2:12345\"\n"
    "tcp:scanner.example\n"
    "tcp:224.0.0.1\n"
    "tcp:192.0.2.3:0\n"
    "tcp:192.0.2.3:65536\n"
    "tcp:192.0.2.3 unexpected\n"
    "usb:001:002\n"
    "\"unterminated\n", file) >= 0);
  assert (fclose (file) == 0);
  unsigned before = scans;
  assert (sane_sp1130n_get_devices (&devices, SANE_FALSE) == SANE_STATUS_GOOD);
  assert (devices[0] && devices[1] && devices[2] && !devices[3]);
  assert (!strcmp (devices[0]->name, "usb:001:002"));
  assert (!strcmp (devices[1]->name, "tcp:192.0.2.1:16888"));
  assert (!strcmp (devices[2]->name, "tcp:192.0.2.2:12345"));
  assert (!strcmp (devices[1]->vendor, "Ricoh/Fujitsu"));
  assert (sane_sp1130n_get_devices (&devices, SANE_TRUE) == SANE_STATUS_GOOD);
  assert (devices[0] && !devices[1]);
  usb_absent = 1;
  assert (sane_sp1130n_get_devices (&devices, SANE_FALSE) == SANE_STATUS_GOOD);
  assert (devices[0] && devices[1] && !devices[2]);
  assert (!strcmp (devices[0]->name, "tcp:192.0.2.1:16888"));
  usb_list_status = SANE_STATUS_IO_ERROR;
  assert (sane_sp1130n_get_devices (&devices, SANE_FALSE) == SANE_STATUS_GOOD);
  assert (devices[0] && devices[1] && !devices[2]);
  assert (sane_sp1130n_get_devices (&devices, SANE_TRUE) == SANE_STATUS_IO_ERROR);
  assert (!devices);
  usb_list_status = 0;
  assert (unlink (path) == 0);
  assert (rmdir (directory) == 0);
  assert (sane_sp1130n_get_devices (&devices, SANE_FALSE) == SANE_STATUS_GOOD);
  assert (!devices[0] && scans == before);
  usb_absent = 0;
}

SANE_Status
sanei_sp1130n_scan_next (struct sanei_sp1130n_io *io,
                         const struct sanei_sp1130n_settings *s,
                         const uint8_t token[40],
                         struct sanei_sp1130n_session *session,
                         struct sanei_sp1130n_sheet *sheet)
{
  if (session->error)
    return session->error;
  if (session->exhausted)
    return SANE_STATUS_NO_DOCS;
  SANE_Status status = sanei_sp1130n_scan_sheet (io, s, token, sheet);
  if (status == SANE_STATUS_GOOD)
    {
      ++session->sheets;
      if (keep_session)
        session->started = session->own_lock = 1;
    }
  if (status == SANE_STATUS_NO_DOCS)
    {
      session->exhausted = 1;
      sanei_sp1130n_scan_finish (io, token, session);
    }
  return status;
}

SANE_Status
sanei_sp1130n_scan_next_encoded (struct sanei_sp1130n_io *io,
                                 const struct sanei_sp1130n_settings *s,
                                 const uint8_t token[40],
                                 struct sanei_sp1130n_session *session,
                                 struct sanei_sp1130n_sheet *sheet)
{
  SANE_Status status = sanei_sp1130n_scan_next (io, s, token, session, sheet);
  if (status == SANE_STATUS_GOOD && encoded_bytes)
    {
      sheet->encoded = 1;
      for (unsigned side = 0; side < sheet->count; ++side)
        sheet->page[side].size = encoded_bytes;
    }
  return status;
}

SANE_Status
sanei_sp1130n_decode_jpeg (uint8_t *data, size_t size, unsigned width,
                           unsigned height, struct sanei_sp1130n_page *page)
{
  if (encoded_bytes)
    {
      /* Queue budgeting uses declared wire lengths, without huge allocations. */
      assert (size == encoded_bytes && width == 2 && height == 2);
      *page = (struct sanei_sp1130n_page) { malloc (12), 12, width, height };
      assert (page->data);
      memcpy (page->data, data, 12);
      return SANE_STATUS_GOOD;
    }
  (void) data;
  (void) size;
  (void) width;
  (void) height;
  (void) page;
  assert (!"mock sheets are already RGB");
  return SANE_STATUS_IO_ERROR;
}

SANE_Status
sanei_sp1130n_scan_finish (struct sanei_sp1130n_io *io,
                           const uint8_t token[40],
                           struct sanei_sp1130n_session *session)
{
  (void) io;
  (void) token;
  if (session->started || session->own_lock)
    {
      session->started = session->own_lock = 0;
      atomic_fetch_add (&finished_sessions, 1);
    }
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_sp1130n_usb_open (const char *name, struct sanei_sp1130n_usb **out)
{
  assert (!strcmp (name, "sp1130n:usb:001:002"));
  *out = calloc (1, sizeof **out);
  assert (*out);
  return SANE_STATUS_GOOD;
}

void
sanei_sp1130n_usb_close (struct sanei_sp1130n_usb *u)
{
  free (u);
}

struct sanei_sp1130n_io
sanei_sp1130n_usb_io (struct sanei_sp1130n_usb *u,
                      const atomic_bool *cancelled)
{
  return (struct sanei_sp1130n_io)
  {.context = u,.cancelled = cancelled };
}

SANE_Status
sanei_sp1130n_token (uint8_t token[40])
{
  memset (token, 0, 40);
  return SANE_STATUS_GOOD;
}

void
sanei_sp1130n_sheet_free (struct sanei_sp1130n_sheet *sheet)
{
  for (unsigned i = 0; i < 2; ++i)
    free (sheet->page[i].data);
  memset (sheet, 0, sizeof *sheet);
}

SANE_Status
sanei_sp1130n_scan_sheet (struct sanei_sp1130n_io *io,
                          const struct sanei_sp1130n_settings *s,
                          const uint8_t token[40],
                          struct sanei_sp1130n_sheet *sheet)
{
  (void) token;
  assert (io->context);
  if (fail_after && numbered_index == fail_after)
    return SANE_STATUS_JAMMED;
  if (numbered_count && numbered_index == numbered_count)
    return SANE_STATUS_NO_DOCS;
  unsigned sheet_number = numbered_count ? ++numbered_index : 0;
  ++scans;
  last_settings = *s;
  unsigned mask = blank_index < blank_count ? blank_masks[blank_index++] : 0;
  sheet->count = s->source == SP_SOURCE_DUPLEX ? 2 : 1;
  for (unsigned i = 0; i < sheet->count; ++i)
    {
      unsigned side = s->source == SP_SOURCE_BACK ? 1 : i;
      sheet->page[i] = (struct sanei_sp1130n_page)
      { malloc (12), 12, 2, 2 };
      assert (sheet->page[i].data);
      memset (sheet->page[i].data, side ? 42 : 17, 12);
      if (sheet_number)
        memset (sheet->page[i].data, 10 * sheet_number + side, 12);
      if (use_pattern)
        memcpy (sheet->page[i].data, side ? back_pattern : front_pattern, 12);
      if (mask & (1u << side))
        memset (sheet->page[i].data, 255, 12);
      if (malformed && i == 0)
        sheet->page[i].size = 11;
    }
  if (drain_handle)
    {
      SANE_Word value = SANE_TRUE;
      assert (sane_sp1130n_control_option
              (drain_handle, BATCH_SCAN, SANE_ACTION_SET_VALUE, &value, NULL)
              == SANE_STATUS_DEVICE_BUSY);
      if (cancel_after && numbered_index == cancel_after)
        sane_sp1130n_cancel (drain_handle);
    }
  return SANE_STATUS_GOOD;
}

static void
read_page (SANE_Handle h, unsigned byte)
{
  uint8_t data[5];
  SANE_Int length = 99;
  size_t count = 0;
  while (count < 12)
    {
      assert (sane_sp1130n_read (h, data, sizeof data, &length) ==
              SANE_STATUS_GOOD);
      assert (length > 0 && length <= 5);
      for (SANE_Int i = 0; i < length; ++i)
        assert (data[i] == byte);
      count += (size_t) length;
    }
  assert (sane_sp1130n_read (h, data, sizeof data, &length) == SANE_STATUS_EOF
          && length == 0);
}

static void
read_bytes (SANE_Handle h, const uint8_t *expected, size_t size)
{
  uint8_t bytes[5];
  size_t done = 0;
  SANE_Int length = 0;
  while (done < size)
    {
      assert (sane_sp1130n_read (h, bytes, sizeof bytes, &length) ==
              SANE_STATUS_GOOD);
      assert (length > 0 && (size_t) length <= size - done);
      assert (!memcmp (bytes, expected + done, (size_t) length));
      done += (size_t) length;
    }
  assert (sane_sp1130n_read (h, bytes, sizeof bytes, &length) ==
          SANE_STATUS_EOF && length == 0);
}

static void
idle_cancel_tests (void)
{
  const char *names[] = {
    "usb:001:002", "tcp:127.0.0.1:16888"
  };
  keep_session = 1;
  use_pattern = 0;
  for (unsigned transport = 0; transport < 2; ++transport)
    {
      SANE_Handle h;
      assert (sane_sp1130n_open (names[transport], &h) == SANE_STATUS_GOOD);
      assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
      read_page (h, 17);
      unsigned before = atomic_load (&finished_sessions);
      unsigned acquired = scans;
      sane_sp1130n_cancel (h);
      /* The GUI remains idle after cancel: do not make another backend call. */
      const struct timespec pause = { .tv_nsec = 10 * 1000 * 1000 };
      unsigned waited;
      for (waited = 0; waited < 100; ++waited)
        {
          if (atomic_load (&finished_sessions) != before)
            break;
          nanosleep (&pause, NULL);
        }
      assert (waited < 100);
      assert (transport ? scans >= acquired && scans <= acquired + 32 :
              scans == acquired);
      assert (atomic_load (&finished_sessions) == before + 1);
      assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
      read_page (h, 17);       /* A new sheet, not the cancelled buffered back. */
      sane_sp1130n_cancel (h);
      sane_sp1130n_close (h);  /* Close must wait for any outstanding cleanup. */
      assert (atomic_load (&finished_sessions) == before + 2);
    }
  keep_session = 0;
}

static void
set_word (SANE_Handle h, SANE_Int option, SANE_Word value)
{
  assert (sane_sp1130n_control_option
          (h, option, SANE_ACTION_SET_VALUE, &value,
           NULL) == SANE_STATUS_GOOD);
}

static void
wait_for_count (atomic_uint *counter, unsigned expected)
{
  const struct timespec pause = { .tv_nsec = 10 * 1000 * 1000 };
  for (unsigned waited = 0; waited < 100; ++waited)
    {
      if (atomic_load (counter) >= expected)
        return;
      nanosleep (&pause, NULL);
    }
  assert (!"acquisition worker did not make progress");
}

static void
prefetch_tests (void)
{
  keep_session = 1;
  use_pattern = 0;
  for (unsigned scenario = 0; scenario < 7; ++scenario)
    {
      numbered_count = scenario >= 5 ? 10 : 40;
      numbered_index = 0;
      fail_after = scenario == 1 ? 2 : 0;
      malformed = scenario == 4;
      encoded_bytes = scenario >= 5 ? (size_t) 32 * 1024 * 1024 : 0;
      unsigned before = scans;
      unsigned finished = atomic_load (&finished_sessions);
      SANE_Handle h;
      assert (sane_sp1130n_open ("tcp:127.0.0.1:16888", &h) ==
              SANE_STATUS_GOOD);
      assert (sane_sp1130n_start (h) ==
              (malformed ? SANE_STATUS_INVAL : SANE_STATUS_GOOD));
      if (malformed)
        {
          assert (sane_sp1130n_start (h) == SANE_STATUS_INVAL);
          assert (atomic_load (&finished_sessions) == finished + 1);
          sane_sp1130n_close (h);
          continue;
        }
      if (scenario == 1)
        wait_for_count (&finished_sessions, finished + 1);
      else
        {
          unsigned limit = encoded_bytes ? 4 : 33;
          wait_for_count (&scans, before + limit);
          /* Bound by sheet count or bytes, including one pending acquisition. */
          const struct timespec pause = { .tv_nsec = 150 * 1000 * 1000 };
          nanosleep (&pause, NULL);
          assert (scans == before + limit);
        }
      SANE_Word value = 1;
      assert (sane_sp1130n_control_option
              (h, 6, SANE_ACTION_SET_VALUE, &value, NULL) ==
              SANE_STATUS_DEVICE_BUSY);
      if (scenario == 2 || scenario == 3 || scenario == 6)
        {
          if (scenario != 3)
            {
#ifdef SP_BACKEND_TEST_SIGNAL
              signal_cancellation_test (h);
#else
              sane_sp1130n_cancel (h);
#endif
              wait_for_count (&finished_sessions, finished + 1);
            }
          sane_sp1130n_close (h);
          assert (scans == before + (encoded_bytes ? 4 : 33));
          assert (atomic_load (&finished_sessions) == finished + 1);
          continue;
        }
      unsigned sheets = scenario == 1 ? 2 : numbered_count;
      for (unsigned side = 0; side < sheets * 2; ++side)
        {
          if (side)
            assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
          read_page (h, (10 * (side / 2 + 1) + side % 2) % 256);
        }
      int terminal = scenario == 1 ? SANE_STATUS_JAMMED : SANE_STATUS_NO_DOCS;
      assert (sane_sp1130n_start (h) == terminal);
      assert (sane_sp1130n_start (h) == terminal);
      assert (scans == before + sheets);
      assert (atomic_load (&finished_sessions) == finished + 1);
      sane_sp1130n_close (h);
    }
  numbered_count = numbered_index = fail_after = 0;
  encoded_bytes = 0;
  malformed = keep_session = 0;
}

static void
processing_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  const SANE_Option_Descriptor *resolution =
    sane_sp1130n_get_option_descriptor (h, 3);
  assert (resolution && resolution->constraint_type == SANE_CONSTRAINT_WORD_LIST
          && resolution->constraint.word_list[0] == 2
          && resolution->constraint.word_list[1] == 300
          && resolution->constraint.word_list[2] == 600);
  for (SANE_Word trial = 50; trial <= 600; ++trial)
    {
      SANE_Word value = trial;
      assert (sane_sp1130n_control_option
              (h, 3, SANE_ACTION_SET_VALUE, &value, NULL) ==
              (trial == 300 || trial == 600 ?
               SANE_STATUS_GOOD : SANE_STATUS_INVAL));
    }
  SANE_Word unsupported = 1200;
  assert (sane_sp1130n_control_option
          (h, 3, SANE_ACTION_SET_VALUE, &unsupported, NULL) == SANE_STATUS_INVAL);
  set_word (h, 3, 300);
  use_pattern = 1;
  SANE_Word count;
  assert (sane_sp1130n_control_option
          (h, 0, SANE_ACTION_GET_VALUE, &count, NULL) == SANE_STATUS_GOOD);
  assert (count == 21);
  for (SANE_Int i = 1; i < count; ++i)
    assert (strcmp (sane_sp1130n_get_option_descriptor (h, i)->name,
                    "content-threshold"));
  assert (sane_sp1130n_get_option_descriptor (h, 8)->cap & SANE_CAP_INACTIVE);
  SANE_Word threshold = 128;
  assert (sane_sp1130n_control_option
          (h, 8, SANE_ACTION_SET_VALUE, &threshold,
           NULL) == SANE_STATUS_INVAL);
  SANE_Int info = 0;
  char mode[16] = "Gray";
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode, &info) == SANE_STATUS_GOOD);
  assert ((info & SANE_INFO_RELOAD_OPTIONS)
          && (info & SANE_INFO_RELOAD_PARAMS));
  assert (sane_sp1130n_get_option_descriptor (h, 8)->cap & SANE_CAP_INACTIVE);
  SANE_Parameters p;
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD);
  assert (p.format == SANE_FRAME_GRAY && p.depth == 8
          && p.bytes_per_line == 2480);
  unsigned before = scans;
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == before + 1);
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD);
  assert (p.depth == 8 && p.bytes_per_line == 2 && p.lines == 2);
  const uint8_t front_gray[] = { 77, 149, 29, 128 };
  const uint8_t back_gray[] = { 10, 20, 30, 40 };
  read_bytes (h, front_gray, sizeof front_gray);
  strcpy (mode, "Lineart");
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode,
           NULL) == SANE_STATUS_DEVICE_BUSY);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == before + 1);
  read_bytes (h, back_gray, sizeof back_gray);
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode, NULL) == SANE_STATUS_GOOD);
  assert (!
          (sane_sp1130n_get_option_descriptor (h, 8)->
           cap & SANE_CAP_INACTIVE));
  set_word (h, 8, 128);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD);
  assert (p.format == SANE_FRAME_GRAY && p.depth == 1
          && p.bytes_per_line == 1);
  const uint8_t front_bits[] = { 0x80, 0x80 }, back_bits[] = { 0xc0, 0xc0 };
  read_bytes (h, front_bits, sizeof front_bits);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, back_bits, sizeof back_bits);
  set_word (h, 6, 100);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  const uint8_t white_bits[] = { 0, 0 };
  read_bytes (h, white_bits, sizeof white_bits);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, white_bits, sizeof white_bits);
  set_word (h, 6, 0);
  strcpy (mode, "Color");
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode, NULL) == SANE_STATUS_GOOD);
  set_word (h, 9, SANE_TRUE);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, front_pattern, sizeof front_pattern);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  const uint8_t rotated_back[] =
    { 40, 40, 40, 30, 30, 30, 20, 20, 20, 10, 10, 10 };
  read_bytes (h, rotated_back, sizeof rotated_back);
  char source[32] = "ADF Front";
  assert (sane_sp1130n_control_option
          (h, 2, SANE_ACTION_SET_VALUE, source, NULL) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, front_pattern, sizeof front_pattern);
  strcpy (source, "ADF Back");
  assert (sane_sp1130n_control_option
          (h, 2, SANE_ACTION_SET_VALUE, source, NULL) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, rotated_back, sizeof rotated_back);
  for (SANE_Int option = 6; option <= 7; ++option)
    {
      SANE_Word invalid = 101;
      assert (sane_sp1130n_control_option
              (h, option, SANE_ACTION_SET_VALUE, &invalid,
               NULL) == SANE_STATUS_INVAL);
    }
  SANE_Word invalid_boolean = 2;
  assert (sane_sp1130n_control_option
          (h, 9, SANE_ACTION_SET_VALUE, &invalid_boolean,
           NULL) == SANE_STATUS_INVAL);
  strcpy (mode, "Unknown");
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode, NULL) == SANE_STATUS_INVAL);
  malformed = 1;
  assert (sane_sp1130n_start (h) == SANE_STATUS_INVAL);
  uint8_t byte;
  SANE_Int length = 99;
  assert (sane_sp1130n_read (h, &byte, 1, &length) == SANE_STATUS_INVAL
          && length == 0);
  malformed = 0;
  sane_sp1130n_close (h);
}

static void
geometry_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  set_word (h, TL_X, SANE_FIX (10));
  set_word (h, TL_Y, SANE_FIX (20));
  set_word (h, BR_X, SANE_FIX (110));
  set_word (h, BR_Y, SANE_FIX (70));
  SANE_Parameters p;
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD);
  assert (p.pixels_per_line == 1181 && p.lines == 590
          && p.bytes_per_line == 3543);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (last_settings.left_mm == 10 && last_settings.top_mm == 20 &&
          last_settings.scan_width_mm == 100
          && last_settings.scan_height_mm == 50);
  SANE_Word v = SANE_FIX (5);
  assert (sane_sp1130n_control_option
          (h, TL_X, SANE_ACTION_SET_VALUE, &v,
           NULL) == SANE_STATUS_DEVICE_BUSY);
  sane_sp1130n_cancel (h);
  set_word (h, TL_X, SANE_FIX (150));
  unsigned before = scans;
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_INVAL);
  assert (sane_sp1130n_start (h) == SANE_STATUS_INVAL && scans == before);
  v = SANE_FIX (216);
  assert (sane_sp1130n_control_option
          (h, BR_X, SANE_ACTION_SET_VALUE, &v, NULL) == SANE_STATUS_INVAL);
  assert (sane_sp1130n_start (h) == SANE_STATUS_INVAL && scans == before);
  set_word (h, BR_X, SANE_FIX (200));
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD
          && p.pixels_per_line == 590);
  v = SANE_FIX (20.5);
  SANE_Int info = 0;
  assert (sane_sp1130n_control_option
          (h, TL_X, SANE_ACTION_SET_VALUE, &v, &info) == SANE_STATUS_GOOD);
  assert (v == SANE_FIX (21) && (info & SANE_INFO_INEXACT));
  v = SANE_FIX (217);
  assert (sane_sp1130n_control_option
          (h, BR_X, SANE_ACTION_SET_VALUE, &v, NULL) == SANE_STATUS_INVAL);
  sane_sp1130n_close (h);
}

static SANE_Word
get_word (SANE_Handle h, SANE_Int option)
{
  SANE_Word value;
  assert (sane_sp1130n_control_option
          (h, option, SANE_ACTION_GET_VALUE, &value, NULL) == SANE_STATUS_GOOD);
  return value;
}

static void
page_resize_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  for (unsigned explicit_edge = 0; explicit_edge < 2; ++explicit_edge)
    {
      set_word (h, 4, SANE_FIX (210));
      set_word (h, 5, SANE_FIX (297));
      if (explicit_edge)
        {
          set_word (h, BR_X, SANE_FIX (200));
          set_word (h, BR_Y, SANE_FIX (290));
        }
      set_word (h, TL_X, SANE_FIX (190));
      set_word (h, TL_Y, SANE_FIX (280));
      for (SANE_Int option = 4; option <= 5; ++option)
        {
          SANE_Word value = SANE_FIX (100);
          SANE_Int info = 0;
          assert (sane_sp1130n_control_option
                  (h, option, SANE_ACTION_SET_VALUE, &value, &info)
                  == SANE_STATUS_GOOD);
          assert (info == (SANE_INFO_RELOAD_OPTIONS | SANE_INFO_RELOAD_PARAMS));
        }
      assert (get_word (h, BR_X) == SANE_FIX (100)
              && get_word (h, BR_Y) == SANE_FIX (100));
      assert (get_word (h, TL_X) == SANE_FIX (99)
              && get_word (h, TL_Y) == SANE_FIX (99));
      assert (sane_sp1130n_get_option_descriptor (h, BR_X)->constraint.range->max
              == SANE_FIX (100));
      assert (sane_sp1130n_get_option_descriptor (h, BR_Y)->constraint.range->max
              == SANE_FIX (100));
      SANE_Parameters p;
      assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD
              && p.pixels_per_line == 11 && p.lines == 11);
    }
  sane_sp1130n_close (h);
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  set_word (h, 4, SANE_FIX (216));
  SANE_Parameters p;
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD
          && p.pixels_per_line == 2550);
  set_word (h, 3, 600);
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD
          && p.pixels_per_line == 5100);
  sane_sp1130n_close (h);
}

static void
document_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  use_pattern = 1;
  set_word (h, AUTOCROP, SANE_TRUE);
  set_word (h, CROP_MARGIN, 0);
  SANE_Parameters p;
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD
          && p.lines == -1);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD &&
          p.pixels_per_line == 2 && p.lines == 2 && p.bytes_per_line == 6);
  /* A two-pixel fixture has no detectable media edges; do not crop content. */
  read_bytes (h, front_pattern, sizeof front_pattern);
  SANE_Word value = 0;
  assert (sane_sp1130n_control_option
          (h, AUTOCROP, SANE_ACTION_SET_VALUE, &value,
           NULL) == SANE_STATUS_DEVICE_BUSY);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, back_pattern, sizeof back_pattern);
  set_word (h, AUTOCROP, SANE_FALSE);
  set_word (h, DESKEW, SANE_TRUE);
  set_word (h, SKIP_BLANK, SANE_TRUE);
  set_word (h, 9, SANE_TRUE);
  blank_masks[0] = 1;
  blank_count = 1;
  blank_index = 0;
  unsigned before = scans;
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == before + 1);
  const uint8_t rotated_back[] =
    { 40, 40, 40, 30, 30, 30, 20, 20, 20, 10, 10, 10 };
  read_bytes (h, rotated_back, sizeof rotated_back);
  blank_masks[0] = 3;
  blank_masks[1] = 2;
  blank_count = 2;
  blank_index = 0;
  before = scans;
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == before + 2);
  read_bytes (h, front_pattern, sizeof front_pattern);
  for (unsigned i = 0; i < 32; ++i)
    blank_masks[i] = 3;
  blank_count = 32;
  blank_index = 0;
  before = scans;
  assert (sane_sp1130n_start (h) == SANE_STATUS_IO_ERROR
          && scans == before + 32);
  uint8_t byte;
  SANE_Int length = 99;
  assert (sane_sp1130n_read (h, &byte, 1, &length) == SANE_STATUS_INVAL
          && !length);
  blank_count = blank_index = 0;
  for (SANE_Int option = AUTOCROP; option <= SKIP_BLANK; ++option)
    {
      value = 2;
      assert (sane_sp1130n_control_option
              (h, option, SANE_ACTION_SET_VALUE, &value,
               NULL) == SANE_STATUS_INVAL);
    }
  for (SANE_Int option = BLANK_FRACTION; option <= CROP_MARGIN; ++option)
    {
      value =
        option == BLANK_FRACTION ? 1001 : 10001;
      assert (sane_sp1130n_control_option
              (h, option, SANE_ACTION_SET_VALUE, &value,
               NULL) == SANE_STATUS_INVAL);
    }
  sane_sp1130n_close (h);
}

static void
native_batch_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  use_pattern = 0;
  numbered_count = 2;
  numbered_index = 0;
  unsigned before = scans;
  for (unsigned page = 0; page < 4; ++page)
    {
      assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
      assert (scans == before + page / 2 + 1);
      read_page (h, 10 * (page / 2 + 1) + page % 2);
    }
  assert (sane_sp1130n_start (h) == SANE_STATUS_NO_DOCS);
  assert (sane_sp1130n_start (h) == SANE_STATUS_NO_DOCS);
  assert (scans == before + 2);
  uint8_t byte;
  SANE_Int length = 99;
  assert (sane_sp1130n_read (h, &byte, 1, &length) == SANE_STATUS_INVAL
          && length == 0);
  sane_sp1130n_close (h);
  numbered_count = numbered_index = 0;
}

static void
first_sheet_tests (void)
{
  const char *names[] = { "usb:001:002", "tcp:127.0.0.1:16888" };
  const char *sources[] = { "ADF Duplex", "ADF Front", "ADF Back" };
  keep_session = 1;
  use_pattern = 0;
  for (unsigned transport = 0; transport < 2; ++transport)
    for (unsigned source = 0; source < 3; ++source)
      {
        SANE_Handle h;
        assert (sane_sp1130n_open (names[transport], &h) == SANE_STATUS_GOOD);
        assert (get_word (h, BATCH_SCAN) == SANE_TRUE);
        const SANE_Option_Descriptor *option =
          sane_sp1130n_get_option_descriptor (h, BATCH_SCAN);
        assert (!strcmp (option->name, "batch-scan")
                && option->type == SANE_TYPE_BOOL
                && !(option->cap & SANE_CAP_INACTIVE));
        SANE_Word invalid = 2;
        assert (sane_sp1130n_control_option
                (h, BATCH_SCAN, SANE_ACTION_SET_VALUE, &invalid, NULL)
                == SANE_STATUS_INVAL);
        set_word (h, BATCH_SCAN, SANE_FALSE);
        assert (sane_sp1130n_control_option
                (h, 2, SANE_ACTION_SET_VALUE, (void *) sources[source], NULL)
                == SANE_STATUS_GOOD);
        numbered_count = 3;
        numbered_index = 0;
        unsigned before = scans, finished = atomic_load (&finished_sessions);
        drain_handle = h;
        assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
        drain_handle = NULL;
        assert (scans == before + 3 && numbered_index == 3
                && atomic_load (&finished_sessions) == finished + 1);
        read_page (h, source == 2 ? 11 : 10);
        if (!source)
          {
            invalid = SANE_TRUE;
            assert (sane_sp1130n_control_option
                    (h, BATCH_SCAN, SANE_ACTION_SET_VALUE, &invalid, NULL)
                    == SANE_STATUS_DEVICE_BUSY);
            assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
            read_page (h, 11);
          }
        assert (sane_sp1130n_start (h) == SANE_STATUS_NO_DOCS);
        assert (sane_sp1130n_start (h) == SANE_STATUS_NO_DOCS);
        assert (scans == before + 3);
        /* Explicit cancellation/reload starts a new first-sheet job. */
        sane_sp1130n_cancel (h);
        numbered_count = 1;
        numbered_index = 0;
        assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
        read_page (h, source == 2 ? 11 : 10);
        sane_sp1130n_close (h);
      }

  for (unsigned transport = 0; transport < 2; ++transport)
    for (unsigned scenario = 0; scenario < 5; ++scenario)
      {
        SANE_Handle h;
        assert (sane_sp1130n_open (names[transport], &h) == SANE_STATUS_GOOD);
        set_word (h, BATCH_SCAN, SANE_FALSE);
        numbered_count = scenario == 3 ? 101 : 3;
        numbered_index = scenario == 0 ? numbered_count : 0;
        fail_after = scenario == 1 ? 1 : 0;
        cancel_after = scenario == 2 ? 2 : 0;
        if (scenario == 4)
          {
            set_word (h, SKIP_BLANK, SANE_TRUE);
            blank_masks[0] = 3;
            blank_count = 1;
            blank_index = 0;
          }
        SANE_Status expected = scenario == 1 ? SANE_STATUS_JAMMED :
          scenario == 2 ? SANE_STATUS_CANCELLED :
          scenario == 3 ? SANE_STATUS_IO_ERROR : SANE_STATUS_NO_DOCS;
        unsigned before = scans, finished = atomic_load (&finished_sessions);
        drain_handle = h;
        assert (sane_sp1130n_start (h) == expected);
        drain_handle = NULL;
        unsigned acquired = scenario == 0 ? 0 : scenario == 1 ? 1 :
          scenario == 2 ? 2 : scenario == 3 ? 100 : 3;
        assert (scans == before + acquired);
        assert (atomic_load (&finished_sessions) == finished + (acquired != 0));
        uint8_t byte = 0xa5;
        SANE_Int length = 99;
        assert (sane_sp1130n_read (h, &byte, 1, &length) ==
                (scenario == 2 ? SANE_STATUS_CANCELLED : SANE_STATUS_INVAL)
                && !length && byte == 0xa5);
        if (scenario != 2)
          assert (sane_sp1130n_start (h) == expected && scans == before + acquired);
        sane_sp1130n_close (h);
        fail_after = cancel_after = blank_count = blank_index = 0;
      }
  numbered_count = numbered_index = 0;
  keep_session = 0;
}

static void
upright_loading_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  use_pattern = 1;
  SANE_Word value = SANE_TRUE;
  assert (sane_sp1130n_control_option
          (h, UPRIGHT_LOADING, SANE_ACTION_GET_VALUE, &value, NULL) ==
          SANE_STATUS_GOOD && value == SANE_FALSE);
  assert (!strcmp (sane_sp1130n_get_option_descriptor
                   (h, UPRIGHT_LOADING)->name, "upright-loading"));
  assert (!(sane_sp1130n_get_option_descriptor
            (h, UPRIGHT_LOADING)->cap & SANE_CAP_INACTIVE));
  set_word (h, UPRIGHT_LOADING, SANE_TRUE);
  value = 2;
  assert (sane_sp1130n_control_option
          (h, UPRIGHT_LOADING, SANE_ACTION_SET_VALUE, &value, NULL) ==
          SANE_STATUS_INVAL);
  const uint8_t rotated_front[] =
    { 128, 128, 128, 0, 0, 255, 0, 255, 0, 255, 0, 0 };
  const uint8_t rotated_back[] =
    { 40, 40, 40, 30, 30, 30, 20, 20, 20, 10, 10, 10 };
  unsigned before = scans;
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == before + 1);
  read_bytes (h, rotated_back, sizeof rotated_back);
  value = SANE_FALSE;
  assert (sane_sp1130n_control_option
          (h, UPRIGHT_LOADING, SANE_ACTION_SET_VALUE, &value, NULL) ==
          SANE_STATUS_DEVICE_BUSY);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == before + 1);
  read_bytes (h, rotated_front, sizeof rotated_front);
  set_word (h, 9, SANE_TRUE);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, rotated_back, sizeof rotated_back);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, front_pattern, sizeof front_pattern);
  char source[32] = "ADF Front";
  assert (sane_sp1130n_control_option
          (h, 2, SANE_ACTION_SET_VALUE, source, NULL) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (last_settings.source == SP_SOURCE_BACK);
  read_bytes (h, rotated_back, sizeof rotated_back);
  strcpy (source, "ADF Back");
  assert (sane_sp1130n_control_option
          (h, 2, SANE_ACTION_SET_VALUE, source, NULL) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (last_settings.source == SP_SOURCE_FRONT);
  read_bytes (h, front_pattern, sizeof front_pattern);
  set_word (h, 9, SANE_FALSE);
  strcpy (source, "ADF Duplex");
  assert (sane_sp1130n_control_option
          (h, 2, SANE_ACTION_SET_VALUE, source, NULL) == SANE_STATUS_GOOD);
  char mode[16] = "Gray";
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode, NULL) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  const uint8_t back_gray[] = { 40, 30, 20, 10 };
  const uint8_t front_gray[] = { 128, 29, 149, 77 };
  read_bytes (h, back_gray, sizeof back_gray);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, front_gray, sizeof front_gray);
  strcpy (mode, "Lineart");
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode, NULL) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  const uint8_t back_bits[] = { 0xc0, 0xc0 }, front_bits[] = { 0x40, 0x40 };
  read_bytes (h, back_bits, sizeof back_bits);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, front_bits, sizeof front_bits);
  strcpy (mode, "Color");
  assert (sane_sp1130n_control_option
          (h, 1, SANE_ACTION_SET_VALUE, mode, NULL) == SANE_STATUS_GOOD);
  set_word (h, TL_X, SANE_FIX (10));
  set_word (h, TL_Y, SANE_FIX (20));
  set_word (h, BR_X, SANE_FIX (110));
  set_word (h, BR_Y, SANE_FIX (70));
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (last_settings.left_mm == 100 && last_settings.top_mm == 227 &&
          last_settings.scan_width_mm == 100 &&
          last_settings.scan_height_mm == 50);
  read_bytes (h, rotated_back, sizeof rotated_back);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, rotated_front, sizeof rotated_front);
  set_word (h, SKIP_BLANK, SANE_TRUE);
  set_word (h, 9, SANE_TRUE);
  blank_masks[0] = 1;
  blank_count = 1;
  blank_index = 0;
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, rotated_back, sizeof rotated_back);
  blank_masks[0] = 2;
  blank_index = 0;
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, front_pattern, sizeof front_pattern);
  blank_count = blank_index = 0;
  set_word (h, SKIP_BLANK, SANE_FALSE);
  set_word (h, UPRIGHT_LOADING, SANE_FALSE);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (last_settings.left_mm == 10 && last_settings.top_mm == 20);
  read_bytes (h, front_pattern, sizeof front_pattern);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, rotated_back, sizeof rotated_back);
  sane_sp1130n_close (h);
}

int
main (void)
{
#ifdef SP_BACKEND_TEST_SIGNAL
  signal_thread = pthread_self ();
#endif
  SANE_Int version;
  assert (sane_sp1130n_init (&version, NULL) == SANE_STATUS_GOOD);
  assert (SANE_VERSION_MAJOR (version) == 1);
  const SANE_Device **devices;
  assert (sane_sp1130n_get_devices (&devices, SANE_TRUE) == SANE_STATUS_GOOD);
  assert (devices[0] && !devices[1]
          && !strcmp (devices[0]->name, "usb:001:002")
          && !strcmp (devices[0]->vendor, "Fujitsu"));
  SANE_Handle h = NULL;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  SANE_Word upright = SANE_TRUE;
  assert (sane_sp1130n_control_option
          (h, UPRIGHT_LOADING, SANE_ACTION_GET_VALUE, &upright, NULL) ==
          SANE_STATUS_GOOD && upright == SANE_FALSE);
  assert (devices[0] && !devices[1]
          && !strcmp (devices[0]->name, "usb:001:002"));
  SANE_Handle other;
  assert (sane_sp1130n_open ("", &other) == SANE_STATUS_DEVICE_BUSY);
  assert (unsetenv ("SP1130N_EXPERIMENTAL") == 0);
  assert (scans == 0);
  char source[32];
  assert (sane_sp1130n_control_option
          (h, 2, SANE_ACTION_GET_VALUE, source, NULL) == SANE_STATUS_GOOD);
  assert (!strcmp (source, "ADF Duplex"));
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == 1);
  SANE_Parameters p;
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD);
  assert (p.last_frame && p.format == SANE_FRAME_RGB && p.depth == 8
          && p.bytes_per_line == 6);
  assert (sane_sp1130n_start (h) == SANE_STATUS_DEVICE_BUSY);
  read_page (h, 17);
  SANE_Word dpi = 600;
  assert (sane_sp1130n_control_option
          (h, 3, SANE_ACTION_SET_VALUE, &dpi,
           NULL) == SANE_STATUS_DEVICE_BUSY);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == 1);
  read_page (h, 42);
  assert (sane_sp1130n_control_option
          (h, 3, SANE_ACTION_SET_VALUE, &dpi, NULL) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == 2);
  sane_sp1130n_cancel (h);
  uint8_t data[10];
  SANE_Int length = 9;
  assert (sane_sp1130n_read (h, data, 10, &length) == SANE_STATUS_CANCELLED
          && length == 0);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD && scans == 3);
  read_page (h, 17);
  sane_sp1130n_cancel (h);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
#ifdef SP_BACKEND_TEST_SIGNAL
  signal_cancellation_test (h);
#else
  sane_sp1130n_cancel (h);
#endif
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_page (h, 17);
  sane_sp1130n_close (h);
  processing_tests ();
  geometry_tests ();
  page_resize_tests ();
  document_tests ();
  native_batch_tests ();
  first_sheet_tests ();
  upright_loading_tests ();
  idle_cancel_tests ();
  prefetch_tests ();
  discovery_tests ();
  sane_sp1130n_exit ();
  puts
    ("SANE tests passed: native batch and bounded first-sheet drain, upright/native loading, duplex/front/back, ROI, image controls, crop, blank filtering, EOF, idle cancellation and device-list lifetime.");
  return 0;
}
