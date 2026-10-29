/* SPDX-License-Identifier: MIT
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

static int
repair_geometry (uint8_t *p, size_t size, unsigned width, unsigned height)
{
  if (size < 4 || p[0] != 0xff || p[1] != 0xd8)
    return SP_IO;
  size_t position = 2, restart = 0;
  unsigned mcu_width = 0;
  int found = 0, changed = 0;
  while (position < size)
    {
      if (p[position++] != 0xff)
        return SP_IO;
      while (position < size && p[position] == 0xff)
        ++position;
      if (position == size)
        return SP_IO;
      unsigned marker = p[position++];
      if (marker == 0xda)
        {
          if (!found)
            return SP_IO;
          if (changed && restart && mcu_width)
            put_be16 (p + restart, (width + mcu_width - 1) / mcu_width);
          return SP_OK;
        }
      if (marker == 0xd8 || marker == 0xd9 || marker == 0
          || (marker >= 0xd0 && marker <= 0xd7))
        return SP_IO;
      if (size - position < 2)
        return SP_IO;
      unsigned length = be16 (p + position);
      if (length < 2 || length > size - position)
        return SP_IO;
      if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2)
        {
          if (found || length < 8 || p[position + 2] != 8)
            return SP_UNSUPPORTED;
          unsigned components = p[position + 7];
          if (!components || components > 3 || length != 8 + components * 3)
            return SP_IO;
          changed = be16 (p + position + 3) != height ||
            be16 (p + position + 5) != width;
          put_be16 (p + position + 3, height);
          put_be16 (p + position + 5, width);
          for (unsigned i = 0; i < components; ++i)
            {
              unsigned sampling = p[position + 9 + 3 * i] >> 4;
              if (!sampling || sampling > 4)
                return SP_IO;
              if (mcu_width < sampling * 8)
                mcu_width = sampling * 8;
            }
          found = 1;
        }
      if (marker == 0xdd)
        {
          if (length != 4)
            return SP_IO;
          restart = position + 2;
        }
      position += length;
    }
  return SP_IO;
}

int
sanei_sp1130n_jpeg_geometry (const uint8_t *p, size_t size,
                             unsigned *width, unsigned *height)
{
  if (!p || !width || !height || size < 4 || p[0] != 0xff || p[1] != 0xd8)
    return SP_IO;
  size_t position = 2;
  while (position < size)
    {
      if (p[position++] != 0xff)
        return SP_IO;
      while (position < size && p[position] == 0xff)
        ++position;
      if (position == size)
        return SP_IO;
      unsigned marker = p[position++];
      if (marker == 0xda || marker == 0xd8 || marker == 0xd9 || !marker ||
          (marker >= 0xd0 && marker <= 0xd7) || size - position < 2)
        return SP_IO;
      unsigned length = be16 (p + position);
      if (length < 2 || length > size - position)
        return SP_IO;
      if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2)
        {
          if (length < 8 || p[position + 2] != 8)
            return SP_UNSUPPORTED;
          unsigned w = be16 (p + position + 5);
          unsigned h = be16 (p + position + 3);
          if (!w || !h || w > 10000 || h > 20000 ||
              (uint64_t) w * h * 3 > SP_PAGE_LIMIT)
            return SP_IO;
          *width = w;
          *height = h;
          return SP_OK;
        }
      position += length;
    }
  return SP_IO;
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

int
sanei_sp1130n_decode_jpeg (uint8_t *bytes, size_t size, unsigned width,
                           unsigned height, struct sanei_sp1130n_page *page)
{
  if (!bytes || !page || !width || !height || width > 10000 || height > 20000 ||
      (uint64_t) width * height * 3 > SP_PAGE_LIMIT)
    return SP_INVALID;
  memset (page, 0, sizeof *page);
  int status = repair_geometry (bytes, size, width, height);
  if (status != SP_OK)
    return status;
  /* Heap state stays well-defined after libjpeg's longjmp error exit. */
  struct decoder *d = calloc (1, sizeof *d);
  if (!d)
    return SP_NOMEM;
  d->jpeg.err = jpeg_std_error (&d->errors);
  d->errors.error_exit = jpeg_failure;
  d->errors.emit_message = jpeg_message;
  d->jpeg.client_data = d;
  if (setjmp (d->jump))
    {
      status = SP_IO;
      goto done;
    }
  jpeg_create_decompress (&d->jpeg);
  d->created = 1;
  jpeg_mem_src (&d->jpeg, bytes, (unsigned long) size);
  if (jpeg_read_header (&d->jpeg, TRUE) != JPEG_HEADER_OK)
    {
      status = SP_IO;
      goto done;
    }
  d->jpeg.out_color_space = JCS_RGB;
  if (!jpeg_start_decompress (&d->jpeg) || d->jpeg.output_width != width ||
      d->jpeg.output_height != height || d->jpeg.output_components != 3)
    {
      status = SP_IO;
      goto done;
    }
  size_t stride = (size_t) width * 3;
  d->pixels = malloc (stride * height);
  if (!d->pixels)
    {
      status = SP_NOMEM;
      goto done;
    }
  while (d->jpeg.output_scanline < height)
    {
      JSAMPROW row = d->pixels + stride * d->jpeg.output_scanline;
      if (jpeg_read_scanlines (&d->jpeg, &row, 1) != 1)
        {
          status = SP_IO;
          goto done;
        }
    }
  if (!jpeg_finish_decompress (&d->jpeg) || d->warning)
    {
      status = SP_IO;
      goto done;
    }
  page->data = d->pixels;
  d->pixels = NULL;
  page->size = stride * height;
  page->width = width;
  page->height = height;
  status = SP_OK;
done:
  if (d->created)
    jpeg_destroy_decompress (&d->jpeg);
  free (d->pixels);
  free (d);
  return status;
}
