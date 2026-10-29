/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "sp1130n.h"
#include "sp1130n-image.h"
#include "../../../include/sane/sane.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static unsigned scans;
static int use_pattern, malformed;
static unsigned blank_masks[32], blank_count, blank_index;
static struct sanei_sp1130n_settings last_settings;
#ifdef SP_BACKEND_TEST_SIGNAL
static SANE_Handle signal_handle;
static volatile sig_atomic_t signal_armed;

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
  if (!status && signal_armed)
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
  CONTENT_THRESHOLD, BLANK_FRACTION, CROP_MARGIN
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
int
sanei_sp1130n_usb_list (struct sanei_sp1130n_device **out, size_t *count)
{
  *out = calloc (1, sizeof **out);
  assert (*out);
  strcpy ((*out)->name, "sp1130n:usb:001:002");
  *count = 1;
  return SP_OK;
}

int
sanei_sp1130n_scan_next (struct sanei_sp1130n_io *io,
                         const struct sanei_sp1130n_settings *s,
                         const uint8_t token[40],
                         struct sanei_sp1130n_session *session,
                         struct sanei_sp1130n_sheet *sheet)
{
  (void) session;
  return sanei_sp1130n_scan_sheet (io, s, token, sheet);
}

int
sanei_sp1130n_scan_finish (struct sanei_sp1130n_io *io,
                           const uint8_t token[40],
                           struct sanei_sp1130n_session *session)
{
  (void) io;
  (void) token;
  (void) session;
  return SP_OK;
}

int
sanei_sp1130n_usb_open (const char *name, struct sanei_sp1130n_usb **out)
{
  assert (!strcmp (name, "sp1130n:usb:001:002"));
  *out = calloc (1, sizeof **out);
  assert (*out);
  return SP_OK;
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

int
sanei_sp1130n_token (uint8_t token[40])
{
  memset (token, 0, 40);
  return SP_OK;
}

void
sanei_sp1130n_sheet_free (struct sanei_sp1130n_sheet *sheet)
{
  for (unsigned i = 0; i < 2; ++i)
    free (sheet->page[i].data);
  memset (sheet, 0, sizeof *sheet);
}

int
sanei_sp1130n_scan_sheet (struct sanei_sp1130n_io *io,
                          const struct sanei_sp1130n_settings *s,
                          const uint8_t token[40],
                          struct sanei_sp1130n_sheet *sheet)
{
  (void) token;
  assert (io->context);
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
      if (use_pattern)
        memcpy (sheet->page[i].data, side ? back_pattern : front_pattern, 12);
      if (mask & (1u << side))
        memset (sheet->page[i].data, 255, 12);
      if (malformed && i == 0)
        sheet->page[i].size = 11;
    }
  return SP_OK;
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
set_word (SANE_Handle h, SANE_Int option, SANE_Word value)
{
  assert (sane_sp1130n_control_option
          (h, option, SANE_ACTION_SET_VALUE, &value,
           NULL) == SANE_STATUS_GOOD);
}

static void
processing_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  use_pattern = 1;
  SANE_Word count;
  assert (sane_sp1130n_control_option
          (h, 0, SANE_ACTION_GET_VALUE, &count, NULL) == SANE_STATUS_GOOD);
  assert (count == 20);
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
  set_word (h, BR_X, SANE_FIX (216));
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

static void
document_tests (void)
{
  SANE_Handle h;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  use_pattern = 1;
  set_word (h, AUTOCROP, SANE_TRUE);
  set_word (h, CONTENT_THRESHOLD, 100);
  set_word (h, CROP_MARGIN, 0);
  SANE_Parameters p;
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD
          && p.lines == -1);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  assert (sane_sp1130n_get_parameters (h, &p) == SANE_STATUS_GOOD &&
          p.pixels_per_line == 1 && p.lines == 2 && p.bytes_per_line == 3);
  const uint8_t cropped[] = { 255, 0, 0, 0, 0, 255 };
  read_bytes (h, cropped, sizeof cropped);
  SANE_Word value = 0;
  assert (sane_sp1130n_control_option
          (h, AUTOCROP, SANE_ACTION_SET_VALUE, &value,
           NULL) == SANE_STATUS_DEVICE_BUSY);
  assert (sane_sp1130n_start (h) == SANE_STATUS_GOOD);
  read_bytes (h, back_pattern, sizeof back_pattern);
  set_word (h, AUTOCROP, SANE_FALSE);
  set_word (h, DESKEW, SANE_TRUE);
  set_word (h, SKIP_BLANK, SANE_TRUE);
  set_word (h, CONTENT_THRESHOLD, 240);
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
  for (SANE_Int option = CONTENT_THRESHOLD; option <= CROP_MARGIN; ++option)
    {
      value =
        option == CONTENT_THRESHOLD ? 256 : option ==
        BLANK_FRACTION ? 1001 : 10001;
      assert (sane_sp1130n_control_option
              (h, option, SANE_ACTION_SET_VALUE, &value,
               NULL) == SANE_STATUS_INVAL);
    }
  sane_sp1130n_close (h);
}

int
main (void)
{
  SANE_Int version;
  assert (sane_sp1130n_init (&version, NULL) == SANE_STATUS_GOOD);
  assert (SANE_VERSION_MAJOR (version) == 1);
  const SANE_Device **devices;
  assert (sane_sp1130n_get_devices (&devices, SANE_TRUE) == SANE_STATUS_GOOD);
  assert (devices[0] && !devices[1]
          && !strcmp (devices[0]->name, "usb:001:002"));
  SANE_Handle h = NULL;
  assert (sane_sp1130n_open ("", &h) == SANE_STATUS_GOOD);
  assert (devices[0] && !devices[1]
          && !strcmp (devices[0]->name, "usb:001:002"));
  SANE_Handle other;
  assert (sane_sp1130n_open ("", &other) == SANE_STATUS_DEVICE_BUSY);
  assert (unsetenv ("SP1130N_EXPERIMENTAL") == 0);
  assert (sane_sp1130n_start (h) == SANE_STATUS_UNSUPPORTED && scans == 0);
  assert (setenv ("SP1130N_EXPERIMENTAL", "1", 1) == 0);
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
  document_tests ();
  sane_sp1130n_exit ();
  puts
    ("SANE tests passed: duplex/front/back, ROI, image controls, crop, blank filtering, bounded feed, EOF, cancellation and device-list lifetime.");
  return 0;
}
