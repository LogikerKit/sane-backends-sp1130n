/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
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
  unsigned blank_per_mille;     /* Maximum average darkness, 0..1000. */
  unsigned crop_margin;         /* Pixels to retain around detected media edges. */
};
struct sanei_sp1130n_post_result
{
  int blank;
};
SANE_Status sanei_sp1130n_post_options_validate (const struct sanei_sp1130n_post_options
                                         *);
/* Fresh RGB8 input, owned memory; success may replace it. Errors preserve it.
 * Uses sanei_magic for media-edge crop/deskew and whole-image blank detection.
 * Blank classification follows any requested geometry changes.
 * Initialize sanei_magic before use. Resolution must be 50..600 dpi.
 * Cancellation is checked between shared helper calls, not inside them.
 * Defaults: crop/deskew off, blank 1 per mille, margin 8 pixels.
 * No detected edges means no geometric change; deskew is limited to 5 degrees.
 */
SANE_Status sanei_sp1130n_postprocess (struct sanei_sp1130n_page *,
                               const struct sanei_sp1130n_post_options *,
                               unsigned dpi,
                               const atomic_bool *,
                               struct sanei_sp1130n_post_result *);
#endif
