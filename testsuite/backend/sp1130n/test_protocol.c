/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "sp1130n.h"
#include "../../../include/sane/sane.h"
#include <assert.h>
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

static int fail_realloc;
void *__real_realloc (void *, size_t);
void *
__wrap_realloc (void *data, size_t size)
{
  if (fail_realloc && !--fail_realloc)
    return NULL;
  return __real_realloc (data, size);
}

struct event
{
  int input, status;
  uint8_t *data;
  size_t size, done;
  int cancel_after;
  size_t cancel_at;
  size_t error_bytes;
  unsigned min_timeout, max_timeout;
};
struct replay
{
  struct event events[256];
  size_t count, index;
  atomic_bool cancelled;
  size_t chunk;
  int recover_enabled, recover_failure;
  unsigned recovery_calls;
  size_t expected_tail;
  int fail_closed, blocked;
  size_t wire_calls, blocked_calls;
  int image_recover_enabled, image_recover_failure;
  unsigned image_recovery_calls, image_commits;
  size_t observe_boundary;
  atomic_bool boundary_reached;
};

static struct replay *cancel_during_pause;
static unsigned pause_slices;
int __real_nanosleep (const struct timespec *, struct timespec *);
int
__wrap_nanosleep (const struct timespec *request, struct timespec *remaining)
{
  if (cancel_during_pause)
    {
      assert (!request->tv_sec && request->tv_nsec == 10 * 1000 * 1000);
      assert (!pause_slices && !atomic_load (&cancel_during_pause->cancelled));
    }
  int status = __real_nanosleep (request, remaining);
  if (cancel_during_pause)
    {
      ++pause_slices;
      atomic_store (&cancel_during_pause->cancelled, 1);
    }
  return status;
}

static void
append (struct replay *r, int input, const uint8_t *data, size_t size)
{
  assert (r->count < sizeof r->events / sizeof r->events[0]);
  struct event *e = &r->events[r->count++];
  *e = (struct event)
  {.input = input,.size = size };
  e->data = malloc (size ? size : 1);
  assert (e->data);
  if (size)
    memcpy (e->data, data, size);
}

static int
transfer (void *context, int input, uint8_t *data, size_t size,
          size_t *actual, unsigned timeout)
{
  struct replay *r = context;
  assert (timeout);
  *actual = 0;
  if (r->blocked)
    {
      ++r->blocked_calls;
      return SP_IO;
    }
  ++r->wire_calls;
  if (r->index == r->count)
    return SP_IO;
  struct event *e = &r->events[r->index];
  assert (e->input == input);
  assert (!e->min_timeout || timeout >= e->min_timeout);
  assert (!e->max_timeout || timeout <= e->max_timeout);
  assert (size <= e->size - e->done || e->status);
  if (e->status)
    {
      assert (e->error_bytes <= size && e->error_bytes <= e->size);
      if (e->error_bytes)
        {
          assert (input);
          memcpy (data, e->data, e->error_bytes);
          *actual = e->error_bytes;
        }
      if (r->fail_closed)
        {
          r->blocked = 1;
          e->done += e->error_bytes;
          if (e->done == e->size)
            ++r->index;
        }
      else
        ++r->index;
      return e->status;
    }
  size_t n = size;
  if (r->chunk && n > r->chunk)
    n = r->chunk;
  if (input)
    memcpy (data, e->data + e->done, n);
  else
    assert (!memcmp (data, e->data + e->done, n));
  *actual = n;
  e->done += n;
  if (e->cancel_at && e->done >= e->cancel_at)
    atomic_store (&r->cancelled, 1);
  if (e->done == e->size)
    {
      ++r->index;
      if (e->cancel_after)
        atomic_store (&r->cancelled, 1);
      if (r->observe_boundary && r->index == r->observe_boundary)
        atomic_store (&r->boundary_reached, 1);
    }
  return SP_OK;
}

static int
recover_inquiry_reply (void *context, uint8_t *data, size_t remaining,
                       unsigned timeout)
{
  struct replay *r = context;
  ++r->recovery_calls;
  assert (remaining == r->expected_tail && timeout == 1000);
  if (r->recover_failure)
    return SP_IO;
  struct sanei_sp1130n_io io = { .context = r, .bulk = transfer };
  return sanei_sp1130n_exact (&io, 1, data, remaining, timeout);
}

static int
recover_image_reply (void *context, uint8_t *data, size_t remaining,
                     unsigned timeout)
{
  struct replay *r = context;
  assert (r->blocked && remaining <= SP_COMPRESSED_LIMIT && timeout == 5000);
  if (++r->image_recovery_calls == (unsigned) r->image_recover_failure)
    return SP_IO;
  while (remaining)
    {
      assert (r->index < r->count);
      struct event *e = &r->events[r->index];
      assert (e->input && e->size > e->done);
      size_t size = e->size - e->done;
      if (size > remaining)
        size = remaining;
      if (data)
        {
          memcpy (data, e->data + e->done, size);
          data += size;
        }
      e->done += size;
      remaining -= size;
      ++r->wire_calls;
      if (e->done == e->size)
        ++r->index;
    }
  return SP_OK;
}

static int
image_recovery_commit (void *context, int complete)
{
  struct replay *r = context;
  assert (r->blocked && r->image_recovery_calls);
  ++r->image_commits;
  r->blocked = !complete;
  return complete ? SP_OK : SP_IO;
}

static void
release (struct replay *r)
{
  assert (r->index == r->count);
  for (size_t i = 0; i < r->count; ++i)
    free (r->events[i].data);
}

static void
command (struct replay *r, uint8_t op, uint8_t sub, uint16_t sel,
         uint32_t value, const uint8_t *reply, size_t size)
{
  uint8_t h[16];
  sanei_sp1130n_header (h, op, sub, sel, value);
  append (r, 0, h, 16);
  if (size)
    append (r, 1, reply, size);
}

static void
ack (struct replay *r, uint8_t op, uint8_t sub, uint16_t sel, uint32_t value)
{
  uint8_t reply[32] = { 0 };
  command (r, op, sub, sel, value, reply, sizeof reply);
}

static void
startup (struct replay *r, const struct sanei_sp1130n_settings *s,
         const uint8_t token[40])
{
  uint8_t reply[108] = { 0 }, params[SP_PARAM_SIZE];
  command (r, 0x12, 0x7a, 108, 0, reply, 108);
  ack (r, 0x2a, 0x78, 1, 40);
  append (r, 0, token, 40);
  ack (r, 0x28, 0x78, 1, 40);
  append (r, 1, token, 40);
  sanei_sp1130n_put16 (reply + 8, 2);
  command (r, 0x12, 2, 32, 0, reply, 32);
  assert (sanei_sp1130n_parameters (params, s) == SP_OK);
  ack (r, 0x2a, 9, 4, SP_PARAM_SIZE);
  append (r, 0, params, sizeof params);
  ack (r, 0x14, 5, 0, 0);
  ack (r, 0x14, 4, 0, 0);
}

static void
cleanup (struct replay *r, const uint8_t token[40])
{
  ack (r, 0x14, 6, 0, 0);
  ack (r, 0x2a, 0x78, 2, 40);
  append (r, 0, token, 40);
}

static void
block (struct replay *r, unsigned side, const uint8_t *jpeg, size_t size,
       uint32_t error, unsigned width, unsigned height)
{
  uint8_t h[64] = { 0 };
  sanei_sp1130n_put32 (h, error);
  h[11] = side ? 0 : 1;
  sanei_sp1130n_put32 (h + 16, height);
  sanei_sp1130n_put32 (h + 20, width);
  sanei_sp1130n_put32 (h + 36, (uint32_t) size);
  h[48] = 1;
  command (r, 0x28, 4, 0, 0, h, sizeof h);
  if (size)
    append (r, 1, jpeg, size);
}

static uint8_t *
jpeg (unsigned side, size_t *size)
{
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr error;
  c.err = jpeg_std_error (&error);
  jpeg_create_compress (&c);
  unsigned char *data = NULL;
  unsigned long length = 0;
  jpeg_mem_dest (&c, &data, &length);
  c.image_width = 2;
  c.image_height = 2;
  c.input_components = 3;
  c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_set_quality (&c, 95, TRUE);
  c.restart_in_rows = 1;
  jpeg_start_compress (&c, TRUE);
  uint8_t row[6] = { 0 };
  row[side ? 2 : 0] = row[side ? 5 : 3] = 240;
  JSAMPROW rows[] = { row };
  while (c.next_scanline < c.image_height)
    jpeg_write_scanlines (&c, rows, 1);
  jpeg_finish_compress (&c);
  jpeg_destroy_compress (&c);
  *size = length;
  return data;
}

static void
check_duplex (unsigned dpi, unsigned first, int interleave, int partial)
{
  struct replay r = {.chunk = partial ? 7 : 0 };
  atomic_init (&r.cancelled, 0);
  const struct sanei_sp1130n_settings s = {.dpi = dpi,.width_mm =
      210,.height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000001";
  size_t sizes[2];
  uint8_t *j[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  startup (&r, &s, token);
  size_t first_header = r.count + 1;
  if (interleave)
    {
      block (&r, first, j[first], sizes[first] / 2, 0, 0, 0);
      block (&r, 1 - first, j[1 - first], sizes[1 - first] / 2, 0, 0, 0);
      block (&r, first, j[first] + sizes[first] / 2,
             sizes[first] - sizes[first] / 2, 0x1005, 2, 2);
      block (&r, 1 - first, j[1 - first] + sizes[1 - first] / 2,
             sizes[1 - first] - sizes[1 - first] / 2, 0x1006, 2, 2);
    }
  else
    {
      block (&r, first, j[first], sizes[first], 0x1005, 2, 2);
      block (&r, 1 - first, j[1 - first], sizes[1 - first], 0x1006, 2, 2);
    }
  for (size_t i = first_header; i < r.count; ++i)
    if (r.events[i].input && r.events[i].size == 64)
      r.events[i].max_timeout = 5000;
  r.events[first_header].min_timeout = 10000;
  r.events[first_header].max_timeout = 30000;
  uint8_t event[64] = { 0 };
  event[6] = 4;
  append (&r, 1, event, sizeof event);
  cleanup (&r, token);
  struct sanei_sp1130n_io io = {.context = &r,.bulk = transfer,.cancelled =
      &r.cancelled
  };
  struct sanei_sp1130n_sheet sheet;
  assert (sanei_sp1130n_scan_sheet (&io, &s, token, &sheet) == SP_OK);
  assert (sheet.count == 2);
  assert (sheet.page[0].size == 12 && sheet.page[1].size == 12);
  assert (sheet.page[0].data[0] > 200 && sheet.page[0].data[2] < 20);
  assert (sheet.page[1].data[2] > 200 && sheet.page[1].data[0] < 20);
  sanei_sp1130n_sheet_free (&sheet);
  release (&r);
  free (j[0]);
  free (j[1]);
}

static void
usb_paper_start (struct replay *r, const uint8_t token[40], uint32_t error)
{
  uint8_t reply[108] = { 0 };
  command (r, 0x12, 0x7a, 108, 0, reply, sizeof reply);
  ack (r, 0x2a, 0x78, 1, 40);
  append (r, 0, token, 40);
  ack (r, 0x28, 0x78, 1, 40);
  append (r, 1, token, 40);
  sanei_sp1130n_put32 (reply, error);
  command (r, 0x12, 2, 32, 0, reply, 32);
  ack (r, 0x2a, 0x78, 2, 40);
  append (r, 0, token, 40);
}

static void
usb_empty_start (struct replay *r, const uint8_t token[40])
{
  usb_paper_start (r, token, 0);
}

static void
usb_session_tests (void)
{
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000008";
  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  for (unsigned source = SP_SOURCE_FRONT; source <= SP_SOURCE_BACK; ++source)
    for (unsigned dpi = 300; dpi <= 600; dpi *= 2)
      for (unsigned first = 0; first < 2; ++first)
        for (unsigned first_event = 2; first_event <= 4; first_event += 2)
        {
          struct replay r = { .chunk = 7 };
          atomic_init (&r.cancelled, 0);
          struct sanei_sp1130n_settings settings = {
            .dpi = dpi, .width_mm = 210, .height_mm = 297, .source = source
          };
          startup (&r, &settings, token);
          size_t boundaries[2];
          for (unsigned n = 0; n < 2; ++n)
            {
              if (n)
                {
                  /* Event 2 continues the same START; only event 4 needs a new job. */
                  if (first_event == 4)
                    startup (&r, &settings, token);
                }
              if (source == SP_SOURCE_DUPLEX)
                {
                  block (&r, first, images[first], sizes[first], 0x1005, 2, 2);
                  block (&r, 1 - first, images[1 - first], sizes[1 - first],
                         n ? 0x1006 : 0x1005, 2, 2);
                }
              else
                {
                  unsigned side = source == SP_SOURCE_BACK ? 1 : 0;
                  block (&r, side, images[side], sizes[side], 0x1005, 2, 2);
                }
              uint8_t event[64] = { 0 };
              event[6] = n ? 4 : first_event;
              append (&r, 1, event, sizeof event);
              if (event[6] == 4)
                cleanup (&r, token);
              boundaries[n] = r.count;
            }
          usb_empty_start (&r, token);
          struct sanei_sp1130n_io io = {
            .context = &r, .bulk = transfer, .cancelled = &r.cancelled
          };
          struct sanei_sp1130n_session session = { 0 };
          struct sanei_sp1130n_sheet sheet;
          assert (sanei_sp1130n_scan_next (&io, &settings, token, &session,
                                          &sheet) == SP_OK);
          assert (r.index == boundaries[0]);
          assert (session.started == (first_event == 2) &&
                  session.stop_pending == (first_event == 2) &&
                  session.own_lock == (first_event == 2) && !session.exhausted &&
                  session.sheets == 1);
          assert (sheet.count == (source == SP_SOURCE_DUPLEX ? 2u : 1u));
          sanei_sp1130n_sheet_free (&sheet);
          assert (sanei_sp1130n_scan_next (&io, &settings, token, &session,
                                          &sheet) == SP_OK);
          assert (r.index == boundaries[1]);
          assert (!session.started && !session.own_lock && !session.exhausted &&
                  session.sheets == 2);
          sanei_sp1130n_sheet_free (&sheet);
          assert (sanei_sp1130n_scan_next (&io, &settings, token, &session,
                                          &sheet) == SP_NO_DOCS);
          size_t before = r.index;
          assert (r.index == r.count && session.exhausted);
          assert (sanei_sp1130n_scan_next (&io, &settings, token, &session,
                                          &sheet) == SP_NO_DOCS);
          assert (r.index == before && !sheet.count);
          assert (sanei_sp1130n_scan_finish (&io, token, &session) == SP_OK);
          release (&r);
        }

  for (unsigned scenario = 0; scenario < 5; ++scenario)
    {
      struct replay r = { .chunk = 7 };
      atomic_init (&r.cancelled, 0);
      struct sanei_sp1130n_settings settings = {
        .dpi = 300, .width_mm = 210, .height_mm = 297,
        .source = SP_SOURCE_DUPLEX
      };
      startup (&r, &settings, token);
      const uint8_t invalid_jpeg[] = { 0, 0, 0, 0 };
      block (&r, 0, scenario == 4 ? invalid_jpeg : images[0],
             scenario == 4 ? sizeof invalid_jpeg : sizes[0], 0x1005, 2, 2);
      block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
      uint8_t event[64] = { 0 };
      event[6] = 2;
      append (&r, 1, event, sizeof event);
      if (scenario == 2 || scenario == 3)
        {
          uint8_t header[64] = { 0 };
          if (scenario == 2)
            sanei_sp1130n_put32 (header, 0x2002);
          command (&r, 0x28, 4, 0, 0, header, sizeof header);
          if (scenario == 3)
            r.events[r.count - 1].status = SP_IO;
        }
      ack (&r, 0x14, 6, 0, 0);
      ack (&r, 0x2a, 0x78, 2, 40);
      append (&r, 0, token, 40);
      struct sanei_sp1130n_io io = {
        .context = &r, .bulk = transfer, .cancelled = &r.cancelled
      };
      struct sanei_sp1130n_session session = { 0 };
      struct sanei_sp1130n_sheet sheet;
      assert (sanei_sp1130n_scan_next (&io, &settings, token, &session,
                                      &sheet) == (scenario == 4 ? SP_IO : SP_OK));
      sanei_sp1130n_sheet_free (&sheet);
      if (!scenario)
        assert (sanei_sp1130n_scan_finish (&io, token, &session) == SP_OK);
      else
        {
          if (scenario == 1)
            atomic_store (&r.cancelled, 1);
          const int errors[] = { 0, SP_CANCELLED, SP_JAMMED, SP_IO, SP_IO };
          assert (sanei_sp1130n_scan_next (&io, &settings, token, &session,
                                          &sheet) == errors[scenario]);
          assert (!sheet.count && !session.started && !session.stop_pending
                  && !session.own_lock);
          size_t before = r.index;
          assert (sanei_sp1130n_scan_next (&io, &settings, token, &session,
                                          &sheet) == errors[scenario]);
          assert (r.index == before);
        }
      assert (sanei_sp1130n_scan_finish (&io, token, &session) == SP_OK);
      release (&r);
    }
  free (images[0]);
  free (images[1]);
}

SANE_Status sane_sp1130n_init (SANE_Int *, SANE_Auth_Callback);
SANE_Status sane_sp1130n_open (SANE_String_Const, SANE_Handle *);
SANE_Status sane_sp1130n_start (SANE_Handle);
SANE_Status sane_sp1130n_read (SANE_Handle, SANE_Byte *, SANE_Int, SANE_Int *);
SANE_Status sane_sp1130n_control_option (SANE_Handle, SANE_Int, SANE_Action,
                                       void *, SANE_Int *);
void sane_sp1130n_cancel (SANE_Handle);
void sane_sp1130n_close (SANE_Handle);
void sane_sp1130n_exit (void);

/* Only the physical USB adapter is replaced; acquisition and SANE state are real. */
struct sanei_sp1130n_usb
{
  struct replay *replay;
};
static struct replay *backend_replay;
static const uint8_t backend_token[40] =
  "00000000-0000-4000-8000-000000000009";

int
sanei_sp1130n_usb_list (struct sanei_sp1130n_device **devices, size_t *count)
{
  *devices = calloc (1, sizeof **devices);
  assert (*devices);
  strcpy ((*devices)->name, "sp1130n:usb:001:002");
  (*devices)->vendor = SP_FUJITSU_VID;
  *count = 1;
  return SP_OK;
}

int
sanei_sp1130n_usb_open (const char *name, struct sanei_sp1130n_usb **usb)
{
  assert (backend_replay && !strcmp (name, "sp1130n:usb:001:002"));
  *usb = malloc (sizeof **usb);
  assert (*usb);
  (*usb)->replay = backend_replay;
  return SP_OK;
}

struct sanei_sp1130n_io
sanei_sp1130n_usb_io (struct sanei_sp1130n_usb *usb,
                      const atomic_bool *cancelled)
{
  return (struct sanei_sp1130n_io) {
    .context = usb->replay, .bulk = transfer, .cancelled = cancelled,
    .recover_inquiry = usb->replay->recover_enabled ?
      recover_inquiry_reply : NULL,
    .recover_image = usb->replay->image_recover_enabled ?
      recover_image_reply : NULL,
    .image_recovery_done = usb->replay->image_recover_enabled ?
      image_recovery_commit : NULL
  };
}

void
sanei_sp1130n_usb_close (struct sanei_sp1130n_usb *usb)
{
  free (usb);
}

int
sanei_sp1130n_token (uint8_t token[40])
{
  memcpy (token, backend_token, 40);
  return SP_OK;
}

static void
backend_page (SANE_Handle handle, unsigned side)
{
  uint8_t data[12];
  SANE_Int length = 99;
  assert (sane_sp1130n_read (handle, data, sizeof data, &length)
          == SANE_STATUS_GOOD && length == sizeof data);
  for (unsigned pixel = 0; pixel < 4; ++pixel)
    {
      assert (data[3 * pixel + (side ? 2 : 0)] > 200);
      assert (data[3 * pixel + (side ? 0 : 2)] < 20);
    }
  assert (sane_sp1130n_read (handle, data, sizeof data, &length)
          == SANE_STATUS_EOF && !length);
}

static void
backend_usb_session_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  assert (sane_sp1130n_init (NULL, NULL) == SANE_STATUS_GOOD);
  for (unsigned event_kind = 2; event_kind <= 4; event_kind += 2)
    for (unsigned cancel_front = 0; cancel_front < 2; ++cancel_front)
      {
        struct replay r = { .chunk = 7 };
        atomic_init (&r.cancelled, 0);
        startup (&r, &settings, backend_token);
        block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
        block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
        uint8_t event[64] = { 0 };
        event[6] = event_kind;
        append (&r, 1, event, sizeof event);
        if (event_kind == 4)
          cleanup (&r, backend_token);
        size_t first_boundary = r.count;
        if (cancel_front && event_kind == 2)
          cleanup (&r, backend_token);
        if (cancel_front || event_kind == 4)
          startup (&r, &settings, backend_token);
        block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
        block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
        event[6] = 4;
        append (&r, 1, event, sizeof event);
        cleanup (&r, backend_token);
        size_t second_boundary = r.count;
        usb_empty_start (&r, backend_token);
        size_t empty_boundary = r.count;
        /* A caller can cancel/reload and start a new batch on this same handle. */
        startup (&r, &settings, backend_token);
        block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
        block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
        append (&r, 1, event, sizeof event);
        cleanup (&r, backend_token);
        usb_paper_start (&r, backend_token, cancel_front ? 0x21 : 0x2002);
        size_t failed_boundary = r.count;
        usb_empty_start (&r, backend_token);

        backend_replay = &r;
        SANE_Handle handle;
        assert (sane_sp1130n_open ("usb:001:002", &handle) == SANE_STATUS_GOOD);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
        assert (r.index == first_boundary);
        backend_page (handle, 0);
        if (cancel_front)
          {
            sane_sp1130n_cancel (handle);
            uint8_t byte;
            SANE_Int length = 99;
            assert (sane_sp1130n_read (handle, &byte, 1, &length)
                    == SANE_STATUS_CANCELLED && !length);
          }
        else
          {
            assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
            assert (r.index == first_boundary);
            backend_page (handle, 1);
          }
        assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
        assert (r.index == second_boundary);
        backend_page (handle, 0);    /* Not the cancelled sheet's buffered back. */
        assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
        assert (r.index == second_boundary);
        backend_page (handle, 1);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
        assert (r.index == empty_boundary);
        sane_sp1130n_cancel (handle);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
        backend_page (handle, 0);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
        backend_page (handle, 1);
        SANE_Status failure = cancel_front ? SANE_STATUS_IO_ERROR :
          SANE_STATUS_JAMMED;
        assert (sane_sp1130n_start (handle) == failure);
        assert (sane_sp1130n_start (handle) == failure);
        assert (r.index == failed_boundary);
        sane_sp1130n_cancel (handle);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
        sane_sp1130n_close (handle);
        release (&r);
        backend_replay = NULL;
      }
  sane_sp1130n_exit ();
  free (images[0]);
  free (images[1]);
}

static void
backend_usb_first_sheet_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  assert (sane_sp1130n_init (NULL, NULL) == SANE_STATUS_GOOD);
  for (unsigned event_kind = 2; event_kind <= 4; event_kind += 2)
    for (unsigned cancel_front = 0; cancel_front < 2; ++cancel_front)
      {
        struct replay r = { .chunk = 7 };
        atomic_init (&r.cancelled, 0);
        startup (&r, &settings, backend_token);
        block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
        block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
        uint8_t event[64] = { 0 };
        event[6] = event_kind;
        append (&r, 1, event, sizeof event);
        if (event_kind == 4)
          {
            cleanup (&r, backend_token);
            startup (&r, &settings, backend_token);
          }
        /* Later sheet has opposite colors: it must never replace the first. */
        block (&r, 0, images[1], sizes[1], 0x1005, 2, 2);
        block (&r, 1, images[0], sizes[0], 0x1005, 2, 2);
        event[6] = 4;
        append (&r, 1, event, sizeof event);
        cleanup (&r, backend_token);
        usb_empty_start (&r, backend_token);
        backend_replay = &r;
        SANE_Handle handle;
        assert (sane_sp1130n_open ("usb:001:002", &handle) == SANE_STATUS_GOOD);
        SANE_Word batch = SANE_FALSE;
        assert (sane_sp1130n_control_option
                (handle, 20, SANE_ACTION_SET_VALUE, &batch, NULL) == SANE_STATUS_GOOD);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
        assert (r.index == r.count);  /* Complete drain before the first read. */
        backend_page (handle, 0);
        if (cancel_front)
          sane_sp1130n_cancel (handle);
        else
          {
            assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
            backend_page (handle, 1);
            assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
            assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
          }
        sane_sp1130n_close (handle);
        release (&r);
        backend_replay = NULL;
      }
  for (unsigned failure = 0; failure < 2; ++failure)
    {
      struct replay r = { .chunk = 7 };
      atomic_init (&r.cancelled, 0);
      startup (&r, &settings, backend_token);
      block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
      block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
      uint8_t event[64] = { 0 };
      event[6] = 4;
      append (&r, 1, event, sizeof event);
      cleanup (&r, backend_token);
      usb_paper_start (&r, backend_token, failure ? 0 : 0x2002);
      if (failure)
        {
          free (r.events[--r.count].data);  /* Rejected unlock has no token OUT. */
          sanei_sp1130n_put32 (r.events[r.count - 1].data, 0x4004);
        }
      backend_replay = &r;
      SANE_Handle handle;
      assert (sane_sp1130n_open ("usb:001:002", &handle) == SANE_STATUS_GOOD);
      SANE_Word batch = SANE_FALSE;
      assert (sane_sp1130n_control_option
              (handle, 20, SANE_ACTION_SET_VALUE, &batch, NULL) == SANE_STATUS_GOOD);
      SANE_Status expected = failure ? SANE_STATUS_IO_ERROR : SANE_STATUS_JAMMED;
      assert (sane_sp1130n_start (handle) == expected && r.index == r.count);
      assert (sane_sp1130n_start (handle) == expected && r.index == r.count);
      uint8_t byte = 0xa5;
      SANE_Int length = 99;
      assert (sane_sp1130n_read (handle, &byte, 1, &length) == SANE_STATUS_INVAL
              && !length && byte == 0xa5);
      sane_sp1130n_close (handle);
      release (&r);
      backend_replay = NULL;
    }
  sane_sp1130n_exit ();
  free (images[0]);
  free (images[1]);
}

static void
backend_usb_idle_cancel_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  assert (sane_sp1130n_init (NULL, NULL) == SANE_STATUS_GOOD);
  for (unsigned event_kind = 2; event_kind <= 4; event_kind += 2)
    {
      struct replay r = { .chunk = 7 };
      atomic_init (&r.cancelled, 0);
      atomic_init (&r.boundary_reached, 0);
      startup (&r, &settings, backend_token);
      block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
      block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
      uint8_t event[64] = { 0 };
      event[6] = event_kind;
      append (&r, 1, event, sizeof event);
      if (event_kind == 4)
        cleanup (&r, backend_token);
      size_t acquired = r.count;
      if (event_kind == 2)
        cleanup (&r, backend_token);
      r.observe_boundary = r.count;
      backend_replay = &r;
      SANE_Handle handle;
      assert (sane_sp1130n_open ("usb:001:002", &handle) == SANE_STATUS_GOOD);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
      assert (r.index == acquired);
      backend_page (handle, 0);
      sane_sp1130n_cancel (handle);
      /* A single-image GUI job never calls read/start again after cancel. */
      const struct timespec pause = { .tv_nsec = 10 * 1000 * 1000 };
      unsigned waited;
      for (waited = 0; waited < 100; ++waited)
        {
          if (atomic_load (&r.boundary_reached))
            break;
          nanosleep (&pause, NULL);
        }
      assert (waited < 100);
      sane_sp1130n_close (handle);
      assert (r.index == r.count); /* STOP/unlock once, no further block or START. */
      release (&r);
      backend_replay = NULL;
    }
  sane_sp1130n_exit ();
  free (images[0]);
  free (images[1]);
}

static void
usb_inquiry_timeout_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  const size_t partials[] = { 0, 17, 108 };
  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  assert (sane_sp1130n_init (NULL, NULL) == SANE_STATUS_GOOD);
  for (unsigned n = 0; n < sizeof partials / sizeof partials[0]; ++n)
    {
      struct replay r = { .chunk = 7, .recover_enabled = 1,
        .expected_tail = 108 - partials[n] };
      atomic_init (&r.cancelled, 0);
      uint8_t reply[108] = { 0 };
      command (&r, 0x12, 0x7a, 108, 0, reply, sizeof reply);
      r.events[r.count - 1].status = SP_IO;
      r.events[r.count - 1].error_bytes = partials[n];
      if (r.expected_tail)
        append (&r, 1, reply + partials[n], r.expected_tail);
      size_t failed_boundary = r.count;
      startup (&r, &settings, backend_token);
      block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
      block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
      uint8_t event[64] = { 0 };
      event[6] = 4;
      append (&r, 1, event, sizeof event);
      cleanup (&r, backend_token);
      usb_empty_start (&r, backend_token);

      backend_replay = &r;
      SANE_Handle handle;
      assert (sane_sp1130n_open ("usb:001:002", &handle) == SANE_STATUS_GOOD);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
      assert (r.index == failed_boundary && r.recovery_calls == 1);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
      assert (r.index == failed_boundary && r.recovery_calls == 1);
      uint8_t byte = 0xa5;
      SANE_Int length = 99;
      assert (sane_sp1130n_read (handle, &byte, 1, &length)
              == SANE_STATUS_INVAL && !length && byte == 0xa5);
      sane_sp1130n_cancel (handle);
      assert (sane_sp1130n_read (handle, &byte, 1, &length)
              == SANE_STATUS_CANCELLED && !length && byte == 0xa5);
      assert (r.index == failed_boundary);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
      backend_page (handle, 0);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
      backend_page (handle, 1);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
      sane_sp1130n_close (handle);
      release (&r);
      backend_replay = NULL;
    }
  sane_sp1130n_exit ();
  free (images[0]);
  free (images[1]);

  /* No inquiry recovery for LAN, other commands, OUT errors or wrong framing. */
  for (unsigned scenario = 0; scenario < 7; ++scenario)
    {
      struct replay r = { .recover_failure = 1, .expected_tail = 108 };
      atomic_init (&r.cancelled, 0);
      uint8_t reply[108] = { 0 };
      uint8_t op = scenario == 1 ? 0x28 : 0x12;
      uint8_t sub = scenario == 2 ? 2 : 0x7a;
      uint16_t selector = scenario == 3 ? 32 : 108;
      uint32_t value = scenario == 4 ? 1 : 0;
      size_t size = scenario == 5 ? 32 : sizeof reply;
      command (&r, op, sub, selector, value, reply, scenario == 6 ? 0 : size);
      r.events[r.count - 1].status = SP_IO;
      struct sanei_sp1130n_io io = { .context = &r, .bulk = transfer,
        .network = scenario == 0, .recover_inquiry = recover_inquiry_reply };
      assert (sanei_sp1130n_command (&io, op, sub, selector, value, reply,
                                     size) == SP_IO);
      assert (!r.recovery_calls);
      release (&r);
    }
  struct replay r = { .recover_failure = 1, .expected_tail = 108 };
  uint8_t reply[108] = { 0 };
  command (&r, 0x12, 0x7a, 108, 0, reply, sizeof reply);
  r.events[r.count - 1].status = SP_IO;
  struct sanei_sp1130n_io io = { .context = &r, .bulk = transfer,
    .recover_inquiry = recover_inquiry_reply };
  assert (sanei_sp1130n_probe (&io, reply) == SP_IO && r.recovery_calls == 1);
  release (&r);
}

static void
failures (void)
{
  const struct sanei_sp1130n_settings s = {.dpi = 300,.width_mm =
      210,.height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000002";
  for (unsigned scenario = 0; scenario < 4; ++scenario)
    {
      struct replay r = { 0 };
      atomic_init (&r.cancelled, 0);
      startup (&r, &s, token);
      uint8_t h[64] = { 0 };
      h[11] = 1;
      if (scenario == 0)
        sanei_sp1130n_put32 (h + 36, SP_COMPRESSED_LIMIT + 1);
      if (scenario == 1)
        sanei_sp1130n_put32 (h, 0x2002);
      if (scenario == 2)
        {
          sanei_sp1130n_put32 (h, 0x1005);
          sanei_sp1130n_put32 (h + 16, 2);
          sanei_sp1130n_put32 (h + 20, 2);
          h[48] = 1;
          sanei_sp1130n_put32 (h + 36, 4);
        }
      command (&r, 0x28, 4, 0, 0, h, sizeof h);
      if (scenario == 2)
        {
          uint8_t invalid[4] = { 0 };
          append (&r, 1, invalid, 4);
          /* Back image is missing: a bounded transfer error, never partial duplex success. */
          uint8_t header[16];
          sanei_sp1130n_header (header, 0x28, 4, 0, 0);
          append (&r, 0, header, sizeof header);
          append (&r, 1, h, sizeof h);
          r.events[r.count - 1].status = SP_IO;
        }
      if (scenario == 3)
        r.events[r.count - 1].cancel_after = 1;
      cleanup (&r, token);
      struct sanei_sp1130n_io io = {.context = &r,.bulk =
          transfer,.cancelled = &r.cancelled
      };
      struct sanei_sp1130n_sheet sheet;
      int expected[] = { SP_NOMEM, SP_JAMMED, SP_IO, SP_CANCELLED };
      assert (sanei_sp1130n_scan_sheet (&io, &s, token, &sheet) ==
              expected[scenario]);
      assert (!sheet.count && !sheet.page[0].data && !sheet.page[1].data);
      release (&r);
    }
  struct replay busy = { 0 };
  uint8_t locked[108] = { 0 };
  sanei_sp1130n_put32 (locked + 32, 1);
  command (&busy, 0x12, 0x7a, 108, 0, locked, sizeof locked);
  struct sanei_sp1130n_io io = {.context = &busy,.bulk = transfer };
  struct sanei_sp1130n_sheet sheet;
  assert (sanei_sp1130n_scan_sheet (&io, &s, token, &sheet) == SP_BUSY);
  release (&busy);
}

static void
usb_cleanup_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000005";
  for (unsigned step = 0; step < 5; ++step)
    {
      struct replay r = { .chunk = 7 };
      atomic_init (&r.cancelled, 0);
      uint8_t reply[108] = { 0 };
      command (&r, 0x12, 0x7a, 108, 0, reply, sizeof reply);
      ack (&r, 0x2a, 0x78, 1, 40);
      if (step < 2)
        r.events[r.count - 2 + step].cancel_after = 1;
      append (&r, 0, token, sizeof token);
      r.events[r.count - 1].cancel_after = step == 2;
      ack (&r, 0x28, 0x78, 1, 40);
      r.events[r.count - 1].cancel_after = step == 3;
      append (&r, 1, token, sizeof token);
      r.events[r.count - 1].cancel_after = step == 4;
      ack (&r, 0x2a, 0x78, 2, 40);
      append (&r, 0, token, sizeof token);
      struct sanei_sp1130n_io io = {
        .context = &r, .bulk = transfer, .cancelled = &r.cancelled
      };
      struct sanei_sp1130n_sheet sheet;
      assert (sanei_sp1130n_scan_sheet (&io, &settings, token, &sheet)
              == SP_CANCELLED);
      assert (!sheet.count && !sheet.page[0].data && !sheet.page[1].data);
      release (&r);
    }
  const uint32_t errors[] = { 0x2002, 0x2009, 0x200e };
  const unsigned lengths[] = { 0, 8, 8193 };
  size_t jpeg_sizes[2];
  uint8_t *images[2] = { jpeg (0, &jpeg_sizes[0]), jpeg (1, &jpeg_sizes[1]) };
  for (unsigned unsolicited = 0; unsolicited < 2; ++unsolicited)
    for (unsigned error = 0; error < sizeof errors / sizeof errors[0]; ++error)
      for (unsigned length = 0; length < sizeof lengths / sizeof lengths[0];
           ++length)
        {
          struct replay r = { .chunk = 7 };
          atomic_init (&r.cancelled, 0);
          startup (&r, &settings, token);
          uint8_t header[64] = { 0 }, payload[8193] = { 0x55 };
          sanei_sp1130n_put32 (header, errors[error]);
          sanei_sp1130n_put32 (header + 36, lengths[length]);
          if (unsolicited)
            {
              block (&r, 0, images[0], jpeg_sizes[0], 0x1005, 2, 2);
              block (&r, 1, images[1], jpeg_sizes[1], 0x1006, 2, 2);
              append (&r, 1, header, sizeof header);
            }
          else
            command (&r, 0x28, 4, 0, 0, header, sizeof header);
          if (lengths[length])
            append (&r, 1, payload, lengths[length]);
          cleanup (&r, token);
          struct sanei_sp1130n_io io = {
            .context = &r, .bulk = transfer, .cancelled = &r.cancelled
          };
          struct sanei_sp1130n_sheet sheet;
          assert (sanei_sp1130n_scan_sheet (&io, &settings, token, &sheet)
                  == sanei_sp1130n_device_error (errors[error]));
          assert (!sheet.count && !sheet.page[0].data && !sheet.page[1].data);
          release (&r);
        }
  free (images[0]);
  free (images[1]);
}

static void
usb_rejected_payload_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000007";
  size_t jpeg_size;
  uint8_t *image = jpeg (0, &jpeg_size);
  for (unsigned scenario = 0; scenario < 3; ++scenario)
    {
      struct replay r = { .chunk = 7 };
      atomic_init (&r.cancelled, 0);
      startup (&r, &settings, token);
      if (scenario == 1)
        block (&r, 0, image, jpeg_size, 0x1005, 2, 2);
      uint8_t header[64] = { 0 }, payload[8193] = { 0x55 };
      header[11] = scenario == 0 ? 2 : 1;
      sanei_sp1130n_put32 (header + 36, sizeof payload);
      command (&r, 0x28, 4, 0, 0, header, sizeof header);
      append (&r, 1, payload, sizeof payload);
      cleanup (&r, token);
      struct sanei_sp1130n_io io = {
        .context = &r, .bulk = transfer, .cancelled = &r.cancelled
      };
      struct sanei_sp1130n_sheet sheet;
      fail_realloc = scenario == 2;
      assert (sanei_sp1130n_scan_sheet (&io, &settings, token, &sheet)
              == (scenario == 2 ? SP_NOMEM : SP_IO));
      assert (!fail_realloc && !sheet.count && !sheet.page[0].data &&
              !sheet.page[1].data);
      release (&r);
    }
  for (unsigned scenario = 0; scenario < 3; ++scenario)
    {
      struct replay r = { .chunk = 7 };
      atomic_init (&r.cancelled, 0);
      startup (&r, &settings, token);
      block (&r, 0, image, jpeg_size, 0x1005, 2, 2);
      size_t next = r.count;
      block (&r, 1, image, jpeg_size, 0x1006, 2, 2);
      if (scenario == 1)
        r.events[next + 1].data[48] = 0;
      if (scenario == 2)
        sanei_sp1130n_put32 (r.events[next + 1].data + 20, 0);
      uint8_t event[64] = { 0 };
      event[6] = 4;
      append (&r, 1, event, sizeof event);
      cleanup (&r, token);
      struct sanei_sp1130n_io io = {
        .context = &r, .bulk = transfer, .cancelled = &r.cancelled
      };
      struct sanei_sp1130n_sheet sheet;
      fail_realloc = scenario == 0 ? 2 : 0;
      const int expected[] = { SP_NOMEM, SP_UNSUPPORTED, SP_IO };
      assert (sanei_sp1130n_scan_sheet (&io, &settings, token, &sheet)
              == expected[scenario]);
      assert (!fail_realloc && !sheet.count && !sheet.page[0].data &&
              !sheet.page[1].data);
      release (&r);
    }
  free (image);
}

static void
truncate_replay (struct replay *r, size_t count)
{
  assert (count <= r->count);
  for (size_t i = count; i < r->count; ++i)
    free (r->events[i].data);
  r->count = count;
}

static void
usb_image_timeout_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  assert (sane_sp1130n_init (NULL, NULL) == SANE_STATUS_GOOD);
  /* Model the adapter's blocked connection, including unread payload tails.
   * These are transfer faults, not evidence of physical ejection or recovery. */
  for (unsigned phase = 0; phase < 5; ++phase)
    for (unsigned partial = 0; partial < 3; ++partial)
      for (unsigned recovery = 0; recovery < 4; ++recovery)
      {
        struct replay r = { .fail_closed = 1, .recover_enabled = 1,
          .image_recover_enabled = recovery != 0,
          .image_recover_failure = recovery >= 2 ?
            (phase == 4 ? 1 : recovery - 1) : 0 };
        atomic_init (&r.cancelled, 0);
        startup (&r, &settings, backend_token);
        size_t first = r.count;
        size_t failed = first + (phase == 0 ? 1 : 2);
        if (phase == 2)
          {
            block (&r, 0, images[0], sizes[0] / 2, 0, 0, 0);
            failed = r.count + 2;
            block (&r, 0, images[0] + sizes[0] / 2,
                   sizes[0] - sizes[0] / 2, 0x1005, 2, 2);
          }
        else
          block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
        size_t packet_boundary = r.count;
        if (phase == 3)
          failed = r.count + 2;
        block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
        if (phase == 4)
          failed = r.count;
        uint8_t event[64] = { 0 };
        event[6] = 4;
        append (&r, 1, event, sizeof event);
        if (recovery && phase < 3)
          truncate_replay (&r, packet_boundary);
        cleanup (&r, backend_token);
        struct event *fault = &r.events[failed];
        assert (fault->input && fault->size > 17);
        fault->status = SP_IO;
        fault->error_bytes = partial == 0 ? 0 : partial == 1 ? 17 :
          fault->size;
        size_t boundary = failed + (fault->error_bytes == fault->size);
        size_t recovered_boundary = r.count;
        if (recovery == 1)
          {
            startup (&r, &settings, backend_token);
            block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
            block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
            append (&r, 1, event, sizeof event);
            cleanup (&r, backend_token);
            usb_empty_start (&r, backend_token);
          }

        backend_replay = &r;
        SANE_Handle handle;
        assert (sane_sp1130n_open ("usb:001:002", &handle) == SANE_STATUS_GOOD);
        assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
        assert (!r.recovery_calls);
        if (recovery == 1)
          assert (!r.blocked && r.index == recovered_boundary &&
                  !r.blocked_calls && r.image_commits == 1);
        else
          assert (r.blocked && r.index < r.count && r.blocked_calls == 2 &&
                  (recovery ? r.image_commits == 1 :
                   r.index == boundary && r.wire_calls == failed + 1));
        size_t attempts = r.blocked_calls;
        size_t wires = r.wire_calls, position = r.index;
        assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
        uint8_t data[33], expected[33];
        memset (data, 0xa5, sizeof data);
        memcpy (expected, data, sizeof data);
        SANE_Int length = 99;
        assert (sane_sp1130n_read (handle, data, 1, &length)
                == SANE_STATUS_INVAL && !length &&
                !memcmp (data, expected, sizeof data));
        sane_sp1130n_cancel (handle);
        assert (sane_sp1130n_read (handle, data, 1, &length)
                == SANE_STATUS_CANCELLED && !length &&
                !memcmp (data, expected, sizeof data));
        assert (r.wire_calls == wires && r.blocked_calls == attempts);
        if (recovery == 1)
          {
            assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
            backend_page (handle, 0);
            assert (sane_sp1130n_start (handle) == SANE_STATUS_GOOD);
            backend_page (handle, 1);
            assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
            assert (sane_sp1130n_start (handle) == SANE_STATUS_NO_DOCS);
            assert (r.index == r.count && r.image_commits == 1);
          }
        else
          {
            /* Cancel clears job state, not a blocked USB connection. */
            assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
            assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
            assert (r.index == position && r.wire_calls == wires &&
                    r.blocked_calls == attempts + 1 && !r.recovery_calls);
          }
        sane_sp1130n_close (handle);
        if (recovery != 1)
          assert (r.index == position && r.wire_calls == wires);
        truncate_replay (&r, r.index);
        release (&r);
        backend_replay = NULL;
      }
  sane_sp1130n_exit ();
  free (images[0]);
  free (images[1]);
  puts ("USB image timeout checks passed: 60 synthetic cases, bounded recovery "
        "and cleanup, explicit restart, failed recovery blocked, no partial images.");
}

static void
usb_image_recovery_bounds_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  size_t image_size;
  uint8_t *image = jpeg (0, &image_size);
  assert (sane_sp1130n_init (NULL, NULL) == SANE_STATUS_GOOD);
  for (unsigned scenario = 0; scenario < 7; ++scenario)
    {
      struct replay r = { .fail_closed = 1, .image_recover_enabled = 1 };
      atomic_init (&r.cancelled, 0);
      startup (&r, &settings, backend_token);
      uint8_t header[64] = { 0 };
      header[11] = scenario == 1 ? 2 : scenario == 2 ? 0 : 1;
      sanei_sp1130n_put32 (header + 36,
                           scenario == 0 ? SP_COMPRESSED_LIMIT + 1 : 0);
      size_t failed = r.count + 1;
      command (&r, 0x28, 4, 0, 0, header, sizeof header);
      if (scenario == 2)
        {
          r.events[failed].data[11] = 2;
          sanei_sp1130n_put32 (r.events[failed].data, 0x1005);
        }
      if (scenario == 6)
        {
          truncate_replay (&r, failed - 1);
          block (&r, 0, image, image_size, 0x1005, 2, 2);
          /* A recovered header must not revive an already completed side. */
          block (&r, 0, header, 0, 0x1005, 2, 2);
          failed = r.count - 1;
        }
      else if (scenario >= 3)
        {
          truncate_replay (&r, failed - 1);
          block (&r, 0, image, image_size, 0x1005, 2, 2);
          block (&r, 1, header, 0, 0x1005, 2, 2);
          /* Force recovery in a known last-side payload: no valid JPEG is
           * needed because the failed job must never reach decoding. */
          sanei_sp1130n_put32 (r.events[r.count - 1].data + 36, 32);
          uint8_t payload[32] = { 0 };
          append (&r, 1, payload, sizeof payload);
          failed = r.count - 1;
          unsigned records = scenario == 5 ? 32 : 1;
          for (unsigned i = 0; i < records; ++i)
            {
              uint8_t event[64] = { 0 };
              if (scenario == 3)
                {
                  event[6] = 4;
                  sanei_sp1130n_put32 (event + 36, 1);
                }
              else if (scenario == 4)
                {
                  sanei_sp1130n_put32 (event, 0x2002);
                  sanei_sp1130n_put32 (event + 36, SP_COMPRESSED_LIMIT + 1);
                }
              append (&r, 1, event, sizeof event);
            }
        }
      r.events[failed].status = SP_IO;
      cleanup (&r, backend_token);
      backend_replay = &r;
      SANE_Handle handle;
      assert (sane_sp1130n_open ("usb:001:002", &handle) == SANE_STATUS_GOOD);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
      assert (r.blocked && r.image_commits == 1 && r.blocked_calls == 2);
      size_t before = r.wire_calls, position = r.index;
      sane_sp1130n_cancel (handle);
      assert (sane_sp1130n_start (handle) == SANE_STATUS_IO_ERROR);
      sane_sp1130n_close (handle);
      assert (r.index == position && r.wire_calls == before);
      truncate_replay (&r, r.index);
      release (&r);
      backend_replay = NULL;
    }
  sane_sp1130n_exit ();
  free (image);
  puts ("USB image recovery framing checks passed: 7 synthetic invalid "
        "size/side/completion cases, including a duplicate completed side.");
}

static void
usb_transaction_cancellation_tests (void)
{
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000006";
  /* Each outgoing command, fixed reply and associated write must finish its
   * accepted transaction, then cancel before the next scanner operation. */
  for (unsigned phase = 0; phase < 17; ++phase)
    for (unsigned fragmented = 0; fragmented < 2; ++fragmented)
      {
        struct replay r = { .chunk = 7 };
        atomic_init (&r.cancelled, 0);
        startup (&r, &settings, token);
        assert (r.count == 17);
        unsigned boundary = phase < 2 ? 2 : phase < 8 ? 8 :
          phase < 10 ? 10 : phase < 13 ? 13 : phase < 15 ? 15 : 17;
        truncate_replay (&r, boundary);
        r.events[phase].cancel_after = !fragmented;
        r.events[phase].cancel_at = fragmented ? 7 : 0;
        if (phase >= 13)
          ack (&r, 0x14, 6, 0, 0);
        if (phase >= 2)
          {
            ack (&r, 0x2a, 0x78, 2, 40);
            append (&r, 0, token, sizeof token);
          }
        struct sanei_sp1130n_io io = {
          .context = &r, .bulk = transfer, .cancelled = &r.cancelled
        };
        struct sanei_sp1130n_sheet sheet;
        assert (sanei_sp1130n_scan_sheet (&io, &settings, token, &sheet)
                == SP_CANCELLED);
        assert (!sheet.count && !sheet.page[0].data && !sheet.page[1].data);
        release (&r);
      }

  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  for (unsigned terminal = 0; terminal < 3; ++terminal)
    for (unsigned phase = 0; phase < (terminal == 2 ? 5u :
                                      terminal == 1 ? 4u : 3u); ++phase)
      for (unsigned fragmented = 0; fragmented < 2; ++fragmented)
        {
          struct replay r = { .chunk = 7 };
          atomic_init (&r.cancelled, 0);
          startup (&r, &settings, token);
          if (terminal)
            block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
          size_t first = r.count;
          block (&r, terminal ? 1 : 0, images[terminal ? 1 : 0],
                 sizes[terminal ? 1 : 0], terminal ? 0x1006 : 0, 2, 2);
          if (terminal)
            {
              uint8_t event[64] = { 0 }, payload[8193] = { 0x55 };
              event[6] = 4;
              if (terminal == 2)
                {
                  sanei_sp1130n_put32 (event, 0x2002);
                  sanei_sp1130n_put32 (event + 36, sizeof payload);
                }
              append (&r, 1, event, sizeof event);
              if (terminal == 2)
                append (&r, 1, payload, sizeof payload);
            }
          r.events[first + phase].cancel_after = !fragmented;
          r.events[first + phase].cancel_at = fragmented ? 7 : 0;
          cleanup (&r, token);
          struct sanei_sp1130n_io io = {
            .context = &r, .bulk = transfer, .cancelled = &r.cancelled
          };
          struct sanei_sp1130n_sheet sheet;
          assert (sanei_sp1130n_scan_sheet (&io, &settings, token, &sheet)
                  == SP_CANCELLED);
          assert (!sheet.count && !sheet.page[0].data && !sheet.page[1].data);
          release (&r);
        }
  free (images[0]);
  free (images[1]);

  for (unsigned phase = 0; phase < 3; ++phase)
    for (unsigned fragmented = 0; fragmented < 2; ++fragmented)
      {
        struct replay r = { .chunk = 7 };
        atomic_init (&r.cancelled, 0);
        startup (&r, &settings, token);
        truncate_replay (&r, 8);
        ack (&r, 0x28, 0x10, 0, SP_NVRAM_SIZE);
        uint8_t snapshot[SP_NVRAM_SIZE] = { 0 }, output[SP_NVRAM_SIZE];
        memset (output, 0xa5, sizeof output);
        append (&r, 1, snapshot, sizeof snapshot);
        r.events[8 + phase].cancel_after = !fragmented;
        r.events[8 + phase].cancel_at = fragmented ? 7 : 0;
        ack (&r, 0x2a, 0x78, 2, 40);
        append (&r, 0, token, sizeof token);
        struct sanei_sp1130n_io io = {
          .context = &r, .bulk = transfer, .cancelled = &r.cancelled
        };
        assert (sanei_sp1130n_read_nvram (&io, token, output)
                == SP_CANCELLED);
        for (unsigned i = 0; i < sizeof output; ++i)
          assert (output[i] == 0xa5);
        release (&r);
      }
}

static void
jpeg_restart_tests (void)
{
  const unsigned intervals[] = { 0, 1, 2, 4, 8 };
  for (int progressive = 0; progressive < 2; ++progressive)
    for (unsigned i = 0; i < sizeof intervals / sizeof intervals[0]; ++i)
      {
        struct jpeg_compress_struct c;
        struct jpeg_error_mgr error;
        c.err = jpeg_std_error (&error);
        jpeg_create_compress (&c);
        unsigned char *bytes = NULL;
        unsigned long size = 0;
        jpeg_mem_dest (&c, &bytes, &size);
        c.image_width = c.image_height = 64;
        c.input_components = 3;
        c.in_color_space = JCS_RGB;
        jpeg_set_defaults (&c);
        if (progressive)
          jpeg_simple_progression (&c);
        c.restart_interval = intervals[i];
        jpeg_start_compress (&c, TRUE);
        uint8_t row[64 * 3];
        while (c.next_scanline < c.image_height)
          {
            for (unsigned x = 0; x < sizeof row; ++x)
              row[x] = (uint8_t) (x * 17 + c.next_scanline * 31);
            JSAMPROW rows[] = { row };
            assert (jpeg_write_scanlines (&c, rows, 1) == 1);
          }
        jpeg_finish_compress (&c);
        jpeg_destroy_compress (&c);
        uint8_t *original = malloc (size);
        assert (original);
        memcpy (original, bytes, size);
        struct sanei_sp1130n_page page;
        assert (sanei_sp1130n_decode_jpeg (bytes, size, 64, 64, &page) == SP_OK);
        assert (page.width == 64 && page.height == 64 && page.size == 64 * 64 * 3);
        assert (!memcmp (bytes, original, size));
        free (page.data);
        if (!progressive && intervals[i] == 4)
          {
            /* Placeholder dimensions and DRI still get the bounded row repair. */
            size_t position = 2;
            while (bytes[position + 1] != 0xda)
              {
                assert (bytes[position] == 0xff);
                unsigned marker = bytes[position + 1];
                unsigned length = (unsigned) bytes[position + 2] * 256 +
                  bytes[position + 3];
                if (marker == 0xc0)
                  memset (bytes + position + 5, 0, 4);
                if (marker == 0xdd)
                  {
                    bytes[position + 4] = 0;
                    bytes[position + 5] = 1;
                  }
                position += 2 + length;
                assert (position + 3 < size);
              }
            assert (sanei_sp1130n_decode_jpeg (bytes, size, 64, 64, &page)
                    == SP_OK);
            free (page.data);
          }
        free (original);
        free (bytes);
      }
}

static int
stalled (void *context, int input, uint8_t *data, size_t size,
         size_t *actual, unsigned timeout)
{
  (void) input;
  (void) data;
  (void) timeout;
  *actual = context ? size + 1 : 0;
  return SP_OK;
}

static void
simplex_tests (void)
{
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000003";
  for (unsigned back = 0; back < 2; ++back)
    {
      for (unsigned wrong_side = 0; wrong_side < 2; ++wrong_side)
        {
          struct replay r = {.chunk = 7 };
          atomic_init (&r.cancelled, 0);
          struct sanei_sp1130n_settings s = {.dpi = 300,.width_mm =
              210,.height_mm = 297,
            .source = back ? SP_SOURCE_BACK : SP_SOURCE_FRONT,
            .left_mm = 10,.top_mm = 20,.scan_width_mm = 100,.scan_height_mm =
              50
          };
          startup (&r, &s, token);
          size_t size;
          uint8_t *j = jpeg (back, &size);
          if (wrong_side)
            {
              uint8_t h[64] = { 0 };
              h[11] = (uint8_t) back;
              command (&r, 0x28, 4, 0, 0, h, sizeof h);
            }
          else
            {
              block (&r, back, j, size, 0x1006, 2, 2);
              uint8_t event[64] = { 0 };
              event[6] = 4;
              append (&r, 1, event, sizeof event);
            }
          cleanup (&r, token);
          struct sanei_sp1130n_io io = {.context = &r,.bulk =
              transfer,.cancelled = &r.cancelled
          };
          struct sanei_sp1130n_sheet sheet;
          assert (sanei_sp1130n_scan_sheet (&io, &s, token, &sheet) ==
                  (wrong_side ? SP_IO : SP_OK));
          if (!wrong_side)
            {
              assert (sheet.count == 1 && sheet.page[0].size == 12
                      && !sheet.page[1].data);
              assert (sheet.page[0].data[back ? 2 : 0] > 200);
              sanei_sp1130n_sheet_free (&sheet);
            }
          else
            assert (!sheet.count && !sheet.page[0].data
                    && !sheet.page[1].data);
          free (j);
          release (&r);
        }
    }
}

static void
resolution_tests (void)
{
  struct sanei_sp1130n_settings s = {
    .width_mm = 210, .height_mm = 297, .source = SP_SOURCE_DUPLEX
  };
  uint8_t p[SP_PARAM_SIZE];
  struct sanei_sp1130n_geometry g;
  const unsigned widths[] = { 50, 210, 216 };
  for (unsigned dpi = 50; dpi <= 600; ++dpi)
    {
      s.dpi = dpi;
      for (unsigned source = SP_SOURCE_FRONT; source <= SP_SOURCE_BACK;
           ++source)
        for (unsigned n = 0; n < sizeof widths / sizeof widths[0]; ++n)
          {
            s.source = (enum sanei_sp1130n_source) source;
            s.width_mm = widths[n];
            s.left_mm = s.top_mm = 0;
            s.scan_width_mm = s.scan_height_mm = 0;
            unsigned feeder = 10201 * dpi / 1200;
            unsigned width = widths[n] * dpi * 10 / 254;
            if (width > feeder)
              width = feeder;
            unsigned height = 297 * dpi * 10 / 254;
            assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_OK);
            assert (g.width == width && g.height == height
                    && g.page_width == width && g.page_height == height);
            assert (g.left == (feeder - width) / 2 && !g.top);
            assert (sanei_sp1130n_parameters (p, &s) == SP_OK);
            assert (sanei_sp1130n_le16 (p + 16) == dpi
                    && sanei_sp1130n_le16 (p + 18) == dpi);
            assert (sanei_sp1130n_le32 (p) ==
                    (source == SP_SOURCE_DUPLEX ? 4 :
                     source == SP_SOURCE_BACK ? 5 : 6));
            assert (sanei_sp1130n_le32 (p + 8) == width
                    && sanei_sp1130n_le32 (p + 12) == width * 3
                    && sanei_sp1130n_le32 (p + 20) == g.left
                    && sanei_sp1130n_le32 (p + 28) == width
                    && sanei_sp1130n_le32 (p + 32) == height
                    && sanei_sp1130n_le32 (p + 36) == width
                    && sanei_sp1130n_le32 (p + 40) == height
                    && sanei_sp1130n_le32 (p + 72) == height);
            s.left_mm = s.width_mm - 1;
            s.top_mm = s.height_mm - 1;
            assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_OK);
            assert (g.width > 0 && g.height > 0
                    && g.left + g.width <= feeder
                    && g.top + g.height <= g.page_height);
          }
      s.width_mm = 210;
      s.left_mm = s.top_mm = 0;
      memset (p, 0xa5, sizeof p);
      assert (sanei_sp1130n_network_parameters (p, &s) == SP_OK);
      unsigned lan_height = (297 * dpi * 10 + 253) / 254;
      assert (sanei_sp1130n_le16 (p + 16) == dpi
              && sanei_sp1130n_le16 (p + 18) == dpi
              && sanei_sp1130n_le32 (p + 8) == 210 * dpi * 10 / 254
              && sanei_sp1130n_le32 (p + 20) == 0
              && sanei_sp1130n_le32 (p + 32) == lan_height
              && sanei_sp1130n_le32 (p + 40) == lan_height
              && sanei_sp1130n_le32 (p + 72) == lan_height
              && sanei_sp1130n_le32 (p) == 4 && p[60] == 0x75
              && p[67] == 4 && sanei_sp1130n_le32 (p + 100) == 0);
    }
  const unsigned invalid[] = { 0, 49, 601, 1200, UINT_MAX };
  for (unsigned n = 0; n < sizeof invalid / sizeof invalid[0]; ++n)
    {
      s.dpi = invalid[n];
      struct sanei_sp1130n_geometry before = g;
      memset (p, 0xa5, sizeof p);
      assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_INVALID
              && !memcmp (&g, &before, sizeof g));
      assert (sanei_sp1130n_parameters (p, &s) == SP_INVALID);
      assert (sanei_sp1130n_network_parameters (p, &s) == SP_INVALID);
      for (unsigned i = 0; i < sizeof p; ++i)
        assert (p[i] == 0xa5);
      struct replay r = { 0 };
      struct sanei_sp1130n_io io = { .context = &r, .bulk = transfer };
      struct sanei_sp1130n_sheet sheet;
      uint8_t token[40] = { 0 };
      assert (sanei_sp1130n_scan_sheet (&io, &s, token, &sheet) == SP_INVALID
              && !r.index);
    }
}

static void
geometry_tests (void)
{
  struct sanei_sp1130n_settings s = {.dpi = 300,.width_mm = 210,.height_mm =
      297,
    .source = SP_SOURCE_BACK,.left_mm = 10,.top_mm = 20,
    .scan_width_mm = 100,.scan_height_mm = 50
  };
  uint8_t p[SP_PARAM_SIZE];
  struct sanei_sp1130n_geometry g;
  assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_OK);
  assert (g.width == 1181 && g.height == 590 && g.left == 153
          && g.top == 236);
  assert (g.page_width == 2480 && g.page_height == 3507);
  assert (sanei_sp1130n_parameters (p, &s) == SP_OK
          && sanei_sp1130n_le32 (p) == 5);
  assert (sanei_sp1130n_le32 (p + 8) == 1181
          && sanei_sp1130n_le32 (p + 12) == 3543);
  assert (sanei_sp1130n_le32 (p + 20) == 153
          && sanei_sp1130n_le32 (p + 24) == 236);
  assert (sanei_sp1130n_le32 (p + 28) == 1181
          && sanei_sp1130n_le32 (p + 32) == 590);
  assert (sanei_sp1130n_le32 (p + 36) == 2480
          && sanei_sp1130n_le32 (p + 40) == 3507);
  assert (sanei_sp1130n_le32 (p + 72) == 590);
  s.scan_width_mm = s.scan_height_mm = 0;
  assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_OK && g.width == 2362
          && g.height == 3271);
  const struct sanei_sp1130n_settings valid = s;
  for (unsigned scenario = 0; scenario < 8; ++scenario)
    {
      s = valid;
      switch (scenario)
        {
        case 0:
          s.left_mm = 210;
          break;
        case 1:
          s.top_mm = 297;
          break;
        case 2:
          s.scan_width_mm = 201;
          break;
        case 3:
          s.scan_height_mm = 278;
          break;
        case 4:
          s.left_mm = UINT_MAX;
          break;
        case 5:
          s.scan_width_mm = UINT_MAX;
          break;
        case 6:
          s.source = (enum sanei_sp1130n_source) -1;
          break;
        case 7:
          s.source = (enum sanei_sp1130n_source) 3;
          break;
        }
      memset (p, 0xa5, sizeof p);
      struct sanei_sp1130n_geometry before = g;
      assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_INVALID
              && !memcmp (&g, &before, sizeof g));
      assert (sanei_sp1130n_parameters (p, &s) == SP_INVALID);
      for (size_t i = 0; i < sizeof p; ++i)
        assert (p[i] == 0xa5);
      struct replay r = { 0 };
      struct sanei_sp1130n_io io = {.context = &r,.bulk = transfer };
      struct sanei_sp1130n_sheet sheet;
      uint8_t token[40] = { 0 };
      assert (sanei_sp1130n_scan_sheet (&io, &s, token, &sheet) == SP_INVALID
              && !r.index);
    }
  s = valid;
  s.left_mm = s.top_mm = 0;
  s.width_mm = 216;
  for (unsigned dpi = 300; dpi <= 600; dpi *= 2)
    {
      s.dpi = dpi;
      unsigned feeder = dpi == 300 ? 2550 : 5100;
      assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_OK
              && g.width == feeder && g.page_width == feeder && !g.left);
      s.left_mm = 215;
      assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_OK
              && g.left + g.width == feeder && g.width > 0);
      s.left_mm = 0;
    }
  s = valid;
  s.dpi = 600;
  s.left_mm = 209;
  s.top_mm = 296;
  assert (sanei_sp1130n_scan_geometry (&s, &g) == SP_OK && g.width == 23
          && g.height == 23);
  assert (sanei_sp1130n_scan_geometry (NULL, &g) == SP_INVALID
          && sanei_sp1130n_scan_geometry (&s, NULL) == SP_INVALID);
}

static void
network_parameter_tests (void)
{
  size_t jpeg_size;
  uint8_t *image = jpeg (0, &jpeg_size);
  unsigned width = 0, height = 0;
  assert (sanei_sp1130n_jpeg_geometry (image, jpeg_size, &width, &height) ==
          SP_OK && width == 2 && height == 2);
  for (size_t size = 0; size < 4; ++size)
    {
      width = height = 77;
      assert (sanei_sp1130n_jpeg_geometry (image, size, &width, &height) ==
              SP_IO && width == 77 && height == 77);
    }
  const uint8_t invalid[] = { 0xff, 0xd8, 0xff, 0xef, 0xff, 0xff, 0 };
  assert (sanei_sp1130n_jpeg_geometry (invalid, sizeof invalid,
                                       &width, &height) == SP_IO);
  free (image);
  struct sanei_sp1130n_settings s = {
    .dpi = 300,.width_mm = 210,.height_mm = 297,.source = SP_SOURCE_DUPLEX
  };
  uint8_t p[SP_PARAM_SIZE];
  assert (sanei_sp1130n_network_parameters (p, &s) == SP_OK);
  assert (sanei_sp1130n_le32 (p) == 4 && p[60] == 0x75 && p[67] == 4);
  assert (sanei_sp1130n_le32 (p + 20) == 0 &&
          sanei_sp1130n_le32 (p + 24) == 0);
  assert (sanei_sp1130n_le32 (p + 8) == 2480 &&
          sanei_sp1130n_le32 (p + 12) == 7440);
  assert (sanei_sp1130n_le32 (p + 28) == 2480 &&
          sanei_sp1130n_le32 (p + 32) == 3508 &&
          sanei_sp1130n_le32 (p + 36) == 2480 &&
          sanei_sp1130n_le32 (p + 40) == 3508 &&
          sanei_sp1130n_le32 (p + 72) == 3508);
  assert (sanei_sp1130n_le32 (p + 100) == 0);
  uint8_t duplex[SP_PARAM_SIZE];
  memcpy (duplex, p, sizeof p);
  for (unsigned source = SP_SOURCE_FRONT; source <= SP_SOURCE_BACK; ++source)
    {
      s.source = (enum sanei_sp1130n_source) source;
      assert (sanei_sp1130n_network_parameters (p, &s) == SP_OK);
      assert (!memcmp (p, duplex, sizeof p));
    }
  struct sanei_sp1130n_settings valid = s;
  for (unsigned scenario = 0; scenario < 6; ++scenario)
    {
      s = valid;
      switch (scenario)
        {
        case 0:
          s.width_mm = 209;
          break;
        case 1:
          s.width_mm = 216;
          break;
        case 2:
          s.height_mm = 356;
          break;
        case 3:
          s.left_mm = 1;
          break;
        case 4:
          s.top_mm = 1;
          break;
        case 5:
          s.scan_height_mm = 100;
          break;
        }
      memset (p, 0xa5, sizeof p);
      assert (sanei_sp1130n_network_parameters (p, &s) == SP_UNSUPPORTED);
      for (unsigned i = 0; i < sizeof p; ++i)
        assert (p[i] == 0xa5);
    }
  struct sanei_sp1130n_sheet sheet = { 0 };
  struct sanei_sp1130n_session session = { 0 };
  struct sanei_sp1130n_io io = {.network = 1 };
  const uint8_t token[40] = { 0 };
  assert (sanei_sp1130n_scan_sheet (&io, &valid, token, &sheet) ==
          SP_INVALID);
  assert (!sheet.count && !sheet.page[0].data && !sheet.page[1].data);
  assert (sanei_sp1130n_scan_next (NULL, &valid, token, &session, &sheet) ==
          SP_INVALID);
}

static void
maintenance_tests (void)
{
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000004";
  uint8_t snapshot[SP_NVRAM_SIZE], output[SP_NVRAM_SIZE];
  for (unsigned i = 0; i < sizeof snapshot; ++i)
    snapshot[i] = (uint8_t) i;
  const int expected[] = {
    SP_OK, SP_OK, SP_IO, SP_INVALID, SP_CANCELLED, SP_CANCELLED,
    SP_IO, SP_BUSY, SP_INVALID, SP_BUSY, SP_BUSY, SP_CANCELLED,
    SP_CANCELLED, SP_INVALID, SP_CANCELLED
  };
  for (unsigned scenario = 0; scenario < sizeof expected / sizeof expected[0];
       ++scenario)
    {
      struct replay r = {.chunk = scenario == 1 ? 7 : 0 };
      atomic_init (&r.cancelled, scenario == 12);
      uint8_t reply[108] = { 0 };
      memset (output, 0xa5, sizeof output);
      if (scenario != 12)
        {
          if (scenario == 9)
            sanei_sp1130n_put32 (reply + 32, 1);
          if (scenario == 10)
            sanei_sp1130n_put32 (reply, 0xb);
          command (&r, 0x12, 0x7a, 108, 0, reply, sizeof reply);
          if (scenario == 11)
            r.events[r.count - 1].cancel_after = 1;
        }
      if (scenario < 9 || scenario >= 13)
        {
          memset (reply, 0, sizeof reply);
          if (scenario == 13)
            sanei_sp1130n_put32 (reply, 0x20);
          command (&r, 0x2a, 0x78, 1, 40, reply, 32);
          if (scenario != 13)
            {
              append (&r, 0, token, sizeof token);
              if (scenario == 14)
                r.events[r.count - 1].cancel_after = 1;
              memset (reply, 0, sizeof reply);
              if (scenario == 8)
                sanei_sp1130n_put32 (reply, 0x20);
              command (&r, 0x28, 0x78, 1, 40, reply, 32);
              if (scenario != 8)
                {
                  uint8_t echo[40];
                  memcpy (echo, token, sizeof echo);
                  if (scenario == 7)
                    echo[0] ^= 1;
                  append (&r, 1, echo, sizeof echo);
                  if (scenario == 4)
                    r.events[r.count - 1].cancel_after = 1;
                }
              if (scenario < 7 && scenario != 4)
                {
                  memset (reply, 0, sizeof reply);
                  if (scenario == 3)
                    sanei_sp1130n_put32 (reply, 0x20);
                  command (&r, 0x28, 0x10, 0, SP_NVRAM_SIZE, reply, 32);
                  if (scenario != 3)
                    {
                      append (&r, 1, snapshot, sizeof snapshot);
                      if (scenario == 2)
                        r.events[r.count - 1].status = SP_IO;
                      if (scenario == 5)
                        r.events[r.count - 1].cancel_after = 1;
                    }
                }
              if (scenario != 7)
                {
                  memset (reply, 0, sizeof reply);
                  if (scenario == 6)
                    sanei_sp1130n_put32 (reply, 0x1000);
                  command (&r, 0x2a, 0x78, 2, 40, reply, 32);
                  if (scenario != 6)
                    append (&r, 0, token, sizeof token);
                }
            }
        }
      struct sanei_sp1130n_io io = {.context = &r,.bulk =
          transfer,.cancelled = &r.cancelled
      };
      assert (sanei_sp1130n_read_nvram (&io, token, output) ==
              expected[scenario]);
      if (expected[scenario] == SP_OK)
        assert (!memcmp (output, snapshot, sizeof output));
      else
        for (unsigned i = 0; i < sizeof output; ++i)
          assert (output[i] == 0xa5);
      release (&r);
    }
  struct replay r = { 0 };
  struct sanei_sp1130n_io io = {.context = &r,.bulk = transfer }, invalid =
    { 0 };
  assert (sanei_sp1130n_read_nvram (NULL, token, output) == SP_INVALID);
  assert (sanei_sp1130n_read_nvram (&invalid, token, output) == SP_INVALID);
  assert (sanei_sp1130n_read_nvram (&io, NULL, output) == SP_INVALID);
  assert (sanei_sp1130n_read_nvram (&io, token, NULL) == SP_INVALID);
  release (&r);
}

/* Consecutive empty USB image replies back off instead of spinning, and a
 * cancellation during that backoff still drains nothing extra. */
static void
usb_empty_reply_backoff_tests (void)
{
  const uint8_t token[40] = "00000000-0000-4000-8000-00000000000e";
  const struct sanei_sp1130n_settings settings = {
    .dpi = 300, .width_mm = 210, .height_mm = 297, .source = SP_SOURCE_DUPLEX
  };
  size_t sizes[2];
  uint8_t *images[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  const unsigned empties = 8;
  struct replay r = { .chunk = 7 };
  atomic_init (&r.cancelled, 0);
  startup (&r, &settings, token);
  for (unsigned n = 0; n < empties; ++n)
    block (&r, 0, NULL, 0, 0, 0, 0);
  block (&r, 0, images[0], sizes[0], 0x1005, 2, 2);
  block (&r, 1, images[1], sizes[1], 0x1005, 2, 2);
  uint8_t event[64] = { 0 };
  event[6] = 4;
  append (&r, 1, event, sizeof event);
  cleanup (&r, token);
  struct sanei_sp1130n_io io = {
    .context = &r, .bulk = transfer, .cancelled = &r.cancelled
  };
  struct sanei_sp1130n_session session = { 0 };
  struct sanei_sp1130n_sheet sheet;
  struct timespec t0, t1;
  clock_gettime (CLOCK_MONOTONIC, &t0);
  assert (sanei_sp1130n_scan_next (&io, &settings, token, &session, &sheet)
          == SP_OK && sheet.count == 2);
  clock_gettime (CLOCK_MONOTONIC, &t1);
  long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  /* Three immediate retries, then about 100 ms per further empty reply. */
  assert (ms >= (long) (empties - 3) * 90);
  sanei_sp1130n_sheet_free (&sheet);
  assert (sanei_sp1130n_scan_finish (&io, token, &session) == SP_OK);
  release (&r);

  /* Inject cancellation after the first sleep slice, not before the pause. */
  struct replay c = { .chunk = 7 };
  atomic_init (&c.cancelled, 0);
  startup (&c, &settings, token);
  for (unsigned n = 0; n < 4; ++n)
    block (&c, 0, NULL, 0, 0, 0, 0);
  cleanup (&c, token);
  io.context = &c;
  io.cancelled = &c.cancelled;
  session = (struct sanei_sp1130n_session) { 0 };
  pause_slices = 0;
  cancel_during_pause = &c;
  clock_gettime (CLOCK_MONOTONIC, &t0);
  assert (sanei_sp1130n_scan_next (&io, &settings, token, &session, &sheet)
          == SP_CANCELLED && !sheet.count);
  clock_gettime (CLOCK_MONOTONIC, &t1);
  cancel_during_pause = NULL;
  ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  assert (pause_slices == 1 && ms < 1000
          && !session.own_lock && !session.started && !session.stop_pending);
  release (&c);
  free (images[0]);
  free (images[1]);
  puts ("USB empty-reply backoff checks passed: bounded pacing and prompt cancellation.");
}

int
main (void)
{
  assert (sanei_sp1130n_usb_matches (0x05ca, 0x031e));
  assert (sanei_sp1130n_device_error (0x21) == SP_IO);
  assert (sanei_sp1130n_device_error (0x4003) == SP_IO);
  assert (sanei_sp1130n_usb_matches (0x04c5, 0x1627));
  assert (!sanei_sp1130n_usb_matches (0x05ca, 0x1627));
  assert (!sanei_sp1130n_usb_matches (0x04c5, 0x031e));
  assert (!sanei_sp1130n_usb_matches (0x04c5, 0x1626));
  assert (!sanei_sp1130n_usb_matches (0, 0));
  uint8_t h[16];
  const uint8_t expected[16] =
    { 0x30, 2, 0, 0, 0x2a, 0, 9, 0, 4, 0, 0, 0, 4, 1, 0, 0 };
  sanei_sp1130n_header (h, 0x2a, 9, 4, 260);
  assert (!memcmp (h, expected, sizeof h));
  uint8_t p[SP_PARAM_SIZE];
  struct sanei_sp1130n_settings s = {.dpi = 300,.width_mm = 210,.height_mm =
      297,
    .source = SP_SOURCE_DUPLEX
  };
  assert (sanei_sp1130n_parameters (p, &s) == SP_OK);
  assert (sanei_sp1130n_le32 (p) == 4 && sanei_sp1130n_le32 (p + 8) == 2480);
  assert (sanei_sp1130n_le32 (p + 32) == 3507
          && sanei_sp1130n_le16 (p + 16) == 300);
  s.source = SP_SOURCE_FRONT;
  assert (sanei_sp1130n_parameters (p, &s) == SP_OK
          && sanei_sp1130n_le32 (p) == 6);
  s.dpi = 600;
  assert (sanei_sp1130n_parameters (p, &s) == SP_OK
          && sanei_sp1130n_le32 (p + 8) == 4960);
  s.dpi = 49;
  assert (sanei_sp1130n_parameters (p, &s) == SP_INVALID);
  struct sanei_sp1130n_io io = {.bulk = stalled };
  assert (sanei_sp1130n_exact (&io, 1, h, 16, 5000) == SP_IO);
  io.context = &io;
  assert (sanei_sp1130n_exact (&io, 1, h, 16, 5000) == SP_IO);
  atomic_bool stop;
  atomic_init (&stop, 1);
  io.cancelled = &stop;
  assert (sanei_sp1130n_exact (&io, 1, h, 16, 5000) == SP_CANCELLED);
  const unsigned trial_dpi[] =
    { 50, 51, 75, 100, 150, 200, 299, 300, 301, 450, 599, 600 };
  for (unsigned n = 0; n < sizeof trial_dpi / sizeof trial_dpi[0]; ++n)
    for (unsigned first = 0; first < 2; ++first)
      for (int interleave = 0; interleave < 2; ++interleave)
        for (int partial = 0; partial < 2; ++partial)
          check_duplex (trial_dpi[n], first, interleave, partial);
  failures ();
  usb_session_tests ();
  backend_usb_session_tests ();
  backend_usb_first_sheet_tests ();
  backend_usb_idle_cancel_tests ();
  usb_inquiry_timeout_tests ();
  usb_image_timeout_tests ();
  usb_image_recovery_bounds_tests ();
  usb_cleanup_tests ();
  usb_rejected_payload_tests ();
  usb_transaction_cancellation_tests ();
  usb_empty_reply_backoff_tests ();
  jpeg_restart_tests ();
  simplex_tests ();
  network_parameter_tests ();
  resolution_tests ();
  geometry_tests ();
  maintenance_tests ();
  struct sanei_sp1130n_page page;
  uint8_t malformed[4] = { 0 };
  assert (sanei_sp1130n_decode_jpeg (malformed, sizeof malformed, 2, 2, &page)
          == SP_IO);
  assert (sanei_sp1130n_decode_jpeg
          (malformed, sizeof malformed, 10000, 20000, &page) == SP_INVALID);
  size_t size;
  uint8_t *j = jpeg (0, &size);
  assert (sanei_sp1130n_decode_jpeg (j, size - 2, 2, 2, &page) == SP_IO);
  free (j);
  puts
    ("Protocol tests passed: 50-600-dpi encoding and bounds, 1200-dpi rejection, USB completion versus paper status, 300/600-dpi startup wait, duplex/front/back, windows, read-only maintenance, wire ordering, cleanup, cancellation, JPEG errors.");
  return 0;
}
