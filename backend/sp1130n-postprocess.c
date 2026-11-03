/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#include "../include/sane/config.h"
#include "../include/sane/sane.h"
#include "../include/sane/sanei_magic.h"
#include "sp1130n-postprocess.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int
is_cancelled (const atomic_bool *cancelled)
{
  return cancelled && atomic_load_explicit (cancelled, memory_order_relaxed);
}

int
sanei_sp1130n_post_options_validate (const struct sanei_sp1130n_post_options
                                     *options)
{
  if (!options || (options->autocrop != 0 && options->autocrop != 1) ||
      (options->deskew != 0 && options->deskew != 1) ||
      options->blank_per_mille > 1000)
    return SP_INVALID;
  return SP_OK;
}

static int
parameters (const struct sanei_sp1130n_page *page, SANE_Parameters *params)
{
  if (!page || !page->data || !page->width || !page->height ||
      page->width > 10000 || page->height > 20000)
    return SP_INVALID;
  size_t stride = (size_t) page->width * 3;
  if (stride > SIZE_MAX / page->height ||
      page->size != stride * page->height || page->size > SP_PAGE_LIMIT ||
      page->size > INT_MAX)
    return SP_INVALID;
  *params = (SANE_Parameters)
  { SANE_FRAME_RGB, SANE_TRUE, (SANE_Int) stride,
    (SANE_Int) page->width, (SANE_Int) page->height, 8 };
  return SP_OK;
}

static int
magic_status (SANE_Status status)
{
  switch (status)
    {
    case SANE_STATUS_GOOD:
      return SP_OK;
    case SANE_STATUS_NO_MEM:
      return SP_NOMEM;
    case SANE_STATUS_INVAL:
      return SP_INVALID;
    default:
      return SP_IO;
    }
}

int
sanei_sp1130n_postprocess (struct sanei_sp1130n_page *page,
                           const struct sanei_sp1130n_post_options *options,
                           unsigned dpi, const atomic_bool *cancelled,
                           struct sanei_sp1130n_post_result *result)
{
  if (result)
    *result = (struct sanei_sp1130n_post_result) { 0 };
  SANE_Parameters params;
  if (!result || sanei_sp1130n_post_options_validate (options) != SP_OK ||
      dpi < 50 || dpi > 600 || parameters (page, &params) != SP_OK)
    return SP_INVALID;
  if (is_cancelled (cancelled))
    return SP_CANCELLED;

  SANE_Status status;
  struct sanei_sp1130n_post_result answer = { 0 };
  uint8_t *working = NULL;
  int changed = 0, rc = SP_OK;
  if (!options->autocrop && !options->deskew)
    goto classify;

  /* Shared helpers modify buffers in place; publish only a complete result. */
  working = malloc (page->size);
  if (!working)
    return is_cancelled (cancelled) ? SP_CANCELLED : SP_NOMEM;
  memcpy (working, page->data, page->size);
  if (is_cancelled (cancelled))
    {
      rc = SP_CANCELLED;
      goto done;
    }
  if (options->deskew)
    {
      int center_x = 0, center_y = 0;
      double slope = 0;
      status = sanei_magic_findSkew (&params, working, (int) dpi, (int) dpi,
                                    &center_x, &center_y, &slope);
      if (is_cancelled (cancelled))
        {
          rc = SP_CANCELLED;
          goto done;
        }
      if (status != SANE_STATUS_GOOD && status != SANE_STATUS_UNSUPPORTED)
        {
          rc = magic_status (status);
          goto done;
        }
      /* Retain the five-degree limit and safe integer coordinates. */
      if (status == SANE_STATUS_GOOD && isfinite (slope) &&
          fabs (atan (slope)) <= 5 * (3.14159265358979323846 / 180) &&
          center_x >= -INT_MAX / 4 && center_x <= INT_MAX / 4 &&
          center_y >= -INT_MAX / 4 && center_y <= INT_MAX / 4)
        {
          status = sanei_magic_rotate (&params, working, center_x, center_y,
                                       slope, 255);
          rc = magic_status (status);
          if (rc != SP_OK)
            goto done;
          changed = 1;
        }
    }
  if (is_cancelled (cancelled))
    {
      rc = SP_CANCELLED;
      goto done;
    }
  if (options->autocrop)
    {
      int top = 0, bottom = 0, left = 0, right = 0;
      status = sanei_magic_findEdges (&params, working, (int) dpi, (int) dpi,
                                     &top, &bottom, &left, &right);
      if (is_cancelled (cancelled))
        {
          rc = SP_CANCELLED;
          goto done;
        }
      if (status != SANE_STATUS_GOOD && status != SANE_STATUS_UNSUPPORTED)
        {
          rc = magic_status (status);
          goto done;
        }
      if (status == SANE_STATUS_GOOD)
        {
          if (top < 0 || bottom < top || bottom >= params.lines ||
              left < 0 || right < left || right >= params.pixels_per_line)
            {
              rc = SP_IO;
              goto done;
            }
          /* findEdges returns pixel indices; crop expects exclusive bounds. */
          unsigned margin = options->crop_margin;
          top = margin < (unsigned) top ? top - (int) margin : 0;
          left = margin < (unsigned) left ? left - (int) margin : 0;
          ++bottom;
          ++right;
          bottom += (int) (margin < (unsigned) (params.lines - bottom) ?
                           margin : (unsigned) (params.lines - bottom));
          right += (int) (margin < (unsigned) (params.pixels_per_line - right) ?
                          margin : (unsigned) (params.pixels_per_line - right));
          if (top || left || bottom != params.lines ||
              right != params.pixels_per_line)
            {
              status = sanei_magic_crop (&params, working, top, bottom,
                                         left, right);
              rc = magic_status (status);
              if (rc != SP_OK)
                goto done;
              changed = 1;
            }
        }
    }
classify:
  if (is_cancelled (cancelled))
    {
      rc = SP_CANCELLED;
      goto done;
    }
  /* Classify after geometry changes, including all remaining edge pixels. */
  status = sanei_magic_isBlank (&params, working ? working : page->data,
                               options->blank_per_mille / 10.0);
  if (is_cancelled (cancelled))
    rc = SP_CANCELLED;
  else if (status != SANE_STATUS_GOOD && status != SANE_STATUS_NO_DOCS)
    rc = magic_status (status);
  else
    answer.blank = status == SANE_STATUS_NO_DOCS;
done:
  if (rc == SP_OK)
    {
      if (changed)
        {
          free (page->data);
          page->data = working;
          page->width = (unsigned) params.pixels_per_line;
          page->height = (unsigned) params.lines;
          page->size = (size_t) params.bytes_per_line * page->height;
          working = NULL;
        }
      *result = answer;
    }
  free (working);
  return rc;
}
