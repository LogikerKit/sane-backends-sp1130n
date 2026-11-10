/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#include "../include/sane/config.h"
#include "sp1130n.h"
#include <stdio.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

static unsigned
be16 (const uint8_t *p)
{
  return (unsigned) p[0] * 256 + p[1];
}

static void
put_be16 (uint8_t *p, unsigned n)
{
  p[0] = (uint8_t) (n >> 8);
  p[1] = (uint8_t) n;
}

static SANE_Status
repair_geometry (uint8_t *p, size_t size, unsigned width, unsigned height)
{
  if (size < 4 || p[0] != 0xff || p[1] != 0xd8)
    return SANE_STATUS_IO_ERROR;
  size_t position = 2, restart = 0;
  unsigned mcu_width = 0;
  int found = 0, changed = 0;
  while (position < size)
    {
      if (p[position++] != 0xff)
        return SANE_STATUS_IO_ERROR;
      while (position < size && p[position] == 0xff)
        ++position;
      if (position == size)
        return SANE_STATUS_IO_ERROR;
      unsigned marker = p[position++];
      if (marker == 0xda)
        {
          if (!found)
            return SANE_STATUS_IO_ERROR;
          if (changed && restart && mcu_width)
            put_be16 (p + restart, (width + mcu_width - 1) / mcu_width);
          return SANE_STATUS_GOOD;
        }
      if (marker == 0xd8 || marker == 0xd9 || marker == 0
          || (marker >= 0xd0 && marker <= 0xd7))
        return SANE_STATUS_IO_ERROR;
      if (size - position < 2)
        return SANE_STATUS_IO_ERROR;
      unsigned length = be16 (p + position);
      if (length < 2 || length > size - position)
        return SANE_STATUS_IO_ERROR;
      if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2)
        {
          if (found || length < 8 || p[position + 2] != 8)
            return SANE_STATUS_UNSUPPORTED;
          unsigned components = p[position + 7];
          if (!components || components > 3 || length != 8 + components * 3)
            return SANE_STATUS_IO_ERROR;
          changed = be16 (p + position + 3) != height ||
            be16 (p + position + 5) != width;
          put_be16 (p + position + 3, height);
          put_be16 (p + position + 5, width);
          for (unsigned i = 0; i < components; ++i)
            {
              unsigned sampling = p[position + 9 + 3 * i] >> 4;
              if (!sampling || sampling > 4)
                return SANE_STATUS_IO_ERROR;
              if (mcu_width < sampling * 8)
                mcu_width = sampling * 8;
            }
          found = 1;
        }
      if (marker == 0xdd)
        {
          if (length != 4)
            return SANE_STATUS_IO_ERROR;
          restart = position + 2;
        }
      position += length;
    }
  return SANE_STATUS_IO_ERROR;
}

SANE_Status
sanei_sp1130n_jpeg_geometry (const uint8_t *p, size_t size,
                             unsigned *width, unsigned *height)
{
  if (!p || !width || !height || size < 4 || p[0] != 0xff || p[1] != 0xd8)
    return SANE_STATUS_IO_ERROR;
  size_t position = 2;
  while (position < size)
    {
      if (p[position++] != 0xff)
        return SANE_STATUS_IO_ERROR;
      while (position < size && p[position] == 0xff)
        ++position;
      if (position == size)
        return SANE_STATUS_IO_ERROR;
      unsigned marker = p[position++];
      if (marker == 0xda || marker == 0xd8 || marker == 0xd9 || !marker ||
          (marker >= 0xd0 && marker <= 0xd7) || size - position < 2)
        return SANE_STATUS_IO_ERROR;
      unsigned length = be16 (p + position);
      if (length < 2 || length > size - position)
        return SANE_STATUS_IO_ERROR;
      if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2)
        {
          if (length < 8 || p[position + 2] != 8)
            return SANE_STATUS_UNSUPPORTED;
          unsigned w = be16 (p + position + 5);
          unsigned h = be16 (p + position + 3);
          if (!w || !h || w > 10000 || h > 20000 ||
              (uint64_t) w * h * 3 > SP_PAGE_LIMIT)
            return SANE_STATUS_IO_ERROR;
          *width = w;
          *height = h;
          return SANE_STATUS_GOOD;
        }
      position += length;
    }
  return SANE_STATUS_IO_ERROR;
}

struct decoder
{
  struct jpeg_decompress_struct jpeg;
  struct jpeg_error_mgr errors;
  jmp_buf jump;
  uint8_t *pixels;
  int created;
  int warning;
};
static void
jpeg_failure (j_common_ptr c)
{
  struct decoder *d = (struct decoder *) c->client_data;
  longjmp (d->jump, 1);
}

static void
jpeg_message (j_common_ptr c, int level)
{
  struct decoder *d = (struct decoder *) c->client_data;
  if (level < 0)
    d->warning = 1;
}

SANE_Status
sanei_sp1130n_decode_jpeg (uint8_t *bytes, size_t size, unsigned width,
                           unsigned height, struct sanei_sp1130n_page *page)
{
  if (!bytes || !page || !width || !height || width > 10000 || height > 20000 ||
      (uint64_t) width * height * 3 > SP_PAGE_LIMIT)
    return SANE_STATUS_INVAL;
  memset (page, 0, sizeof *page);
  SANE_Status status = repair_geometry (bytes, size, width, height);
  if (status != SANE_STATUS_GOOD)
    return status;
  /* Heap state stays well-defined after libjpeg's longjmp error exit. */
  struct decoder *d = calloc (1, sizeof *d);
  if (!d)
    return SANE_STATUS_NO_MEM;
  d->jpeg.err = jpeg_std_error (&d->errors);
  d->errors.error_exit = jpeg_failure;
  d->errors.emit_message = jpeg_message;
  d->jpeg.client_data = d;
  if (setjmp (d->jump))
    {
      status = SANE_STATUS_IO_ERROR;
      goto done;
    }
  jpeg_create_decompress (&d->jpeg);
  d->created = 1;
  jpeg_mem_src (&d->jpeg, bytes, (unsigned long) size);
  if (jpeg_read_header (&d->jpeg, TRUE) != JPEG_HEADER_OK)
    {
      status = SANE_STATUS_IO_ERROR;
      goto done;
    }
  d->jpeg.out_color_space = JCS_RGB;
  if (!jpeg_start_decompress (&d->jpeg) || d->jpeg.output_width != width ||
      d->jpeg.output_height != height || d->jpeg.output_components != 3)
    {
      status = SANE_STATUS_IO_ERROR;
      goto done;
    }
  size_t stride = (size_t) width * 3;
  d->pixels = malloc (stride * height);
  if (!d->pixels)
    {
      status = SANE_STATUS_NO_MEM;
      goto done;
    }
  while (d->jpeg.output_scanline < height)
    {
      JSAMPROW row = d->pixels + stride * d->jpeg.output_scanline;
      if (jpeg_read_scanlines (&d->jpeg, &row, 1) != 1)
        {
          status = SANE_STATUS_IO_ERROR;
          goto done;
        }
    }
  if (!jpeg_finish_decompress (&d->jpeg) || d->warning)
    {
      status = SANE_STATUS_IO_ERROR;
      goto done;
    }
  page->data = d->pixels;
  d->pixels = NULL;
  page->size = stride * height;
  page->width = width;
  page->height = height;
  status = SANE_STATUS_GOOD;
done:
  if (d->created)
    jpeg_destroy_decompress (&d->jpeg);
  free (d->pixels);
  free (d);
  return status;
}
