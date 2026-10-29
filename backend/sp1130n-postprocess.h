/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#ifndef SP_POSTPROCESS_H
#define SP_POSTPROCESS_H
#include "sp1130n.h"

struct sanei_sp1130n_post_options
{
  int autocrop, deskew;
  unsigned content_threshold;   /* RGB luma below this is content, 0..255. */
  unsigned blank_per_mille;     /* Maximum dark-pixel fraction for blank, 0..1000. */
  unsigned crop_margin;         /* Pixels to retain around the content bounds. */
};
struct sanei_sp1130n_post_result
{
  int blank;
  int deskew_tenths;            /* Applied rotation in tenths of a degree, -50..50. */
  unsigned dark_per_mille;
};
int sanei_sp1130n_post_options_validate (const struct sanei_sp1130n_post_options
                                         *);
/* Fresh RGB8 input, owned memory; success may replace it. Errors preserve it.
 * The optional cancellation flag is checked during bounded image processing.
 * Defaults: all processing off, threshold 240, blank 1 per mille, margin 8.
 * Content-based cropping is not physical paper-edge detection.
 */
int sanei_sp1130n_postprocess (struct sanei_sp1130n_page *,
                               const struct sanei_sp1130n_post_options *,
                               const atomic_bool *,
                               struct sanei_sp1130n_post_result *);
#endif
