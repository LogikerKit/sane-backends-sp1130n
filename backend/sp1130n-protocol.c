/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
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

/* Byte fields give a packed wire layout without unaligned native integers.
 * Multi-byte fields are little-endian. Unknown profile fields retain their
 * byte offsets in their names rather than claiming unverified semantics. */
struct scan_parameters_wire
{
  uint8_t source[4];
  uint8_t front_mode, back_mode, front_depth, back_depth;
  uint8_t image_width[4], bytes_per_line[4];
  uint8_t x_resolution[2], y_resolution[2];
  uint8_t left[4], top[4], width[4], height[4];
  uint8_t page_width[4], page_height[4];
  uint8_t profile_44[2], profile_46[2], reserved_48[4];
  uint8_t profile_52[2], profile_54[2], profile_56[4];
  uint8_t profile_60[4], profile_64[8];
  uint8_t image_height[4], profile_76[4], reserved_80[20];
  uint8_t profile_100[4], reserved_104[28];
  uint8_t profile_132[4], reserved_136[124];
};

_Static_assert (sizeof (struct scan_parameters_wire) == SP_PARAM_SIZE,
                "SP-1130N parameters must occupy exactly 260 bytes");
_Static_assert (offsetof (struct scan_parameters_wire, left) == 20 &&
                offsetof (struct scan_parameters_wire, profile_60) == 60 &&
                offsetof (struct scan_parameters_wire, image_height) == 72 &&
                offsetof (struct scan_parameters_wire, profile_132) == 132,
                "SP-1130N parameter field offsets must match the wire format");

static void
parameters_wire (struct scan_parameters_wire *p,
                 const struct sanei_sp1130n_settings *s,
                 const struct sanei_sp1130n_geometry *g)
{
  memset (p, 0, sizeof *p);
  sanei_sp1130n_put32 (p->source,
                      s->source == SP_SOURCE_DUPLEX ? 4 :
                      s->source == SP_SOURCE_BACK ? 5 : 6);
  p->front_mode = p->back_mode = 3;
  p->front_depth = p->back_depth = 24;
  sanei_sp1130n_put32 (p->image_width, g->width);
  sanei_sp1130n_put32 (p->bytes_per_line, g->width * 3);
  sanei_sp1130n_put16 (p->x_resolution, (uint16_t) s->dpi);
  sanei_sp1130n_put16 (p->y_resolution, (uint16_t) s->dpi);
  sanei_sp1130n_put32 (p->left, g->left);
  sanei_sp1130n_put32 (p->top, g->top);
  sanei_sp1130n_put32 (p->width, g->width);
  sanei_sp1130n_put32 (p->height, g->height);
  sanei_sp1130n_put32 (p->page_width, g->page_width);
  sanei_sp1130n_put32 (p->page_height, g->page_height);
  sanei_sp1130n_put16 (p->profile_44, 100);
  sanei_sp1130n_put16 (p->profile_46, 100);
  sanei_sp1130n_put16 (p->profile_52, 4);
  sanei_sp1130n_put16 (p->profile_54, 31);
  sanei_sp1130n_put32 (p->profile_56, 1);
  p->profile_60[3] = 128;
  sanei_sp1130n_put32 (p->image_height, g->height);
  sanei_sp1130n_put32 (p->profile_76, 32);
  sanei_sp1130n_put32 (p->profile_100, 1);
  sanei_sp1130n_put32 (p->profile_132, 100);
}

SANE_Status
sanei_sp1130n_parameters (uint8_t output[SP_PARAM_SIZE],
                         const struct sanei_sp1130n_settings *s)
{
  struct sanei_sp1130n_geometry g;
  if (!output || sanei_sp1130n_scan_geometry (s, &g) != SANE_STATUS_GOOD)
    return SANE_STATUS_INVAL;
  struct scan_parameters_wire p;
  parameters_wire (&p, s, &g);
  memcpy (output, &p, sizeof p);
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_sp1130n_network_parameters (uint8_t output[SP_PARAM_SIZE],
                                  const struct sanei_sp1130n_settings *s)
{
  struct sanei_sp1130n_geometry g;
  if (!output || sanei_sp1130n_scan_geometry (s, &g) != SANE_STATUS_GOOD)
    return SANE_STATUS_INVAL;
  /* Keep the full-A4 feed profile; broader dpi values are diagnostic trials. */
  if (s->width_mm != 210 || s->height_mm != 297 ||
      s->left_mm || s->top_mm ||
      (s->scan_width_mm && s->scan_width_mm != 210) ||
      (s->scan_height_mm && s->scan_height_mm != 297))
    return SANE_STATUS_UNSUPPORTED;
  struct scan_parameters_wire p;
  parameters_wire (&p, s, &g);
  /* Preserve LAN's outward-rounded A4 height (3508 at 300 dpi). */
  unsigned height = (s->height_mm * s->dpi * 10 + 253) / 254;
  /* Acquire both sides; select the output on the host. */
  sanei_sp1130n_put32 (p.source, 4);
  sanei_sp1130n_put32 (p.left, 0);
  sanei_sp1130n_put32 (p.height, height);
  sanei_sp1130n_put32 (p.page_height, height);
  sanei_sp1130n_put32 (p.image_height, height);
  p.profile_60[0] = 0x75;
  p.profile_64[3] = 4;
  sanei_sp1130n_put32 (p.profile_100, 0);
  memcpy (output, &p, sizeof p);
  return SANE_STATUS_GOOD;
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

/* Back off after consecutive empty image replies; never sleep past cancel. */
static void
empty_reply_pause (const struct sanei_sp1130n_io *io, unsigned *empty)
{
  if (*empty < 3)
    {
      ++*empty;
      return;
    }
  for (unsigned slice = 0; slice < 10 && !cancelled (io); ++slice)
    {
      struct timespec t = { 0, 10 * 1000 * 1000 };
      nanosleep (&t, NULL);
    }
}

static SANE_Status
exact_progress (struct sanei_sp1130n_io *io, int input, uint8_t *data,
                size_t size, unsigned timeout, size_t *done)
{
  *done = 0;
  if (!io || !io->bulk || (!data && size) || !timeout)
    return SANE_STATUS_INVAL;
  uint64_t deadline = now_ms () + timeout;
  unsigned calls = 0;
  while (*done < size)
    {
      if (cancelled (io))
        return SANE_STATUS_CANCELLED;
      uint64_t now = now_ms ();
      if (now >= deadline || ++calls > 65536)
        return SANE_STATUS_IO_ERROR;
      size_t chunk = size - *done;
      if (chunk > 1024 * 1024)
        chunk = 1024 * 1024;
      size_t actual = 0;
      SANE_Status status = io->bulk (io->context, input, data + *done, chunk, &actual,
                             (unsigned) (deadline - now));
      if (actual > chunk)
        return SANE_STATUS_IO_ERROR;
      /* libusb can report transferred bytes even when the call times out. */
      *done += actual;
      if (cancelled (io))
        return SANE_STATUS_CANCELLED;
      if (status != SANE_STATUS_GOOD)
        return status == SANE_STATUS_EOF && *done ? SANE_STATUS_IO_ERROR : status;
      if (!actual)
        return SANE_STATUS_IO_ERROR;
    }
  return cancelled (io) ? SANE_STATUS_CANCELLED : SANE_STATUS_GOOD;
}

SANE_Status
sanei_sp1130n_exact (struct sanei_sp1130n_io *io, int input, uint8_t *data,
                     size_t size, unsigned timeout)
{
  size_t done;
  return exact_progress (io, input, data, size, timeout, &done);
}

SANE_Status
sanei_sp1130n_command (struct sanei_sp1130n_io *io, uint8_t op, uint8_t sub,
                       uint16_t sel, uint32_t value, uint8_t *reply,
                       size_t reply_size)
{
  if (!io || !io->bulk || (reply_size && !reply))
    return SANE_STATUS_INVAL;
  if (cancelled (io))
    return SANE_STATUS_CANCELLED;
  /* USB replies share an input endpoint with later cleanup acknowledgements. */
  struct sanei_sp1130n_io command = *io;
  if (!io->network)
    command.cancelled = NULL;
  if (command.begin_command)
    {
      SANE_Status status = command.begin_command (&command, op, sub);
      if (status != SANE_STATUS_GOOD)
        return status;
    }
  uint8_t h[16];
  sanei_sp1130n_header (h, op, sub, sel, value);
  SANE_Status status = sanei_sp1130n_exact (&command, 0, h, sizeof h, 5000);
  if (status != SANE_STATUS_GOOD)
    return status;
  if (!reply_size)
    return cancelled (io) ? SANE_STATUS_CANCELLED : SANE_STATUS_GOOD;
  size_t done;
  status = exact_progress (&command, 1, reply, reply_size, 5000, &done);
  if (status == SANE_STATUS_IO_ERROR && !io->network && op == 0x12 && sub == 0x7a &&
      sel == 108 && !value && reply_size == 108 && command.recover_inquiry)
    {
      /* Consume this reply only; keep the failed job latched and never refeed. */
      SANE_Status recovery = command.recover_inquiry (command.context, reply + done,
                                              reply_size - done, 1000);
      if (io->log_header)
        io->log_header ("USB inquiry timeout consumption", recovery, NULL, 0);
    }
  if (status == SANE_STATUS_GOOD && cancelled (io))
    return SANE_STATUS_CANCELLED;
  return status == SANE_STATUS_EOF ? SANE_STATUS_IO_ERROR : status;
}

SANE_Status
sanei_sp1130n_device_error (uint32_t n)
{
  if (!n)
    return SANE_STATUS_GOOD;
  if (n == 0xb)
    return SANE_STATUS_DEVICE_BUSY;
  if (n == 0xf)
    return SANE_STATUS_UNSUPPORTED;
  if (n == 0x20)
    return SANE_STATUS_INVAL;
  if (n >= 0x2002 && n <= 0x2005)
    return SANE_STATUS_JAMMED;
  if (n == 0x2009)
    return SANE_STATUS_COVER_OPEN;
  if (n == 0x200e)
    return SANE_STATUS_NO_DOCS;
  if (n == 0x4001 || n == 0x4002)
    return SANE_STATUS_CANCELLED;
  return SANE_STATUS_IO_ERROR;
}

const char *
sanei_sp1130n_strstatus (SANE_Status n)
{
  static const char *const names[] = {
    "Success", "Unsupported operation", "Cancelled", "Device busy",
    "Invalid argument",
    "End of image", "Paper jam", "No documents", "Cover open",
    "I/O or protocol error",
    "Out of memory", "Access denied"
  };
  return (unsigned) n < sizeof names / sizeof names[0] ?
    names[n] : "Unknown error";
}

SANE_Status
sanei_sp1130n_probe (struct sanei_sp1130n_io *io, uint8_t reply[108])
{
  return sanei_sp1130n_command (io, 0x12, 0x7a, 108, 0, reply, 108);
}

static SANE_Status
acknowledged (struct sanei_sp1130n_io *io, uint8_t op, uint8_t sub,
              uint16_t sel, uint32_t value)
{
  uint8_t reply[32] = { 0 };
  SANE_Status status =
    sanei_sp1130n_command (io, op, sub, sel, value, reply, sizeof reply);
  return status ==
    SANE_STATUS_GOOD ? sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply)) : status;
}

static SANE_Status
network_paper_status (struct sanei_sp1130n_io *io)
{
  uint8_t reply[32] = { 0 };
  SANE_Status status = sanei_sp1130n_command (io, 0x12, 2, 32, 0, reply, sizeof reply);
  if (io->log_header)
    io->log_header ("LAN paper status", status, reply,
                    status == SANE_STATUS_GOOD ? sizeof reply : 0);
  if (status != SANE_STATUS_GOOD)
    return status;
  status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
  if (status != SANE_STATUS_GOOD)
    return status;
  /* Idle 12/02 tags distinguish closed/empty from open cover, irrespective of +4. */
  uint32_t tag = sanei_sp1130n_le32 (reply + 8);
  if (tag == 0xc5000001)
    return sanei_sp1130n_le32 (reply + 16) ||
      sanei_sp1130n_le32 (reply + 20) ? SANE_STATUS_IO_ERROR : SANE_STATUS_COVER_OPEN;
  if (tag == 0xc5000000 && !sanei_sp1130n_le32 (reply + 16) &&
      !sanei_sp1130n_le32 (reply + 20))
    return SANE_STATUS_NO_DOCS;
  return SANE_STATUS_GOOD;
}

static SANE_Status
token_write (struct sanei_sp1130n_io *io, uint16_t selector,
             const uint8_t token[40])
{
  SANE_Status status = acknowledged (io, 0x2a, 0x78, selector, 40);
  if (status != SANE_STATUS_GOOD)
    return status;
  return sanei_sp1130n_exact (io, 0, (uint8_t *) token, 40, 5000);
}

SANE_Status
sanei_sp1130n_read_nvram (struct sanei_sp1130n_io *io,
                          const uint8_t token[40],
                          uint8_t output[SP_NVRAM_SIZE])
{
  if (!io || !io->bulk || !token || !output)
    return SANE_STATUS_INVAL;
  uint8_t reply[108] = { 0 }, echo[40], snapshot[SP_NVRAM_SIZE];
  SANE_Status status = sanei_sp1130n_probe (io, reply);
  if (status != SANE_STATUS_GOOD)
    return status;
  status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
  if (status != SANE_STATUS_GOOD)
    return status;
  if (sanei_sp1130n_le32 (reply + 32))
    return SANE_STATUS_DEVICE_BUSY;
  if (cancelled (io))
    return SANE_STATUS_CANCELLED;
  /* Finish the bounded UUID handshake before honoring cancellation. */
  struct sanei_sp1130n_io handshake = *io;
  handshake.cancelled = NULL;
  status = token_write (&handshake, 1, token);
  if (status != SANE_STATUS_GOOD)
    return status;
  int own_lock = 1;
  status = acknowledged (&handshake, 0x28, 0x78, 1, 40);
  if (status == SANE_STATUS_GOOD)
    status = sanei_sp1130n_exact (&handshake, 1, echo, sizeof echo, 5000);
  if (status == SANE_STATUS_GOOD && memcmp (token, echo, sizeof echo))
    {
      status = SANE_STATUS_DEVICE_BUSY;
      own_lock = 0;
    }
  if (status == SANE_STATUS_GOOD && cancelled (io))
    status = SANE_STATUS_CANCELLED;
  struct sanei_sp1130n_io snapshot_io = *io;
  if (!io->network)
    snapshot_io.cancelled = NULL;
  if (status == SANE_STATUS_GOOD)
    status = acknowledged (&snapshot_io, 0x28, 0x10, 0, SP_NVRAM_SIZE);
  if (status == SANE_STATUS_GOOD)
    status = sanei_sp1130n_exact (&snapshot_io, 1, snapshot, sizeof snapshot,
                                 5000);
  if (own_lock)
    {
      SANE_Status unlock_status = token_write (&handshake, 2, token);
      if (status == SANE_STATUS_GOOD)
        status = unlock_status;
    }
  if (status == SANE_STATUS_GOOD && cancelled (io))
    status = SANE_STATUS_CANCELLED;
  if (status == SANE_STATUS_GOOD)
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

static SANE_Status
network_blocks (struct sanei_sp1130n_io *io, struct compressed_page pages[3],
                int *no_paper, unsigned prior_sheets,
                const uint8_t initial[32])
{
  unsigned completed = 0;
  int current_side = -1;
  int empty_boundary = 0;
  unsigned empty = 0;
  uint64_t deadline = now_ms () + 120000;
  for (unsigned block = 0; block < 65536; ++block)
    {
      if (now_ms () >= deadline)
        return SANE_STATUS_IO_ERROR;
      uint8_t h[64] = { 0 };
      SANE_Status status = SANE_STATUS_GOOD;
      if (!block && initial)
        memcpy (h, initial, 32);
      else
        {
          status = sanei_sp1130n_command (io, 0x28, 4, 0, 0, NULL, 0);
          if (io->log_header)
            io->log_header ("LAN image request", status, NULL, 0);
          if (status == SANE_STATUS_GOOD)
            {
              /* High-resolution startup can exceed a normal block's wait. */
              uint64_t now = now_ms ();
              if (now >= deadline)
                return SANE_STATUS_IO_ERROR;
              unsigned timeout = block ? 5000 : 30000;
              if (deadline - now < timeout)
                timeout = (unsigned) (deadline - now);
              status = sanei_sp1130n_exact (io, 1, h, 32, timeout);
            }
        }
      if (io->log_header)
        io->log_header ("LAN image prefix", status, h,
                        status == SANE_STATUS_GOOD ? 32 : 0);
      if (status != SANE_STATUS_GOOD)
        {
          if ((empty_boundary || (!block && prior_sheets)) &&
              io->closed_without_reply &&
              io->closed_without_reply (io) &&
              (status == SANE_STATUS_IO_ERROR || status == SANE_STATUS_EOF))
            {
              /* A complete-sheet idle close still needs verified empty status. */
              SANE_Status paper = network_paper_status (io);
              return paper == SANE_STATUS_GOOD ? SANE_STATUS_IO_ERROR : paper;
            }
          /* A clean sheet-boundary close still needs positive empty status. */
          if (status == SANE_STATUS_EOF && !block && prior_sheets)
            {
              SANE_Status paper = network_paper_status (io);
              return paper == SANE_STATUS_GOOD ? SANE_STATUS_IO_ERROR : paper;
            }
          return status == SANE_STATUS_EOF ? SANE_STATUS_IO_ERROR : status;
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
              return status == SANE_STATUS_NO_DOCS && block ? SANE_STATUS_IO_ERROR : status;
            }
          status = sanei_sp1130n_exact (io, 1, h + 32, 32, 5000);
          if (io->log_header)
            io->log_header ("LAN image header", status, h,
                            status == SANE_STATUS_GOOD ? 64 : 32);
          if (status != SANE_STATUS_GOOD)
            return status == SANE_STATUS_EOF ? SANE_STATUS_IO_ERROR : status;
          bytes = sanei_sp1130n_le32 (h + 36);
          if (!bytes)
            bytes = sanei_sp1130n_le32 (h + 32);
        }
      if (!block && prior_sheets && !error && !bytes && h[11] <= 1 &&
          !sanei_sp1130n_le32 (h + 16) &&
          !sanei_sp1130n_le32 (h + 20))
        {
          empty_boundary = 1;
          continue;
        }
      empty_boundary = 0;
      if (!bytes && error == 0x1006 && !block)
        return sanei_sp1130n_le32 (h + 16) ||
          sanei_sp1130n_le32 (h + 20) ? SANE_STATUS_IO_ERROR : SANE_STATUS_NO_DOCS;
      unsigned side;
      if (short_header)
        side = current_side < 0 ? 2u : (unsigned) current_side;
      else
        {
          if (h[11] > 1)
            return SANE_STATUS_IO_ERROR;
          /* Face-down LAN labels confirm byte1 FRONT, byte0 BACK, as on USB. */
          side = h[11] == 1 ? 0 : 1;
          if (pages[2].size)
            {
              if (pages[side].size || pages[side].complete)
                return SANE_STATUS_IO_ERROR;
              pages[side] = pages[2];
              pages[2] = (struct compressed_page) { 0 };
            }
          current_side = (int) side;
        }
      struct compressed_page *p = &pages[side];
      if (p->complete)
        return SANE_STATUS_IO_ERROR;
      unsigned width = short_header ? 0 : sanei_sp1130n_le32 (h + 20);
      unsigned height = short_header ? 0 : sanei_sp1130n_le32 (h + 16);
      if (width || height)
        {
          if (width > 10000 || height > 20000 ||
              (width && p->width && width != p->width) ||
              (height && p->height && height != p->height))
            return SANE_STATUS_IO_ERROR;
          if (width)
            p->width = width;
          if (height)
            p->height = height;
          if ((uint64_t) p->width * p->height * 3 > SP_PAGE_LIMIT)
            return SANE_STATUS_IO_ERROR;
        }
      if (!short_header && h[48] > 3)
        return SANE_STATUS_UNSUPPORTED;
      if (bytes > SP_COMPRESSED_LIMIT - p->size)
        return SANE_STATUS_NO_MEM;
      if (bytes)
        {
          void *larger = realloc (p->data, p->size + bytes);
          if (!larger)
            return SANE_STATUS_NO_MEM;
          p->data = larger;
          size_t done;
          status = exact_progress (io, 1, p->data + p->size, bytes, 30000,
                                   &done);
          if (status == SANE_STATUS_CANCELLED && io->cancel_image_tail)
            {
              /* Aborting a partially unread TCP payload can reset cleanup. */
              SANE_Status consumed = io->cancel_image_tail (io->context, bytes - done,
                                                    1000);
              if (io->log_header)
                io->log_header ("LAN cancelled payload consumption", consumed,
                                NULL, 0);
            }
          if (status != SANE_STATUS_GOOD)
            return status == SANE_STATUS_EOF ? SANE_STATUS_IO_ERROR : status;
          p->size += bytes;
        }
      if (bytes)
        empty = 0;
      else if (!error)
        empty_reply_pause (io, &empty);
      if (!error)
        continue;
      if (!p->size)
        return SANE_STATUS_IO_ERROR;
      if (!p->width || !p->height)
        {
          unsigned jpeg_width, jpeg_height;
          status = sanei_sp1130n_jpeg_geometry (p->data, p->size,
                                                &jpeg_width, &jpeg_height);
          if (status != SANE_STATUS_GOOD)
            return status;
          if ((p->width && p->width != jpeg_width) ||
              (p->height && p->height != jpeg_height))
            return SANE_STATUS_IO_ERROR;
          p->width = jpeg_width;
          p->height = jpeg_height;
        }
      p->complete = 1;
      current_side = -1;
      if (error == 0x1006)
        *no_paper = 1;
      if (++completed == 2)
        return SANE_STATUS_GOOD;           /* LAN has no unsolicited USB end-of-sheet records. */
    }
  return SANE_STATUS_IO_ERROR;
}

SANE_Status
sanei_sp1130n_scan_finish (struct sanei_sp1130n_io *io,
                           const uint8_t token[40],
                           struct sanei_sp1130n_session *session)
{
  if (!io || !token || !session)
    return SANE_STATUS_INVAL;
  struct sanei_sp1130n_io cleanup = *io;
  cleanup.cancelled = NULL;
  SANE_Status status = SANE_STATUS_GOOD;
  if ((session->started || session->stop_pending)
      && (!session->exhausted || !io->network))
    {
      status = acknowledged (&cleanup, 0x14, 6, 0, 0);
      if (io->log_header)
        io->log_header ("Acquisition stop", status, NULL, 0);
    }
  if (session->own_lock)
    {
      SANE_Status unlock = token_write (&cleanup, 2, token);
      if (io->log_header)
        io->log_header ("Acquisition unlock", unlock, NULL, 0);
      if (status == SANE_STATUS_GOOD)
        status = unlock;
    }
  session->own_lock = session->started = session->stop_pending = 0;
  return status;
}

static SANE_Status usb_scan_next (struct sanei_sp1130n_io *,
                          const struct sanei_sp1130n_settings *,
                          const uint8_t[40], struct sanei_sp1130n_session *,
                          struct sanei_sp1130n_sheet *);

static SANE_Status
scan_next (struct sanei_sp1130n_io *io,
           const struct sanei_sp1130n_settings *settings,
           const uint8_t token[40],
           struct sanei_sp1130n_session *session,
           struct sanei_sp1130n_sheet *sheet, int encoded)
{
  if (!sheet)
    return SANE_STATUS_INVAL;
  memset (sheet, 0, sizeof *sheet);
  if (!io || !io->bulk || !settings || !token || !session)
    return SANE_STATUS_INVAL;
  if (!io->network)
    return usb_scan_next (io, settings, token, session, sheet);
  if (session->error)
    return session->error;
  if (session->exhausted)
    return SANE_STATUS_NO_DOCS;
  uint8_t params[SP_PARAM_SIZE], reply[108] = { 0 }, echo[40];
  SANE_Status status = sanei_sp1130n_network_parameters (params, settings);
  if (status != SANE_STATUS_GOOD)
    return status;
  struct compressed_page pages[3] = { {0}, {0}, {0} };
  uint8_t initial[32];
  const uint8_t *first = NULL;
  if (!session->started)
    {
      status = sanei_sp1130n_probe (io, reply);
      if (status == SANE_STATUS_GOOD)
        status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
      if (io->log_header)
        io->log_header ("LAN acquisition inquiry", status, NULL, 0);
      if (status != SANE_STATUS_GOOD)
        goto done;
      if (sanei_sp1130n_le32 (reply + 32))
        {
          status = SANE_STATUS_DEVICE_BUSY;
          if (io->log_header)
            io->log_header ("LAN inquiry ownership", status, NULL, 0);
          goto done;
        }
      if (cancelled (io))
        {
          status = SANE_STATUS_CANCELLED;
          goto done;
        }
      struct sanei_sp1130n_io handshake = *io;
      handshake.cancelled = NULL;
      status = token_write (&handshake, 1, token);
      if (io->log_header)
        io->log_header ("LAN lock request", status, NULL, 0);
      if (status != SANE_STATUS_GOOD)
        goto done;
      session->own_lock = 1;
      status = acknowledged (&handshake, 0x28, 0x78, 1, 40);
      if (status == SANE_STATUS_GOOD)
        status = sanei_sp1130n_exact (&handshake, 1, echo, sizeof echo, 5000);
      if (status == SANE_STATUS_GOOD && memcmp (token, echo, sizeof echo))
        {
          status = SANE_STATUS_DEVICE_BUSY;
          session->own_lock = 0;
        }
      if (io->log_header)
        io->log_header ("LAN lock verification", status, NULL, 0);
      if (status == SANE_STATUS_GOOD && cancelled (io))
        status = SANE_STATUS_CANCELLED;
      if (status != SANE_STATUS_GOOD)
        goto done;
      status = network_paper_status (io);
      if (status == SANE_STATUS_NO_DOCS)
        {
          session->exhausted = 1;
          goto done;
        }
      if (status == SANE_STATUS_GOOD)
        status = acknowledged (io, 0x2a, 9, 4, SP_PARAM_SIZE);
      if (status == SANE_STATUS_GOOD)
        status = sanei_sp1130n_exact (io, 0, params, sizeof params, 5000);
      if (status == SANE_STATUS_GOOD)
        status = acknowledged (io, 0x14, 5, 0, 0);
      if (io->log_header)
        io->log_header ("LAN prepare", status, NULL, 0);
      if (status != SANE_STATUS_GOOD)
        goto done;
      session->started = 1;
      status = sanei_sp1130n_command (io, 0x14, 4, 0, 0, reply, 32);
      if (io->log_header)
        io->log_header ("LAN start", status, reply,
                        status == SANE_STATUS_GOOD ? 32 : 0);
      if (status == SANE_STATUS_GOOD)
        {
          uint32_t error = sanei_sp1130n_le32 (reply);
          unsigned width = sanei_sp1130n_le32 (reply + 20);
          if ((error == 0 || error == 0x1005 || error == 0x1006) &&
              !sanei_sp1130n_le32 (reply + 4) && reply[11] <= 1 &&
              width && width <= 10000)
            {
              /* START can already carry an image prefix on this connection. */
              memcpy (initial, reply, sizeof initial);
              first = initial;
              status = io->retain_image ? io->retain_image (io) : SANE_STATUS_IO_ERROR;
            }
          else
            status = sanei_sp1130n_device_error (error);
        }
      if (status != SANE_STATUS_GOOD)
        goto done;
    }
  int no_paper = 0;
  status = network_blocks (io, pages, &no_paper, session->sheets, first);
  if (status == SANE_STATUS_GOOD)
    {
      /* Queued JPEGs are decoded by the consumer, including discarded sides. */
      sheet->count = 2;
      for (unsigned side = 0; side < 2 && status == SANE_STATUS_GOOD; ++side)
        {
          if (encoded)
            {
              sheet->page[side] = (struct sanei_sp1130n_page)
                { pages[side].data, pages[side].size,
                  pages[side].width, pages[side].height };
              pages[side].data = NULL;
              sheet->encoded = 1;
            }
          else
            status =
              sanei_sp1130n_decode_jpeg (pages[side].data, pages[side].size,
                                         pages[side].width, pages[side].height,
                                         &sheet->page[side]);
          if (status == SANE_STATUS_GOOD && cancelled (io))
            status = SANE_STATUS_CANCELLED;
        }
      if (status == SANE_STATUS_GOOD)
        {
          ++session->sheets;
          session->exhausted = no_paper;
          if (!encoded && settings->source != SP_SOURCE_DUPLEX)
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
  else if (status == SANE_STATUS_NO_DOCS)
    session->exhausted = 1;
done:
  if (status != SANE_STATUS_GOOD || session->exhausted)
    {
      SANE_Status cleanup = sanei_sp1130n_scan_finish (io, token, session);
      if (status == SANE_STATUS_GOOD || (status == SANE_STATUS_NO_DOCS && cleanup != SANE_STATUS_GOOD))
        status = cleanup;
    }
  for (unsigned i = 0; i < 3; ++i)
    free (pages[i].data);
  if (status != SANE_STATUS_GOOD)
    {
      if (status != SANE_STATUS_NO_DOCS)
        session->error = status;
      sanei_sp1130n_sheet_free (sheet);
    }
  return status;
}

SANE_Status
sanei_sp1130n_scan_next (struct sanei_sp1130n_io *io,
                         const struct sanei_sp1130n_settings *settings,
                         const uint8_t token[40],
                         struct sanei_sp1130n_session *session,
                         struct sanei_sp1130n_sheet *sheet)
{
  return scan_next (io, settings, token, session, sheet, 0);
}

SANE_Status
sanei_sp1130n_scan_next_encoded (struct sanei_sp1130n_io *io,
                                 const struct sanei_sp1130n_settings *settings,
                                 const uint8_t token[40],
                                 struct sanei_sp1130n_session *session,
                                 struct sanei_sp1130n_sheet *sheet)
{
  return scan_next (io, settings, token, session, sheet, 1);
}

static SANE_Status
discard_error_payload (struct sanei_sp1130n_io *io, uint32_t bytes,
                       uint64_t deadline)
{
  if (bytes > SP_COMPRESSED_LIMIT)
    return SANE_STATUS_IO_ERROR;
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
        return SANE_STATUS_IO_ERROR;
      size_t size = bytes < sizeof buffer ? bytes : sizeof buffer;
      SANE_Status status = sanei_sp1130n_exact (&drain, 1, buffer, size,
                                       (unsigned) (deadline - now));
      if (status != SANE_STATUS_GOOD)
        return status == SANE_STATUS_EOF ? SANE_STATUS_IO_ERROR : status;
      bytes -= (uint32_t) size;
    }
  return SANE_STATUS_GOOD;
}

static SANE_Status
exact_before (struct sanei_sp1130n_io *io, int input, uint8_t *data,
              size_t size, unsigned timeout, uint64_t deadline)
{
  uint64_t now = now_ms ();
  if (now >= deadline)
    return SANE_STATUS_IO_ERROR;
  if (deadline - now < timeout)
    timeout = (unsigned) (deadline - now);
  return sanei_sp1130n_exact (io, input, data, size, timeout);
}

static SANE_Status
image_read_before (struct sanei_sp1130n_io *io, uint8_t *data, size_t size,
                   unsigned timeout, uint64_t deadline, size_t *done)
{
  *done = 0;
  uint64_t now = now_ms ();
  if (now >= deadline)
    return SANE_STATUS_IO_ERROR;
  if (deadline - now < timeout)
    timeout = (unsigned) (deadline - now);
  return exact_progress (io, 1, data, size, timeout, done);
}

static SANE_Status
recover_image_completion (struct sanei_sp1130n_io *io, uint8_t h[64],
                          size_t done)
{
  for (unsigned event = 0; event < 32; ++event)
    {
      if (done > 64 ||
          io->recover_image (io->context, h + done, 64 - done, 5000) != SANE_STATUS_GOOD)
        return SANE_STATUS_IO_ERROR;
      uint32_t error = sanei_sp1130n_le32 (h);
      uint32_t bytes = sanei_sp1130n_le32 (h + 36);
      if (bytes > SP_COMPRESSED_LIMIT)
        return SANE_STATUS_IO_ERROR;
      if (error && error != 0x1005 && error != 0x1006)
        return io->recover_image (io->context, NULL, bytes, 5000);
      if (bytes)
        return SANE_STATUS_IO_ERROR;
      if (h[6] == 2 || h[6] == 4)
        return SANE_STATUS_GOOD;
      done = 0;
    }
  return SANE_STATUS_IO_ERROR;
}

static SANE_Status
recover_image_transaction (struct sanei_sp1130n_io *io, uint8_t h[64],
                           size_t header_done, uint8_t *payload,
                           size_t payload_size, size_t payload_done,
                           enum sanei_sp1130n_source source,
                           const struct compressed_page pages[2],
                           int completion)
{
  if (io->network || !io->recover_image || !io->image_recovery_done)
    return SANE_STATUS_IO_ERROR;
  SANE_Status recovery = SANE_STATUS_IO_ERROR;
  if (completion)
    recovery = recover_image_completion (io, h, header_done);
  else if (pages && header_done <= 64 && payload_done <= payload_size &&
           io->recover_image (io->context, h + header_done,
                              64 - header_done, 5000) == SANE_STATUS_GOOD)
    {
      uint32_t error = sanei_sp1130n_le32 (h);
      uint32_t bytes = sanei_sp1130n_le32 (h + 36);
      int image = !error || error == 0x1005 || error == 0x1006;
      int side_valid = h[11] <= 1 &&
        (source != SP_SOURCE_FRONT || h[11] == 1) &&
        (source != SP_SOURCE_BACK || h[11] == 0);
      unsigned side = h[11] == 1 ? 0 : 1;
      side_valid = side_valid && !pages[side].complete &&
        pages[side].size <= SP_COMPRESSED_LIMIT &&
        bytes <= SP_COMPRESSED_LIMIT - pages[side].size;
      if (bytes <= SP_COMPRESSED_LIMIT && (!image || side_valid) &&
          (!payload || bytes == payload_size))
        {
          uint8_t *tail = payload ? payload + payload_done : NULL;
          size_t remaining = payload ? payload_size - payload_done : bytes;
          recovery = io->recover_image (io->context, tail, remaining, 5000);
          unsigned completed = pages[0].complete + pages[1].complete;
          if (recovery == SANE_STATUS_GOOD && image && error &&
              completed + 1 == (source == SP_SOURCE_DUPLEX ? 2u : 1u))
            {
              uint8_t event[64];
              recovery = recover_image_completion (io, event, 0);
            }
        }
    }
  SANE_Status commit = io->image_recovery_done (io->context, recovery == SANE_STATUS_GOOD);
  if (io->log_header)
    io->log_header ("USB image timeout recovery",
                    recovery == SANE_STATUS_GOOD ? commit : SANE_STATUS_IO_ERROR, NULL, 0);
  /* Recovery permits cleanup only; the failed scan never becomes a success. */
  return SANE_STATUS_IO_ERROR;
}

static SANE_Status
completion_records (struct sanei_sp1130n_io *io, uint64_t deadline,
                    int *consumed, int *event_kind)
{
  *consumed = 0;
  *event_kind = 0;
  uint8_t h[64];
  for (unsigned event = 0; event < 32; ++event)
    {
      size_t done;
      SANE_Status status = image_read_before (io, h, sizeof h, 5000, deadline, &done);
      if (io->log_header)
        io->log_header ("USB completion", status, h,
                        status == SANE_STATUS_GOOD ? sizeof h : 0);
      if (status != SANE_STATUS_GOOD)
        return status == SANE_STATUS_IO_ERROR ?
          recover_image_transaction (io, h, done, NULL, 0, 0,
                                     SP_SOURCE_DUPLEX, NULL, 1) : status;
      uint32_t error = sanei_sp1130n_le32 (h);
      if (error && error != 0x1005 && error != 0x1006)
        {
          status = discard_error_payload (io, sanei_sp1130n_le32 (h + 36),
                                           deadline);
          *consumed = status == SANE_STATUS_GOOD;
          return status == SANE_STATUS_GOOD ? sanei_sp1130n_device_error (error) : status;
        }
      if (h[6] == 2 || h[6] == 4)
        {
          *consumed = 1;
          *event_kind = h[6];
          return SANE_STATUS_GOOD;
        }
    }
  return SANE_STATUS_IO_ERROR;
}

static SANE_Status
reject_payload (struct sanei_sp1130n_io *io, uint32_t bytes,
                uint64_t deadline, SANE_Status reason, int final_side)
{
  /* Do not trust or drain an oversized declaration, even after a local fault. */
  if (bytes > SP_COMPRESSED_LIMIT)
    return reason;
  SANE_Status status = discard_error_payload (io, bytes, deadline);
  if (status == SANE_STATUS_GOOD && final_side)
    {
      int consumed, event_kind;
      status = completion_records (io, deadline, &consumed, &event_kind);
      if (consumed)
        status = SANE_STATUS_GOOD;
    }
  return status == SANE_STATUS_GOOD ? reason : status;
}

static SANE_Status
image_blocks (struct sanei_sp1130n_io *io, enum sanei_sp1130n_source source,
              struct compressed_page pages[2], int *event_kind)
{
  /* An accepted USB block request must consume its full header and payload
   * before STOP/unlock can reuse bulk-IN. Honor cancellation between blocks. */
  struct sanei_sp1130n_io packet = *io;
  packet.cancelled = NULL;
  unsigned completed = 0, empty = 0;
  uint64_t deadline = now_ms () + 120000;
  for (unsigned block = 0; block < 65536; ++block)
    {
      if (now_ms () >= deadline)
        return SANE_STATUS_IO_ERROR;
      if (cancelled (io))
        return SANE_STATUS_CANCELLED;
      uint8_t request[16], h[64] = { 0 };
      sanei_sp1130n_header (request, 0x28, 4, 0, 0);
      SANE_Status status = exact_before (&packet, 0, request, sizeof request, 5000,
                                 deadline);
      size_t header_done = 0;
      int accepted = status == SANE_STATUS_GOOD;
      if (status == SANE_STATUS_GOOD)
        {
          /* Initial image readiness can take nearly five seconds at 600 dpi. */
          unsigned timeout = block ? 5000 : 30000;
          status = image_read_before (&packet, h, sizeof h, timeout, deadline,
                                       &header_done);
        }
      if (status != SANE_STATUS_GOOD)
        return status == SANE_STATUS_IO_ERROR && accepted ?
          recover_image_transaction (&packet, h, header_done, NULL, 0, 0,
                                     source, pages, 0) : status;
      if (io->log_header)
        io->log_header ("USB image header", status, h, sizeof h);
      uint32_t error = sanei_sp1130n_le32 (h);
      uint32_t bytes = sanei_sp1130n_le32 (h + 36);
      if (error && error != 0x1005 && error != 0x1006)
        {
          /* Keep payload bytes from becoming STOP or unlock acknowledgements. */
          status = discard_error_payload (&packet,
                                           sanei_sp1130n_le32 (h + 36),
                                           deadline);
          return status == SANE_STATUS_GOOD ? (cancelled (io) ? SANE_STATUS_CANCELLED :
                                    sanei_sp1130n_device_error (error)) : status;
        }
      if (h[11] > 1 || (source == SP_SOURCE_FRONT && h[11] != 1) ||
          (source == SP_SOURCE_BACK && h[11] != 0))
        return reject_payload (&packet, bytes, deadline, SANE_STATUS_IO_ERROR, 0);
      unsigned side = h[11] == 1 ? 0 : 1;
      struct compressed_page *p = &pages[side];
      if (p->complete)
        return reject_payload (&packet, bytes, deadline, SANE_STATUS_IO_ERROR, 0);
      int final_side = error &&
        completed + 1 == (source == SP_SOURCE_DUPLEX ? 2u : 1u);
      if (bytes > SP_COMPRESSED_LIMIT - p->size)
        return reject_payload (&packet, bytes, deadline, SANE_STATUS_NO_MEM, final_side);
      if (bytes)
        {
          void *larger = realloc (p->data, p->size + bytes);
          if (!larger)
            return reject_payload (&packet, bytes, deadline, SANE_STATUS_NO_MEM,
                                    final_side);
          p->data = larger;
          size_t done;
          status = image_read_before (&packet, p->data + p->size, bytes,
                                       30000, deadline, &done);
          if (status != SANE_STATUS_GOOD)
            return status == SANE_STATUS_IO_ERROR ?
              recover_image_transaction (&packet, h, sizeof h,
                                         p->data + p->size, bytes, done,
                                         source, pages, 0) : status;
          p->size += bytes;
        }
      if (bytes)
        empty = 0;
      else if (!error)
        empty_reply_pause (io, &empty);
      if (!error)
        continue;
      p->height = sanei_sp1130n_le32 (h + 16);
      p->width = sanei_sp1130n_le32 (h + 20);
      if (h[48] < 1 || h[48] > 3)
        return reject_payload (&packet, 0, deadline, SANE_STATUS_UNSUPPORTED,
                                final_side);
      if (!p->size || !p->width || !p->height || p->width > 10000 ||
          p->height > 20000
          || (uint64_t) p->width * p->height * 3 > SP_PAGE_LIMIT)
        return reject_payload (&packet, 0, deadline, SANE_STATUS_IO_ERROR, final_side);
      p->complete = 1;
      if (++completed == (source == SP_SOURCE_DUPLEX ? 2u : 1u))
        {
          /* End-of-sheet events follow directly, without another read command. */
          int consumed;
          status = completion_records (&packet, deadline, &consumed, event_kind);
          return consumed && cancelled (io) ? SANE_STATUS_CANCELLED : status;
        }
    }
  return SANE_STATUS_IO_ERROR;
}

SANE_Status
sanei_sp1130n_scan_sheet (struct sanei_sp1130n_io *io,
                          const struct sanei_sp1130n_settings *settings,
                          const uint8_t token[40],
                          struct sanei_sp1130n_sheet *sheet)
{
  if (!io || !io->bulk || !settings || !token || !sheet)
    return SANE_STATUS_INVAL;
  struct sanei_sp1130n_session session = { 0 };
  SANE_Status status = sanei_sp1130n_scan_next (io, settings, token, &session, sheet);
  SANE_Status cleanup = sanei_sp1130n_scan_finish (io, token, &session);
  if (status == SANE_STATUS_GOOD)
    status = cleanup;
  if (status != SANE_STATUS_GOOD)
    sanei_sp1130n_sheet_free (sheet);
  return status;
}

static SANE_Status
usb_scan_next (struct sanei_sp1130n_io *io,
               const struct sanei_sp1130n_settings *settings,
               const uint8_t token[40],
               struct sanei_sp1130n_session *session,
               struct sanei_sp1130n_sheet *sheet)
{
  if (session->error)
    return session->error;
  if (session->exhausted)
    return SANE_STATUS_NO_DOCS;
  uint8_t params[SP_PARAM_SIZE], reply[108] = { 0 }, echo[40];
  SANE_Status status = sanei_sp1130n_parameters (params, settings);
  if (status != SANE_STATUS_GOOD)
    return status;
  struct compressed_page pages[2] = { {0}, {0} };
  int event_kind = 0;
  if (cancelled (io))
    {
      status = SANE_STATUS_CANCELLED;
      goto done;
    }
  if (session->started)
    goto acquire;
  if (session->own_lock)
    goto prepare;
  status = sanei_sp1130n_probe (io, reply);
  if (status != SANE_STATUS_GOOD)
    goto done;
  status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
  if (status != SANE_STATUS_GOOD)
    goto done;
  if (sanei_sp1130n_le32 (reply + 32))
    {
      status = SANE_STATUS_DEVICE_BUSY;
      goto done;
    }
  if (cancelled (io))
    {
      status = SANE_STATUS_CANCELLED;
      goto done;
    }
  struct sanei_sp1130n_io handshake = *io;
  handshake.cancelled = NULL;
  status = token_write (&handshake, 1, token);
  if (status != SANE_STATUS_GOOD)
    goto done;
  session->own_lock = 1;
  status = acknowledged (&handshake, 0x28, 0x78, 1, 40);
  if (status != SANE_STATUS_GOOD)
    goto done;
  status = sanei_sp1130n_exact (&handshake, 1, echo, sizeof echo, 5000);
  if (status != SANE_STATUS_GOOD)
    goto done;
  if (memcmp (token, echo, sizeof echo))
    {
      status = SANE_STATUS_DEVICE_BUSY;
      session->own_lock = 0;
      goto done;
    }
  if (cancelled (io))
    {
      status = SANE_STATUS_CANCELLED;
      goto done;
    }
  status = sanei_sp1130n_command (io, 0x12, 2, 32, 0, reply, 32);
  if (io->log_header)
    io->log_header ("USB paper status", status, reply,
                    status == SANE_STATUS_GOOD ? 32 : 0);
  if (status != SANE_STATUS_GOOD)
    goto done;
  status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
  if (status != SANE_STATUS_GOOD)
    goto done;
  if (sanei_sp1130n_le16 (reply + 8) & 1)
    {
      status = SANE_STATUS_COVER_OPEN;
      goto done;
    }
  if (!(sanei_sp1130n_le16 (reply + 8) & 2))
    {
      status = SANE_STATUS_NO_DOCS;
      goto done;
    }
prepare:
  handshake = *io;
  handshake.cancelled = NULL;
  status = acknowledged (&handshake, 0x2a, 9, 4, SP_PARAM_SIZE);
  if (status != SANE_STATUS_GOOD)
    goto done;
  status = sanei_sp1130n_exact (&handshake, 0, params, sizeof params, 5000);
  if (status != SANE_STATUS_GOOD)
    goto done;
  if (cancelled (io))
    {
      status = SANE_STATUS_CANCELLED;
      goto done;
    }
  status = acknowledged (&handshake, 0x14, 5, 0, 0);
  if (status != SANE_STATUS_GOOD)
    goto done;
  session->started = 1;
  session->stop_pending = 1;
  if (cancelled (io))
    {
      status = SANE_STATUS_CANCELLED;
      goto done;
    }
  status = acknowledged (io, 0x14, 4, 0, 0);
  if (status != SANE_STATUS_GOOD)
    goto done;
acquire:
  status = image_blocks (io, settings->source, pages, &event_kind);
  if (status == SANE_STATUS_GOOD)
    {
      ++session->sheets;
      /* Completion is not paper status: event 4 can occur with sheets left.
       * Event 2 keeps this acquisition running: the next sheet is read with
       * further block requests, without new parameters, prepare or START. */
    }
done:
  /* Cleanup ignores the cancellation flag but remains bounded and never unlocks a busy device. */
  if (status == SANE_STATUS_NO_DOCS)
    session->exhausted = 1;
  if (status != SANE_STATUS_GOOD || event_kind == 4)
    {
      SANE_Status cleanup = sanei_sp1130n_scan_finish (io, token, session);
      if (status == SANE_STATUS_GOOD || (status == SANE_STATUS_NO_DOCS && cleanup != SANE_STATUS_GOOD))
        status = cleanup;
    }
  if (status == SANE_STATUS_GOOD && cancelled (io))
    status = SANE_STATUS_CANCELLED;
  if (status == SANE_STATUS_GOOD)
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
          if (status == SANE_STATUS_GOOD && cancelled (io))
            status = SANE_STATUS_CANCELLED;
          if (status != SANE_STATUS_GOOD)
            break;
        }
    }
  for (unsigned i = 0; i < 2; ++i)
    free (pages[i].data);
  if (status != SANE_STATUS_GOOD)
    {
      sanei_sp1130n_scan_finish (io, token, session);
      sanei_sp1130n_sheet_free (sheet);
      if (status != SANE_STATUS_NO_DOCS)
        session->error = status;
    }
  return status;
}
