/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#ifndef SP_IMAGE_H
#define SP_IMAGE_H

#include "sp1130n.h"

enum sanei_sp1130n_image_mode
{
  SP_IMAGE_COLOR = 0,
  SP_IMAGE_GRAY = 1,
  SP_IMAGE_LINEART = 2
};

struct sanei_sp1130n_image_options
{
  enum sanei_sp1130n_image_mode mode;
  int brightness;
  int contrast;
  unsigned threshold;
  int rotate180;
};

/* Brightness/contrast: -100..100; threshold: 0..255; rotate180: 0 or 1.
 * Adjustment uses C integer division (truncation toward zero):
 * clamp(((sample - 128) * (100 + contrast)) / 100
 *       + 128 + brightness * 255 / 100), clamped to 0..255.
 * Color adjusts each channel; gray/lineart first compute RGB8 luma:
 * (77 * r + 150 * g + 29 * b + 128) >> 8.
 */
int sanei_sp1130n_image_options_validate (const struct
                                          sanei_sp1130n_image_options *);

/* Packed bytes per row, or zero for zero width, invalid mode, or overflow.
 * Color: width * 3; gray: width; lineart: ceil(width / 8).
 * Lineart is MSB-first, 1 = black (adjusted luma < threshold), 0 = white.
 * Each row starts on a byte boundary; unused padding bits are white (zero).
 * Polarity/packing references:
 * https://gitlab.com/sane-project/backends/-/raw/master/frontend/scanimage.c
 * https://netpbm.sourceforge.net/doc/pbm.html
 */
size_t sanei_sp1130n_image_stride (unsigned width,
                                   enum sanei_sp1130n_image_mode);

/* Input must own freshly decoded packed RGB8, exactly width * height * 3
 * bytes, with width 1..10000, height 1..20000, and size <= SP_PAGE_LIMIT.
 * Rotate via source coordinates before conversion, preserving geometry.
 * Success frees input and installs an owned replacement with size equal to
 * stride * height; unadjusted, unrotated color may retain the input unchanged.
 * Invalid input/options returns SP_INVALID, allocation failure SP_NOMEM.
 * Every error preserves all page fields and input bytes.
 */
int sanei_sp1130n_image_process (struct sanei_sp1130n_page *,
                                 const struct sanei_sp1130n_image_options *);

#endif
