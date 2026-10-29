/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#include "../include/sane/config.h"
#include "sp1130n.h"

int
sanei_sp1130n_scan_geometry (const struct sanei_sp1130n_settings *s,
                             struct sanei_sp1130n_geometry *result)
{
  if (!s || !result || (s->dpi != 300 && s->dpi != 600) ||
      s->width_mm < 50 || s->width_mm > 216 || s->height_mm < 50 ||
      s->height_mm > 356 || (s->source != SP_SOURCE_FRONT &&
                             s->source != SP_SOURCE_DUPLEX
                             && s->source != SP_SOURCE_BACK)
      || s->left_mm >= s->width_mm || s->top_mm >= s->height_mm)
    return SP_INVALID;
  unsigned width_mm =
    s->scan_width_mm ? s->scan_width_mm : s->width_mm - s->left_mm;
  unsigned height_mm =
    s->scan_height_mm ? s->scan_height_mm : s->height_mm - s->top_mm;
  if (width_mm > s->width_mm - s->left_mm
      || height_mm > s->height_mm - s->top_mm)
    return SP_INVALID;
  unsigned width = width_mm * s->dpi * 10 / 254;
  unsigned height = height_mm * s->dpi * 10 / 254;
  unsigned page_width = s->width_mm * s->dpi * 10 / 254;
  unsigned page_height = s->height_mm * s->dpi * 10 / 254;
  /* Center the physical paper, then apply the window origin within it. */
  unsigned feeder_width = 10201 * s->dpi / 1200;
  unsigned centered =
    page_width < feeder_width ? (feeder_width - page_width) / 2 : 0;
  *result = (struct sanei_sp1130n_geometry)
  { width, height,
    centered + s->left_mm * s->dpi * 10 / 254, s->top_mm * s->dpi * 10 / 254,
    page_width, page_height
  };
  return SP_OK;
}
