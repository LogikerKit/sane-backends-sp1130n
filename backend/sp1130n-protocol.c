/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#define _POSIX_C_SOURCE 200809L
#include "../include/sane/config.h"
#include "sp1130n.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint16_t
sanei_sp1130n_le16 (const uint8_t *p)
{
  return (uint16_t) (p[0] | (uint16_t) p[1] << 8);
}

uint32_t
sanei_sp1130n_le32 (const uint8_t *p)
{
  return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 |
    (uint32_t) p[3] << 24;
}

void
sanei_sp1130n_put16 (uint8_t *p, uint16_t n)
{
  p[0] = (uint8_t) n;
  p[1] = (uint8_t) (n >> 8);
}

void
sanei_sp1130n_put32 (uint8_t *p, uint32_t n)
{
  for (unsigned i = 0; i < 4; ++i)
    p[i] = (uint8_t) (n >> (8 * i));
}

void
sanei_sp1130n_header (uint8_t h[16], uint8_t op, uint8_t sub,
                      uint16_t selector, uint32_t value)
{
  memset (h, 0, 16);
  sanei_sp1130n_put32 (h, 0x230);
  h[4] = op;
  h[6] = sub;
  sanei_sp1130n_put16 (h + 8, selector);
  sanei_sp1130n_put32 (h + 12, value);
}

int
sanei_sp1130n_parameters (uint8_t p[SP_PARAM_SIZE],
                          const struct sanei_sp1130n_settings *s)
{
  struct sanei_sp1130n_geometry g;
  if (!p || sanei_sp1130n_scan_geometry (s, &g) != SP_OK)
    return SP_INVALID;
  memset (p, 0, SP_PARAM_SIZE);
  sanei_sp1130n_put32 (p,
                       s->source == SP_SOURCE_DUPLEX ? 4 : s->source ==
                       SP_SOURCE_BACK ? 5 : 6);
  p[4] = p[5] = 3;
  p[6] = p[7] = 24;
  sanei_sp1130n_put32 (p + 8, g.width);
  sanei_sp1130n_put32 (p + 12, g.width * 3);
  sanei_sp1130n_put16 (p + 16, (uint16_t) s->dpi);
  sanei_sp1130n_put16 (p + 18, (uint16_t) s->dpi);
  sanei_sp1130n_put32 (p + 20, g.left);
  sanei_sp1130n_put32 (p + 24, g.top);
  sanei_sp1130n_put32 (p + 28, g.width);
  sanei_sp1130n_put32 (p + 32, g.height);
  sanei_sp1130n_put32 (p + 36, g.page_width);
  sanei_sp1130n_put32 (p + 40, g.page_height);
  sanei_sp1130n_put16 (p + 44, 100);
  sanei_sp1130n_put16 (p + 46, 100);
  sanei_sp1130n_put16 (p + 52, 4);
  sanei_sp1130n_put16 (p + 54, 31);
  sanei_sp1130n_put32 (p + 56, 1);
  p[63] = 128;
  sanei_sp1130n_put32 (p + 72, g.height);
  sanei_sp1130n_put32 (p + 76, 32);
  sanei_sp1130n_put32 (p + 100, 1);
  sanei_sp1130n_put32 (p + 132, 100);
  return SP_OK;
}

int
sanei_sp1130n_network_parameters (uint8_t p[SP_PARAM_SIZE],
                                  const struct sanei_sp1130n_settings *s)
{
  struct sanei_sp1130n_geometry g;
  if (!p || sanei_sp1130n_scan_geometry (s, &g) != SP_OK)
    return SP_INVALID;
  /* Do not extrapolate feed parameters to untested resolutions or windows. */
  if (s->dpi != 300 || s->width_mm != 210 || s->height_mm != 297 ||
      s->left_mm || s->top_mm ||
      (s->scan_width_mm && s->scan_width_mm != 210) ||
      (s->scan_height_mm && s->scan_height_mm != 297))
    return SP_UNSUPPORTED;
  sanei_sp1130n_parameters (p, s);
  sanei_sp1130n_put32 (p, 4);   /* Acquire both sides; select the output on the host. */
  sanei_sp1130n_put32 (p + 20, 0);
  sanei_sp1130n_put32 (p + 32, 3508);
  sanei_sp1130n_put32 (p + 40, 3508);
  sanei_sp1130n_put32 (p + 72, 3508);
  p[60] = 0x75;
  p[67] = 4;
  sanei_sp1130n_put32 (p + 100, 0);
  return SP_OK;
}

static uint64_t
now_ms (void)
{
  struct timespec t;
  if (clock_gettime (CLOCK_MONOTONIC, &t))
    return 0;
  return (uint64_t) t.tv_sec * 1000 + (unsigned long) t.tv_nsec / 1000000;
}

static int
cancelled (const struct sanei_sp1130n_io *io)
{
  return io->cancelled && atomic_load (io->cancelled);
}

int
sanei_sp1130n_exact (struct sanei_sp1130n_io *io, int input, uint8_t *data,
                     size_t size, unsigned timeout)
{
  if (!io || !io->bulk || (!data && size) || !timeout)
    return SP_INVALID;
  uint64_t deadline = now_ms () + timeout;
  size_t done = 0;
  unsigned calls = 0;
  while (done < size)
    {
      if (cancelled (io))
        return SP_CANCELLED;
      uint64_t now = now_ms ();
      if (now >= deadline || ++calls > 65536)
        return SP_IO;
      size_t chunk = size - done;
      if (chunk > 1024 * 1024)
        chunk = 1024 * 1024;
      size_t actual = 0;
      int status = io->bulk (io->context, input, data + done, chunk, &actual,
                             (unsigned) (deadline - now));
      if (cancelled (io))
        return SP_CANCELLED;
      if (status != SP_OK)
        return status == SP_EOF && done ? SP_IO : status;
      if (!actual || actual > chunk)
        return SP_IO;
      done += actual;
    }
  return cancelled (io) ? SP_CANCELLED : SP_OK;
}

int
sanei_sp1130n_command (struct sanei_sp1130n_io *io, uint8_t op, uint8_t sub,
                       uint16_t sel, uint32_t value, uint8_t *reply,
                       size_t reply_size)
{
  if (!io || !io->bulk || (reply_size && !reply))
    return SP_INVALID;
  if (cancelled (io))
    return SP_CANCELLED;
  /* USB replies share an input endpoint with later cleanup acknowledgements. */
  struct sanei_sp1130n_io command = *io;
  if (!io->network)
    command.cancelled = NULL;
  if (command.begin_command)
    {
      int status = command.begin_command (&command, op, sub);
      if (status != SP_OK)
        return status;
    }
  uint8_t h[16];
  sanei_sp1130n_header (h, op, sub, sel, value);
  int status = sanei_sp1130n_exact (&command, 0, h, sizeof h, 5000);
  if (status != SP_OK)
    return status;
  if (!reply_size)
    return cancelled (io) ? SP_CANCELLED : SP_OK;
  status = sanei_sp1130n_exact (&command, 1, reply, reply_size, 5000);
  if (status == SP_OK && cancelled (io))
    return SP_CANCELLED;
  return status == SP_EOF ? SP_IO : status;
}

int
sanei_sp1130n_device_error (uint32_t n)
{
  if (!n)
    return SP_OK;
  if (n == 0xb)
    return SP_BUSY;
  if (n == 0xf)
    return SP_UNSUPPORTED;
  if (n == 0x20)
    return SP_INVALID;
  if (n == 0x21 || n == 0x4003)
    return SP_NOMEM;
  if (n >= 0x2002 && n <= 0x2005)
    return SP_JAMMED;
  if (n == 0x2009)
    return SP_COVER_OPEN;
  if (n == 0x200e)
    return SP_NO_DOCS;
  if (n == 0x4001 || n == 0x4002)
    return SP_CANCELLED;
  return SP_IO;
}

const char *
sanei_sp1130n_strstatus (int n)
{
  static const char *const names[] = {
    "Success", "Unsupported operation", "Cancelled", "Device busy",
    "Invalid argument",
    "End of image", "Paper jam", "No documents", "Cover open",
    "I/O or protocol error",
    "Out of memory", "Access denied"
  };
  return n >= 0
    && n <
    (int) (sizeof names / sizeof names[0]) ? names[n] : "Unknown error";
}

int
sanei_sp1130n_probe (struct sanei_sp1130n_io *io, uint8_t reply[108])
{
  return sanei_sp1130n_command (io, 0x12, 0x7a, 108, 0, reply, 108);
}

static int
acknowledged (struct sanei_sp1130n_io *io, uint8_t op, uint8_t sub,
              uint16_t sel, uint32_t value)
{
  uint8_t reply[32] = { 0 };
  int status =
    sanei_sp1130n_command (io, op, sub, sel, value, reply, sizeof reply);
  return status ==
    SP_OK ? sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply)) : status;
}

static int
token_write (struct sanei_sp1130n_io *io, uint16_t selector,
             const uint8_t token[40])
{
  int status = acknowledged (io, 0x2a, 0x78, selector, 40);
  if (status != SP_OK)
    return status;
  return sanei_sp1130n_exact (io, 0, (uint8_t *) token, 40, 5000);
}

int
sanei_sp1130n_read_nvram (struct sanei_sp1130n_io *io,
                          const uint8_t token[40],
                          uint8_t output[SP_NVRAM_SIZE])
{
  if (!io || !io->bulk || !token || !output)
    return SP_INVALID;
  uint8_t reply[108] = { 0 }, echo[40], snapshot[SP_NVRAM_SIZE];
  int status = sanei_sp1130n_probe (io, reply);
  if (status != SP_OK)
    return status;
  status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
  if (status != SP_OK)
    return status;
  if (sanei_sp1130n_le32 (reply + 32))
    return SP_BUSY;
  if (cancelled (io))
    return SP_CANCELLED;
  /* Finish the bounded UUID handshake before honoring cancellation. */
  struct sanei_sp1130n_io handshake = *io;
  handshake.cancelled = NULL;
  status = token_write (&handshake, 1, token);
  if (status != SP_OK)
    return status;
  int own_lock = 1;
  status = acknowledged (&handshake, 0x28, 0x78, 1, 40);
  if (status == SP_OK)
    status = sanei_sp1130n_exact (&handshake, 1, echo, sizeof echo, 5000);
  if (status == SP_OK && memcmp (token, echo, sizeof echo))
    {
      status = SP_BUSY;
      own_lock = 0;
    }
  if (status == SP_OK && cancelled (io))
    status = SP_CANCELLED;
  struct sanei_sp1130n_io snapshot_io = *io;
  if (!io->network)
    snapshot_io.cancelled = NULL;
  if (status == SP_OK)
    status = acknowledged (&snapshot_io, 0x28, 0x10, 0, SP_NVRAM_SIZE);
  if (status == SP_OK)
    status = sanei_sp1130n_exact (&snapshot_io, 1, snapshot, sizeof snapshot,
                                 5000);
  if (own_lock)
    {
      int unlock_status = token_write (&handshake, 2, token);
      if (status == SP_OK)
        status = unlock_status;
    }
  if (status == SP_OK && cancelled (io))
    status = SP_CANCELLED;
  if (status == SP_OK)
    memcpy (output, snapshot, sizeof snapshot);
  return status;
}

void
sanei_sp1130n_sheet_free (struct sanei_sp1130n_sheet *sheet)
{
  if (!sheet)
    return;
  for (unsigned i = 0; i < 2; ++i)
    free (sheet->page[i].data);
  memset (sheet, 0, sizeof *sheet);
}

struct compressed_page
{
  uint8_t *data;
  size_t size;
  unsigned width, height;
  int complete;
};

static int
network_blocks (struct sanei_sp1130n_io *io, struct compressed_page pages[3],
                int *no_paper, unsigned prior_sheets)
{
  unsigned completed = 0;
  int current_side = -1;
  uint64_t deadline = now_ms () + 120000;
  for (unsigned block = 0; block < 65536; ++block)
    {
      if (now_ms () >= deadline)
        return SP_IO;
      uint8_t h[64] = { 0 };
      int status = sanei_sp1130n_command (io, 0x28, 4, 0, 0, NULL, 0);
      if (status != SP_OK)
        return status;
      status = sanei_sp1130n_exact (io, 1, h, 32, 5000);
      if (status != SP_OK)
        {
          /* Only a clean boundary after a complete sheet can end the batch. */
          if (status == SP_EOF && !block && prior_sheets)
            return SP_NO_DOCS;
          return status == SP_EOF ? SP_IO : status;
        }
      uint32_t error = sanei_sp1130n_le32 (h), bytes;
      int short_header = error == 0x80000 &&
        sanei_sp1130n_le32 (h + 4) == 0x80000;
      if (short_header)
        {
          error = 0;
          bytes = 0x80000;
        }
      else
        {
          if (error && error != 0x1005 && error != 0x1006)
            {
              status = sanei_sp1130n_device_error (error);
              return status == SP_NO_DOCS && block ? SP_IO : status;
            }
          status = sanei_sp1130n_exact (io, 1, h + 32, 32, 5000);
          if (status != SP_OK)
            return status == SP_EOF ? SP_IO : status;
          bytes = sanei_sp1130n_le32 (h + 36);
          if (!bytes)
            bytes = sanei_sp1130n_le32 (h + 32);
        }
      if (!bytes && error == 0x1006 && !block)
        return sanei_sp1130n_le32 (h + 16) ||
          sanei_sp1130n_le32 (h + 20) ? SP_IO : SP_NO_DOCS;
      unsigned side;
      if (short_header)
        side = current_side < 0 ? 2u : (unsigned) current_side;
      else
        {
          if (h[11] > 1)
            return SP_IO;
          side = h[11]; /* LAN: 0 front, 1 back; short-form metadata is absent. */
          if (pages[2].size)
            {
              if (pages[side].size || pages[side].complete)
                return SP_IO;
              pages[side] = pages[2];
              pages[2] = (struct compressed_page) { 0 };
            }
          current_side = (int) side;
        }
      struct compressed_page *p = &pages[side];
      if (p->complete)
        return SP_IO;
      unsigned width = short_header ? 0 : sanei_sp1130n_le32 (h + 20);
      unsigned height = short_header ? 0 : sanei_sp1130n_le32 (h + 16);
      if (width || height)
        {
          if (!width || !height || width > 10000 || height > 20000 ||
              (uint64_t) width * height * 3 > SP_PAGE_LIMIT ||
              (p->width && (width != p->width || height != p->height)))
            return SP_IO;
          p->width = width;
          p->height = height;
        }
      if (!short_header && h[48] > 3)
        return SP_UNSUPPORTED;
      if (bytes > SP_COMPRESSED_LIMIT - p->size)
        return SP_NOMEM;
      if (bytes)
        {
          void *larger = realloc (p->data, p->size + bytes);
          if (!larger)
            return SP_NOMEM;
          p->data = larger;
          status =
            sanei_sp1130n_exact (io, 1, p->data + p->size, bytes, 30000);
          if (status != SP_OK)
            return status == SP_EOF ? SP_IO : status;
          p->size += bytes;
        }
      if (!error)
        continue;
      if (!p->size)
        return SP_IO;
      if (!p->width)
        {
          status = sanei_sp1130n_jpeg_geometry (p->data, p->size,
                                                &p->width, &p->height);
          if (status != SP_OK)
            return status;
        }
      p->complete = 1;
      current_side = -1;
      if (error == 0x1006)
        *no_paper = 1;
      if (++completed == 2)
        return SP_OK;           /* LAN has no unsolicited USB end-of-sheet records. */
    }
  return SP_IO;
}

int
sanei_sp1130n_scan_finish (struct sanei_sp1130n_io *io,
                           const uint8_t token[40],
                           struct sanei_sp1130n_session *session)
{
  if (!io || !token || !session)
    return SP_INVALID;
  struct sanei_sp1130n_io cleanup = *io;
  cleanup.cancelled = NULL;
  int status = SP_OK;
  if (session->started && !session->exhausted)
    status = acknowledged (&cleanup, 0x14, 6, 0, 0);
  if (session->own_lock)
    {
      int unlock = token_write (&cleanup, 2, token);
      if (status == SP_OK)
        status = unlock;
    }
  session->own_lock = session->started = 0;
  return status;
}

int
sanei_sp1130n_scan_next (struct sanei_sp1130n_io *io,
                         const struct sanei_sp1130n_settings *settings,
                         const uint8_t token[40],
                         struct sanei_sp1130n_session *session,
                         struct sanei_sp1130n_sheet *sheet)
{
  if (!sheet)
    return SP_INVALID;
  memset (sheet, 0, sizeof *sheet);
  if (!io || !io->bulk || !settings || !token || !session)
    return SP_INVALID;
  if (!io->network)
    return sanei_sp1130n_scan_sheet (io, settings, token, sheet);
  if (session->error)
    return session->error;
  if (session->exhausted)
    return SP_NO_DOCS;
  uint8_t params[SP_PARAM_SIZE], reply[108] = { 0 }, echo[40];
  int status = sanei_sp1130n_network_parameters (params, settings);
  if (status != SP_OK)
    return status;
  struct compressed_page pages[3] = { {0}, {0}, {0} };
  if (!session->started)
    {
      status = sanei_sp1130n_probe (io, reply);
      if (status != SP_OK)
        goto done;
      status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
      if (status != SP_OK)
        goto done;
      if (sanei_sp1130n_le32 (reply + 32))
        {
          status = SP_BUSY;
          goto done;
        }
      if (cancelled (io))
        {
          status = SP_CANCELLED;
          goto done;
        }
      struct sanei_sp1130n_io handshake = *io;
      handshake.cancelled = NULL;
      status = token_write (&handshake, 1, token);
      if (status != SP_OK)
        goto done;
      session->own_lock = 1;
      status = acknowledged (&handshake, 0x28, 0x78, 1, 40);
      if (status == SP_OK)
        status = sanei_sp1130n_exact (&handshake, 1, echo, sizeof echo, 5000);
      if (status == SP_OK && memcmp (token, echo, sizeof echo))
        {
          status = SP_BUSY;
          session->own_lock = 0;
        }
      if (status == SP_OK && cancelled (io))
        status = SP_CANCELLED;
      if (status != SP_OK)
        goto done;
      status = sanei_sp1130n_command (io, 0x12, 2, 32, 0, reply, 32);
      if (status == SP_OK)
        status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
      /* LAN sensor layout is not the USB layout; rely on device error codes. */
      if (status == SP_OK)
        status = acknowledged (io, 0x2a, 9, 4, SP_PARAM_SIZE);
      if (status == SP_OK)
        status = sanei_sp1130n_exact (io, 0, params, sizeof params, 5000);
      if (status == SP_OK)
        status = acknowledged (io, 0x14, 5, 0, 0);
      if (status != SP_OK)
        goto done;
      session->started = 1;
      status = acknowledged (io, 0x14, 4, 0, 0);
      if (status != SP_OK)
        goto done;
    }
  int no_paper = 0;
  status = network_blocks (io, pages, &no_paper, session->sheets);
  if (status == SP_OK)
    {
      /* Decode even discarded sides: truncated duplex must never become success. */
      sheet->count = 2;
      for (unsigned side = 0; side < 2 && status == SP_OK; ++side)
        {
          status =
            sanei_sp1130n_decode_jpeg (pages[side].data, pages[side].size,
                                       pages[side].width, pages[side].height,
                                       &sheet->page[side]);
          if (status == SP_OK && cancelled (io))
            status = SP_CANCELLED;
        }
      if (status == SP_OK)
        {
          ++session->sheets;
          session->exhausted = no_paper;
          if (settings->source != SP_SOURCE_DUPLEX)
            {
              unsigned selected = settings->source == SP_SOURCE_BACK ? 1 : 0;
              free (sheet->page[1 - selected].data);
              sheet->page[0] = sheet->page[selected];
              sheet->page[1] = (struct sanei_sp1130n_page)
              { 0 };
              sheet->count = 1;
            }
        }
    }
  else if (status == SP_NO_DOCS)
    session->exhausted = 1;
done:
  if (status != SP_OK || session->exhausted)
    {
      int cleanup = sanei_sp1130n_scan_finish (io, token, session);
      if (status == SP_OK)
        status = cleanup;
    }
  for (unsigned i = 0; i < 3; ++i)
    free (pages[i].data);
  if (status != SP_OK)
    {
      if (status != SP_NO_DOCS)
        session->error = status;
      sanei_sp1130n_sheet_free (sheet);
    }
  return status;
}

static int
discard_error_payload (struct sanei_sp1130n_io *io, uint32_t bytes,
                       uint64_t deadline)
{
  if (bytes > SP_COMPRESSED_LIMIT)
    return SP_IO;
  struct sanei_sp1130n_io drain = *io;
  drain.cancelled = NULL;
  uint8_t buffer[4096];
  uint64_t limit = now_ms () + 30000;
  if (deadline > limit)
    deadline = limit;
  while (bytes)
    {
      uint64_t now = now_ms ();
      if (now >= deadline)
        return SP_IO;
      size_t size = bytes < sizeof buffer ? bytes : sizeof buffer;
      int status = sanei_sp1130n_exact (&drain, 1, buffer, size,
                                       (unsigned) (deadline - now));
      if (status != SP_OK)
        return status == SP_EOF ? SP_IO : status;
      bytes -= (uint32_t) size;
    }
  return SP_OK;
}

static int
exact_before (struct sanei_sp1130n_io *io, int input, uint8_t *data,
              size_t size, unsigned timeout, uint64_t deadline)
{
  uint64_t now = now_ms ();
  if (now >= deadline)
    return SP_IO;
  if (deadline - now < timeout)
    timeout = (unsigned) (deadline - now);
  return sanei_sp1130n_exact (io, input, data, size, timeout);
}

static int
completion_records (struct sanei_sp1130n_io *io, uint64_t deadline,
                    int *consumed)
{
  *consumed = 0;
  uint8_t h[64];
  for (unsigned event = 0; event < 32; ++event)
    {
      int status = exact_before (io, 1, h, sizeof h, 5000, deadline);
      if (status != SP_OK)
        return status;
      uint32_t error = sanei_sp1130n_le32 (h);
      if (error && error != 0x1005 && error != 0x1006)
        {
          status = discard_error_payload (io, sanei_sp1130n_le32 (h + 36),
                                           deadline);
          *consumed = status == SP_OK;
          return status == SP_OK ? sanei_sp1130n_device_error (error) : status;
        }
      if (h[6] == 2 || h[6] == 4)
        {
          *consumed = 1;
          return SP_OK;
        }
    }
  return SP_IO;
}

static int
reject_payload (struct sanei_sp1130n_io *io, uint32_t bytes,
                uint64_t deadline, int reason, int final_side)
{
  /* Do not trust or drain an oversized declaration, even after a local fault. */
  if (bytes > SP_COMPRESSED_LIMIT)
    return reason;
  int status = discard_error_payload (io, bytes, deadline);
  if (status == SP_OK && final_side)
    {
      int consumed;
      status = completion_records (io, deadline, &consumed);
      if (consumed)
        status = SP_OK;
    }
  return status == SP_OK ? reason : status;
}

static int
image_blocks (struct sanei_sp1130n_io *io, enum sanei_sp1130n_source source,
              struct compressed_page pages[2])
{
  /* An accepted USB block request must consume its full header and payload
   * before STOP/unlock can reuse bulk-IN. Honor cancellation between blocks. */
  struct sanei_sp1130n_io packet = *io;
  packet.cancelled = NULL;
  unsigned completed = 0;
  uint64_t deadline = now_ms () + 120000;
  for (unsigned block = 0; block < 65536; ++block)
    {
      if (now_ms () >= deadline)
        return SP_IO;
      if (cancelled (io))
        return SP_CANCELLED;
      uint8_t request[16], h[64] = { 0 };
      sanei_sp1130n_header (request, 0x28, 4, 0, 0);
      int status = exact_before (&packet, 0, request, sizeof request, 5000,
                                 deadline);
      if (status == SP_OK)
        status = exact_before (&packet, 1, h, sizeof h, 5000, deadline);
      if (status != SP_OK)
        return status;
      uint32_t error = sanei_sp1130n_le32 (h);
      uint32_t bytes = sanei_sp1130n_le32 (h + 36);
      if (error && error != 0x1005 && error != 0x1006)
        {
          /* Keep payload bytes from becoming STOP or unlock acknowledgements. */
          status = discard_error_payload (&packet,
                                           sanei_sp1130n_le32 (h + 36),
                                           deadline);
          return status == SP_OK ? (cancelled (io) ? SP_CANCELLED :
                                    sanei_sp1130n_device_error (error)) : status;
        }
      if (h[11] > 1 || (source == SP_SOURCE_FRONT && h[11] != 1) ||
          (source == SP_SOURCE_BACK && h[11] != 0))
        return reject_payload (&packet, bytes, deadline, SP_IO, 0);
      unsigned side = h[11] == 1 ? 0 : 1;
      struct compressed_page *p = &pages[side];
      if (p->complete)
        return reject_payload (&packet, bytes, deadline, SP_IO, 0);
      int final_side = error &&
        completed + 1 == (source == SP_SOURCE_DUPLEX ? 2u : 1u);
      if (bytes > SP_COMPRESSED_LIMIT - p->size)
        return reject_payload (&packet, bytes, deadline, SP_NOMEM, final_side);
      if (bytes)
        {
          void *larger = realloc (p->data, p->size + bytes);
          if (!larger)
            return reject_payload (&packet, bytes, deadline, SP_NOMEM,
                                    final_side);
          p->data = larger;
          status =
            exact_before (&packet, 1, p->data + p->size, bytes, 30000,
                          deadline);
          if (status != SP_OK)
            return status;
          p->size += bytes;
        }
      if (!error)
        continue;
      p->height = sanei_sp1130n_le32 (h + 16);
      p->width = sanei_sp1130n_le32 (h + 20);
      if (h[48] < 1 || h[48] > 3)
        return reject_payload (&packet, 0, deadline, SP_UNSUPPORTED,
                                final_side);
      if (!p->size || !p->width || !p->height || p->width > 10000 ||
          p->height > 20000
          || (uint64_t) p->width * p->height * 3 > SP_PAGE_LIMIT)
        return reject_payload (&packet, 0, deadline, SP_IO, final_side);
      p->complete = 1;
      if (++completed == (source == SP_SOURCE_DUPLEX ? 2u : 1u))
        {
          /* End-of-sheet events follow directly, without another read command. */
          int consumed;
          status = completion_records (&packet, deadline, &consumed);
          return consumed && cancelled (io) ? SP_CANCELLED : status;
        }
    }
  return SP_IO;
}

int
sanei_sp1130n_scan_sheet (struct sanei_sp1130n_io *io,
                          const struct sanei_sp1130n_settings *settings,
                          const uint8_t token[40],
                          struct sanei_sp1130n_sheet *sheet)
{
  if (!io || !settings || !token || !sheet)
    return SP_INVALID;
  memset (sheet, 0, sizeof *sheet);
  if (io->network)
    {
      struct sanei_sp1130n_session session = { 0 };
      int status =
        sanei_sp1130n_scan_next (io, settings, token, &session, sheet);
      int cleanup = sanei_sp1130n_scan_finish (io, token, &session);
      if (status == SP_OK)
        status = cleanup;
      if (status != SP_OK)
        sanei_sp1130n_sheet_free (sheet);
      return status;
    }
  uint8_t params[SP_PARAM_SIZE], reply[108] = { 0 }, echo[40];
  int status = sanei_sp1130n_parameters (params, settings);
  if (status != SP_OK)
    return status;
  struct compressed_page pages[2] = { {0}, {0} };
  int own_lock = 0, started = 0;
  status = sanei_sp1130n_probe (io, reply);
  if (status != SP_OK)
    goto done;
  status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
  if (status != SP_OK)
    goto done;
  if (sanei_sp1130n_le32 (reply + 32))
    {
      status = SP_BUSY;
      goto done;
    }
  if (cancelled (io))
    {
      status = SP_CANCELLED;
      goto done;
    }
  struct sanei_sp1130n_io handshake = *io;
  handshake.cancelled = NULL;
  status = token_write (&handshake, 1, token);
  if (status != SP_OK)
    goto done;
  own_lock = 1;
  status = acknowledged (&handshake, 0x28, 0x78, 1, 40);
  if (status != SP_OK)
    goto done;
  status = sanei_sp1130n_exact (&handshake, 1, echo, sizeof echo, 5000);
  if (status != SP_OK)
    goto done;
  if (memcmp (token, echo, sizeof echo))
    {
      status = SP_BUSY;
      own_lock = 0;
      goto done;
    }
  if (cancelled (io))
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = sanei_sp1130n_command (io, 0x12, 2, 32, 0, reply, 32);
  if (status != SP_OK)
    goto done;
  status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
  if (status != SP_OK)
    goto done;
  if (sanei_sp1130n_le16 (reply + 8) & 1)
    {
      status = SP_COVER_OPEN;
      goto done;
    }
  if (!(sanei_sp1130n_le16 (reply + 8) & 2))
    {
      status = SP_NO_DOCS;
      goto done;
    }
  status = acknowledged (&handshake, 0x2a, 9, 4, SP_PARAM_SIZE);
  if (status != SP_OK)
    goto done;
  status = sanei_sp1130n_exact (&handshake, 0, params, sizeof params, 5000);
  if (status != SP_OK)
    goto done;
  if (cancelled (io))
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = acknowledged (&handshake, 0x14, 5, 0, 0);
  if (status != SP_OK)
    goto done;
  started = 1;
  if (cancelled (io))
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = acknowledged (io, 0x14, 4, 0, 0);
  if (status != SP_OK)
    goto done;
  status = image_blocks (io, settings->source, pages);
done:
  /* Cleanup ignores the cancellation flag but remains bounded and never unlocks a busy device. */
  if (own_lock)
    {
      struct sanei_sp1130n_io cleanup = *io;
      cleanup.cancelled = NULL;
      if (started)
        {
          int stop_status = acknowledged (&cleanup, 0x14, 6, 0, 0);
          if (status == SP_OK)
            status = stop_status;
        }
      int unlock_status = token_write (&cleanup, 2, token);
      if (status == SP_OK)
        status = unlock_status;
    }
  if (status == SP_OK && cancelled (io))
    status = SP_CANCELLED;
  if (status == SP_OK)
    {
      sheet->count = settings->source == SP_SOURCE_DUPLEX ? 2 : 1;
      for (unsigned side = 0; side < sheet->count; ++side)
        {
          unsigned wire_side = settings->source == SP_SOURCE_BACK ? 1 : side;
          status =
            sanei_sp1130n_decode_jpeg (pages[wire_side].data,
                                       pages[wire_side].size,
                                       pages[wire_side].width,
                                       pages[wire_side].height,
                                       &sheet->page[side]);
          if (status == SP_OK && cancelled (io))
            status = SP_CANCELLED;
          if (status != SP_OK)
            break;
        }
    }
  for (unsigned i = 0; i < 2; ++i)
    free (pages[i].data);
  if (status != SP_OK)
    sanei_sp1130n_sheet_free (sheet);
  return status;
}
