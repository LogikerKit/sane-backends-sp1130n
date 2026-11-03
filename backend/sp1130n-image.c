/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#include "../include/sane/config.h"
#include "../include/sane/sane.h"
#include "../include/sane/sanei_magic.h"
#include "sp1130n-image.h"
#include <stdlib.h>
#include <string.h>

int
sanei_sp1130n_image_options_validate (const struct sanei_sp1130n_image_options
                                      *options)
{
  if (!options ||
      (options->mode != SP_IMAGE_COLOR && options->mode != SP_IMAGE_GRAY &&
       options->mode != SP_IMAGE_LINEART) ||
      options->brightness < -100 || options->brightness > 100 ||
      options->contrast < -100 || options->contrast > 100 ||
      options->threshold > 255 ||
      (options->rotate180 != 0 && options->rotate180 != 1))
    return SP_INVALID;
  return SP_OK;
}

size_t
sanei_sp1130n_image_stride (unsigned width, enum sanei_sp1130n_image_mode mode)
{
  size_t stride = width;
  if (!stride)
    return 0;
  switch (mode)
    {
    case SP_IMAGE_COLOR:
      if (stride > SIZE_MAX / 3)
        return 0;
      return stride * 3;
    case SP_IMAGE_GRAY:
      return stride;
    case SP_IMAGE_LINEART:
      return stride / 8 + (stride % 8 != 0);
    default:
      return 0;
    }
}

static int
page_size (size_t stride, unsigned height, size_t *size)
{
  if (!stride || !height || stride > SIZE_MAX / height)
    return SP_INVALID;
  *size = stride * height;
  return *size > SP_PAGE_LIMIT ? SP_INVALID : SP_OK;
}

static uint8_t
adjusted (unsigned sample, const struct sanei_sp1130n_image_options *options)
{
  int value = (((int) sample - 128) * (100 + options->contrast)) / 100
    + 128 + options->brightness * 255 / 100;
  if (value < 0)
    return 0;
  if (value > 255)
    return 255;
  return (uint8_t) value;
}

int
sanei_sp1130n_image_process (struct sanei_sp1130n_page *page,
                             const struct sanei_sp1130n_image_options *options)
{
  if (!page || !page->data || !page->width || !page->height ||
      page->width > 10000 || page->height > 20000 ||
      sanei_sp1130n_image_options_validate (options) != SP_OK)
    return SP_INVALID;

  size_t input_stride =
    sanei_sp1130n_image_stride (page->width, SP_IMAGE_COLOR);
  size_t output_stride =
    sanei_sp1130n_image_stride (page->width, options->mode);
  size_t input_size, output_size;
  if (page_size (input_stride, page->height, &input_size) != SP_OK ||
      page->size != input_size ||
      page_size (output_stride, page->height, &output_size) != SP_OK)
    return SP_INVALID;

  if (options->mode == SP_IMAGE_COLOR && !options->brightness &&
      !options->contrast && !options->rotate180)
    return SP_OK;

  /* Rotate byte samples before packing Lineart: odd-width rows stay correct. */
  int gray_rotation = options->rotate180 && options->mode == SP_IMAGE_LINEART;
  size_t working_size = gray_rotation ?
    (size_t) page->width * page->height : output_size;
  uint8_t *output = malloc (working_size);
  if (!output)
    return SP_NOMEM;
  if (options->mode == SP_IMAGE_LINEART && !gray_rotation)
    memset (output, 0, output_size);

  for (unsigned y = 0; y < page->height; ++y)
    {
      const uint8_t *input_row = page->data + (size_t) y * input_stride;
      uint8_t *output_row =
        output + (size_t) y * (gray_rotation ? page->width : output_stride);
      for (unsigned x = 0; x < page->width; ++x)
        {
          const uint8_t *pixel = input_row + (size_t) x * 3;
          if (options->mode == SP_IMAGE_COLOR)
            {
              for (unsigned channel = 0; channel < 3; ++channel)
                output_row[(size_t) x * 3 + channel] =
                  adjusted (pixel[channel], options);
            }
          else
            {
              unsigned luma = (77u * pixel[0] + 150u * pixel[1] +
                               29u * pixel[2] + 128u) >> 8;
              uint8_t sample = adjusted (luma, options);
              if (options->mode == SP_IMAGE_GRAY || gray_rotation)
                output_row[x] = sample;
              else if (sample < options->threshold)
                output_row[x / 8] |= (uint8_t) (0x80u >> (x % 8));
            }
        }
    }

  if (options->rotate180)
    {
      SANE_Parameters params = {
        options->mode == SP_IMAGE_COLOR ? SANE_FRAME_RGB : SANE_FRAME_GRAY,
        SANE_TRUE,
        (SANE_Int) (gray_rotation ? page->width : output_stride),
        (SANE_Int) page->width, (SANE_Int) page->height, 8
      };
      SANE_Status status = sanei_magic_turn (&params, output, 180);
      if (status != SANE_STATUS_GOOD)
        {
          free (output);
          return status == SANE_STATUS_NO_MEM ? SP_NOMEM : SP_IO;
        }
      if (gray_rotation)
        {
          /* Forward packing overwrites only samples already consumed. */
          for (unsigned y = 0; y < page->height; ++y)
            {
              uint8_t bits = 0;
              for (unsigned x = 0; x < page->width; ++x)
                {
                  if (output[(size_t) y * page->width + x] < options->threshold)
                    bits |= (uint8_t) (0x80u >> (x % 8));
                  if (x % 8 == 7 || x + 1 == page->width)
                    {
                      output[(size_t) y * output_stride + x / 8] = bits;
                      bits = 0;
                    }
                }
            }
        }
    }

  free (page->data);
  page->data = output;
  page->size = output_size;
  return SP_OK;
}
