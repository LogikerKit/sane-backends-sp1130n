/* SPDX-License-Identifier: MIT */
#include "sp1130n.h"
#include <assert.h>
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

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
};
struct replay
{
  struct event events[128];
  size_t count, index;
  atomic_bool cancelled;
  size_t chunk;
};
static void
append (struct replay *r, int input, const uint8_t *data, size_t size)
{
  assert (r->count < 128);
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
  if (r->index == r->count)
    return SP_IO;
  struct event *e = &r->events[r->index];
  assert (e->input == input);
  assert (size <= e->size - e->done || e->status);
  if (e->status)
    {
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
    }
  return SP_OK;
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
check_duplex (unsigned first, int interleave, int partial)
{
  struct replay r = {.chunk = partial ? 7 : 0 };
  atomic_init (&r.cancelled, 0);
  const struct sanei_sp1130n_settings s = {.dpi = 300,.width_mm =
      210,.height_mm = 297,
    .source = SP_SOURCE_DUPLEX
  };
  const uint8_t token[40] = "00000000-0000-4000-8000-000000000001";
  size_t sizes[2];
  uint8_t *j[2] = { jpeg (0, &sizes[0]), jpeg (1, &sizes[1]) };
  startup (&r, &s, token);
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
          s.dpi = 600;
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

int
main (void)
{
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
  s.dpi = 200;
  assert (sanei_sp1130n_parameters (p, &s) == SP_INVALID);
  struct sanei_sp1130n_io io = {.bulk = stalled };
  assert (sanei_sp1130n_exact (&io, 1, h, 16, 5000) == SP_IO);
  io.context = &io;
  assert (sanei_sp1130n_exact (&io, 1, h, 16, 5000) == SP_IO);
  atomic_bool stop;
  atomic_init (&stop, 1);
  io.cancelled = &stop;
  assert (sanei_sp1130n_exact (&io, 1, h, 16, 5000) == SP_CANCELLED);
  for (unsigned first = 0; first < 2; ++first)
    for (int interleave = 0; interleave < 2; ++interleave)
      for (int partial = 0; partial < 2; ++partial)
        check_duplex (first, interleave, partial);
  failures ();
  usb_cleanup_tests ();
  usb_rejected_payload_tests ();
  usb_transaction_cancellation_tests ();
  jpeg_restart_tests ();
  simplex_tests ();
  network_parameter_tests ();
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
    ("Protocol tests passed: duplex/front/back, windows, read-only maintenance, wire ordering, cleanup, cancellation, bounds, JPEG errors.");
  return 0;
}
