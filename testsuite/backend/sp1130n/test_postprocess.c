/* SPDX-License-Identifier: MIT */
#include "sp1130n-postprocess.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct sanei_sp1130n_post_options defaults = {
  .content_threshold = 240,.blank_per_mille = 1,.crop_margin = 8
};

#ifdef SP_POST_TEST_ALLOC_FAILURE
/* GNU/LLVM linker fault injection: -Wl,--wrap=malloc; calloc is not used. */
static volatile unsigned allocation_number, fail_allocation, cancel_allocation;
static atomic_bool *volatile allocation_cancel_flag;
void *__real_malloc (size_t);
void *
__wrap_malloc (size_t size)
{
  ++allocation_number;
  if (allocation_number == cancel_allocation && allocation_cancel_flag)
    atomic_store (allocation_cancel_flag, 1);
  if (allocation_number == fail_allocation)
    return NULL;
  return __real_malloc (size);
}

static void
arm_allocations (unsigned fail, unsigned cancel, atomic_bool *flag)
{
  allocation_number = 0;
  fail_allocation = fail;
  cancel_allocation = cancel;
  allocation_cancel_flag = flag;
}
#endif

static struct sanei_sp1130n_page
solid_page (unsigned width, unsigned height, uint8_t sample)
{
  size_t size = (size_t) width * height * 3;
  uint8_t *data = malloc (size);
  assert (data);
  memset (data, sample, size);
  return (struct sanei_sp1130n_page)
  { data, size, width, height };
}

static struct sanei_sp1130n_page
copy_page (const struct sanei_sp1130n_page *page)
{
  struct sanei_sp1130n_page copy = solid_page (page->width, page->height, 255);
  memcpy (copy.data, page->data, page->size);
  return copy;
}

static void
put_gray (struct sanei_sp1130n_page *page, unsigned x, unsigned y,
          uint8_t sample)
{
  assert (x < page->width && y < page->height);
  memset (page->data + ((size_t) y * page->width + x) * 3, sample, 3);
}

static void
assert_same (const struct sanei_sp1130n_page *page,
             const struct sanei_sp1130n_page *before, const uint8_t *bytes)
{
  assert (page->data == before->data && page->size == before->size);
  assert (page->width == before->width && page->height == before->height);
  if (bytes)
    assert (!memcmp (page->data, bytes, page->size));
}

static void
assert_zero_result (const struct sanei_sp1130n_post_result *result)
{
  assert (!result->blank && !result->deskew_tenths && !result->dark_per_mille);
}

static unsigned
reference_luma (const uint8_t *pixel)
{
  return (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2] + 128u) >> 8;
}

static uint64_t
dark_count (const struct sanei_sp1130n_page *page, unsigned threshold)
{
  uint64_t dark = 0;
  for (size_t i = 0; i < page->size; i += 3)
    dark += reference_luma (page->data + i) < threshold;
  return dark;
}

static void
assert_classification (const struct sanei_sp1130n_page *original,
                       const struct sanei_sp1130n_post_options *options,
                       const struct sanei_sp1130n_post_result *result)
{
  uint64_t pixels = (uint64_t) original->width * original->height;
  uint64_t dark = dark_count (original, options->content_threshold);
  assert (result->blank ==
          (dark * 1000 <= (uint64_t) options->blank_per_mille * pixels));
  assert (result->dark_per_mille == (dark * 1000 + pixels / 2) / pixels);
}

static void
validate_options (void)
{
  assert (sanei_sp1130n_post_options_validate (NULL) == SP_INVALID);
  assert (sanei_sp1130n_post_options_validate (&defaults) == SP_OK);
  struct sanei_sp1130n_post_options options = {
    .autocrop = 1,.deskew = 1,.content_threshold = 255,
    .blank_per_mille = 1000,.crop_margin = UINT_MAX
  };
  assert (sanei_sp1130n_post_options_validate (&options) == SP_OK);
  options.content_threshold = 0;
  options.blank_per_mille = 0;
  assert (sanei_sp1130n_post_options_validate (&options) == SP_OK);
  const struct sanei_sp1130n_post_options invalid[] = {
    {.autocrop = -1}, {.autocrop = 2}, {.autocrop = INT_MIN},
    {.autocrop = INT_MAX}, {.deskew = -1}, {.deskew = 2},
    {.deskew = INT_MIN}, {.deskew = INT_MAX}, {.content_threshold = 256},
    {.content_threshold = UINT_MAX}, {.blank_per_mille = 1001},
    {.blank_per_mille = UINT_MAX}
  };
  for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
    assert (sanei_sp1130n_post_options_validate (&invalid[i]) == SP_INVALID);
}

static void
default_identity_and_color_luma (void)
{
  struct sanei_sp1130n_page page = solid_page (4, 2, 255);
  const uint8_t rgb[] = {
    255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128,
    240, 240, 240, 239, 239, 239, 0, 0, 0, 255, 255, 255
  };
  memcpy (page.data, rgb, sizeof rgb);
  struct sanei_sp1130n_page before = page;
  struct sanei_sp1130n_post_result result = { 1, 50, 1000 };
  assert (sanei_sp1130n_postprocess (&page, &defaults, NULL, &result) == SP_OK);
  assert_same (&page, &before, rgb);
  assert (!result.blank && !result.deskew_tenths
          && result.dark_per_mille == 750);
  assert_classification (&before, &defaults, &result);
  struct sanei_sp1130n_post_options options = defaults;
  options.content_threshold = 128;
  assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) == SP_OK);
  assert_same (&page, &before, rgb);
  assert (result.dark_per_mille == 375);
  options.content_threshold = 0;
  options.autocrop = options.deskew = 1;
  assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) == SP_OK);
  assert_same (&page, &before, rgb);
  assert (result.blank && !result.dark_per_mille && !result.deskew_tenths);
  free (page.data);
}

static void
exact_blank_classification (void)
{
  const struct
  {
    unsigned pixels, dark, limit, rounded;
    int blank;
  } cases[] = {
    {700, 1, 1, 1, 0}, {1000, 1, 1, 1, 1},
    {1000, 2, 1, 2, 0}, {2000, 1, 0, 1, 0},
    {3000, 1, 0, 0, 0}, {3000, 3, 1, 1, 1},
    {3000, 4, 1, 1, 0}, {1000, 0, 0, 0, 1},
    {1000, 1000, 999, 1000, 0}, {1000, 1000, 1000, 1000, 1}
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
      struct sanei_sp1130n_page page = solid_page (cases[i].pixels, 1, 255);
      for (unsigned x = 0; x < cases[i].dark; ++x)
        put_gray (&page, x, 0, 0);
      struct sanei_sp1130n_page original = copy_page (&page), before = page;
      struct sanei_sp1130n_post_options options = defaults;
      options.blank_per_mille = cases[i].limit;
      struct sanei_sp1130n_post_result result;
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert_same (&page, &before, original.data);
      assert (result.blank == cases[i].blank);
      assert (result.dark_per_mille == cases[i].rounded);
      assert (!result.deskew_tenths);
      assert_classification (&original, &options, &result);
      free (original.data);
      free (page.data);
    }
  /* One dark pixel, including the final pixel, must not disappear in sampling. */
  struct sanei_sp1130n_page page = solid_page (1000, 1000, 255);
  put_gray (&page, 999, 999, 0);
  struct sanei_sp1130n_post_options options = defaults;
  options.blank_per_mille = 0;
  struct sanei_sp1130n_post_result result;
  assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) == SP_OK);
  assert (!result.blank && !result.dark_per_mille);
  free (page.data);
}

static void
crop_bounds_and_margins (void)
{
  struct sanei_sp1130n_page fixture = solid_page (31, 27, 255);
  for (unsigned y = 6; y <= 17; ++y)
    for (unsigned x = 9; x <= 20; ++x)
      put_gray (&fixture, x, y, (uint8_t) (x + y));
  const unsigned margins[] = { 0, 1, 3, 8, 30, 10000, UINT_MAX };
  for (size_t i = 0; i < sizeof margins / sizeof margins[0]; ++i)
    {
      unsigned margin = margins[i];
      unsigned left = margin < 9 ? 9 - margin : 0;
      unsigned top = margin < 6 ? 6 - margin : 0;
      unsigned right = margin < 10 ? 20 + margin : 30;
      unsigned bottom = margin < 9 ? 17 + margin : 26;
      unsigned width = right - left + 1, height = bottom - top + 1;
      struct sanei_sp1130n_page page = copy_page (&fixture), before = page;
      struct sanei_sp1130n_post_options options = defaults;
      options.autocrop = 1;
      options.crop_margin = margin;
      struct sanei_sp1130n_post_result result;
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert (page.width == width && page.height == height);
      assert (page.size == (size_t) width * height * 3);
      assert ((page.data == before.data) == (width == 31 && height == 27));
      for (unsigned y = 0; y < height; ++y)
        assert (!memcmp (page.data + (size_t) y * width * 3,
                         fixture.data + ((size_t) (top + y) * 31 + left) * 3,
                         (size_t) width * 3));
      assert_classification (&fixture, &options, &result);
      assert (!result.deskew_tenths);
      free (page.data);
    }
  free (fixture.data);

  for (unsigned corner = 0; corner < 4; ++corner)
    {
      struct sanei_sp1130n_page page = solid_page (13, 17, 255);
      unsigned x = corner & 1 ? 12 : 0;
      unsigned y = corner & 2 ? 16 : 0;
      put_gray (&page, x, y, 0);
      struct sanei_sp1130n_post_options options = defaults;
      options.autocrop = 1;
      options.crop_margin = 0;
      options.blank_per_mille = 0;
      struct sanei_sp1130n_post_result result;
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert (page.width == 1 && page.height == 1 && page.size == 3);
      assert (!page.data[0] && !page.data[1] && !page.data[2]);
      free (page.data);
    }
}

static void
blank_geometry_identity (void)
{
  struct sanei_sp1130n_post_options options = defaults;
  options.autocrop = options.deskew = 1;
  options.crop_margin = 0;
  for (unsigned noise = 0; noise <= 1; ++noise)
    {
      struct sanei_sp1130n_page page = solid_page (320, 240, 255);
      if (noise)
        put_gray (&page, 150, 100, 0);
      struct sanei_sp1130n_page original = copy_page (&page), before = page;
      struct sanei_sp1130n_post_result result;
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert_same (&page, &before, original.data);
      assert (result.blank && !result.deskew_tenths);
      assert_classification (&original, &options, &result);
      free (original.data);
      free (page.data);
    }
}

/* Independently authored 5x7 uppercase glyphs, not bars standing in for text. */
static const uint8_t font[26][7] = {
  {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
  {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
  {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
  {14, 17, 16, 23, 17, 17, 14}, {17, 17, 17, 31, 17, 17, 17},
  {31, 4, 4, 4, 4, 4, 31}, {7, 2, 2, 2, 18, 18, 12},
  {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
  {17, 27, 21, 21, 17, 17, 17}, {17, 25, 25, 21, 19, 19, 17},
  {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
  {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
  {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
  {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
  {17, 17, 17, 21, 21, 27, 17}, {17, 17, 10, 4, 10, 17, 17},
  {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}
};

static void
draw_text (struct sanei_sp1130n_page *page, unsigned x, unsigned y,
           unsigned scale, const char *text)
{
  for (; *text; ++text, x += 6 * scale)
    {
      if (*text == ' ')
        continue;
      assert (*text >= 'A' && *text <= 'Z');
      const uint8_t *glyph = font[(unsigned) (*text - 'A')];
      for (unsigned row = 0; row < 7; ++row)
        for (unsigned col = 0; col < 5; ++col)
          if ((glyph[row] >> (4 - col)) & 1u)
            for (unsigned dy = 0; dy < scale; ++dy)
              for (unsigned dx = 0; dx < scale; ++dx)
                put_gray (page, x + col * scale + dx, y + row * scale + dy, 0);
    }
}

static struct sanei_sp1130n_page
text_page (unsigned scale)
{
  struct sanei_sp1130n_page page = solid_page (256 * scale, 224 * scale, 255);
  for (unsigned line = 0; line < 12; ++line)
    draw_text (&page, 26 * scale, (24 + line * 15) * scale, scale,
               line % 2 ? "RGB SCANNER DOCUMENT TEXT LINES" :
               "OPEN SOURCE DESKEW SAMPLE TEXT");
  return page;
}

static struct sanei_sp1130n_page
slant_fixture (const struct sanei_sp1130n_page *source, int tenths)
{
  struct sanei_sp1130n_page result =
    solid_page (source->width, source->height, 255);
  double angle = tenths * (3.14159265358979323846 / 1800.0);
  double c = cos (angle), s = sin (angle);
  double cx = ((double) source->width - 1) / 2;
  double cy = ((double) source->height - 1) / 2;
  for (unsigned y = 0; y < source->height; ++y)
    {
      for (unsigned x = 0; x < source->width; ++x)
        {
          double dx = (double) x - cx, dy = (double) y - cy;
          long sx = lround (c * dx + s * dy + cx);
          long sy = lround (-s * dx + c * dy + cy);
          if (sx < 0 || sy < 0 || sx >= source->width || sy >= source->height)
            continue;
          memcpy (result.data + ((size_t) y * result.width + x) * 3,
                  source->data + ((size_t) sy * source->width +
                                  (size_t) sx) * 3, 3);
        }
    }
  return result;
}

static uint64_t
horizontal_score (const struct sanei_sp1130n_page *page)
{
  uint64_t score = 0;
  for (unsigned y = 0; y < page->height; ++y)
    {
      const uint8_t *row = page->data + (size_t) y * page->width * 3;
      uint64_t count = 0;
      for (unsigned x = 0; x < page->width; ++x)
        count += reference_luma (row + (size_t) x * 3) < 128;
      score += count * count;
    }
  return score;
}

static void
deskew_rendered_text (void)
{
  struct sanei_sp1130n_post_options options = defaults;
  options.deskew = 1;
  const int angles[] = { -46, -32, -14, 14, 32, 46 };
  for (unsigned scale = 2; scale <= 4; ++scale)
    {
      struct sanei_sp1130n_page horizontal = text_page (scale);
      struct sanei_sp1130n_page page = copy_page (&horizontal), before = page;
      struct sanei_sp1130n_post_result result = { 1, 50, 1000 };
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert_same (&page, &before, horizontal.data);
      assert (!result.blank && !result.deskew_tenths);
      free (page.data);
      for (size_t i = 0; i < sizeof angles / sizeof angles[0]; ++i)
        {
          page = slant_fixture (&horizontal, angles[i]);
          struct sanei_sp1130n_page original = copy_page (&page);
          uint64_t score_before = horizontal_score (&page);
          assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
                  SP_OK);
          printf
            ("Deskew fixture %ux%u, input %+d tenths, applied %+d tenths.\n",
             page.width, page.height, angles[i], result.deskew_tenths);
          assert (abs (result.deskew_tenths + angles[i]) <= 4);
          assert (result.deskew_tenths * angles[i] < 0);
          assert (page.width == horizontal.width
                  && page.height == horizontal.height);
          assert (page.size == horizontal.size);
          assert (horizontal_score (&page) * 100 > score_before * 130);
          assert_classification (&original, &options, &result);
          for (unsigned corner = 0; corner < 4; ++corner)
            {
              unsigned x = corner & 1 ? page.width - 1 : 0;
              unsigned y = corner & 2 ? page.height - 1 : 0;
              const uint8_t *pixel =
                page.data + ((size_t) y * page.width + x) * 3;
              assert (pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255);
            }
          assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
                  SP_OK);
          assert (!result.deskew_tenths);
          free (original.data);
          free (page.data);
        }
      free (horizontal.data);
    }
}

static void
deskew_sampling_bounds (void)
{
  const struct
  {
    unsigned width, height, scale, lines, left, top, gap;
  } fixtures[] = {
    {1280, 2048, 4, 28, 280, 100, 64},  /* Exactly 640x1024 after sampling. */
    {2480, 3508, 8, 26, 480, 200, 120}  /* Scanner-sized RGB document. */
  };
  struct sanei_sp1130n_post_options options = defaults;
  options.deskew = 1;
  for (size_t f = 0; f < sizeof fixtures / sizeof fixtures[0]; ++f)
    {
      struct sanei_sp1130n_page horizontal =
        solid_page (fixtures[f].width, fixtures[f].height, 255);
      for (unsigned line = 0; line < fixtures[f].lines; ++line)
        draw_text (&horizontal, fixtures[f].left,
                   fixtures[f].top + line * fixtures[f].gap, fixtures[f].scale,
                   line % 2 ? "RGB SCANNER DOCUMENT TEXT LINES" :
                   "OPEN SOURCE DESKEW SAMPLE TEXT");
      for (int angle = -32; angle <= 32; angle += 64)
        {
          struct sanei_sp1130n_page page = slant_fixture (&horizontal, angle);
          struct sanei_sp1130n_post_result result;
          uint64_t dark = dark_count (&page, options.content_threshold);
          uint64_t pixels = (uint64_t) page.width * page.height;
          uint64_t score_before = horizontal_score (&page);
          assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
                  SP_OK);
          printf ("Bounded deskew fixture %ux%u, applied %+d tenths.\n",
                  page.width, page.height, result.deskew_tenths);
          assert (abs (result.deskew_tenths + angle) <= 4);
          assert (page.width == horizontal.width
                  && page.height == horizontal.height);
          assert (page.size == horizontal.size);
          assert (horizontal_score (&page) * 100 > score_before * 130);
          assert (result.dark_per_mille == (dark * 1000 + pixels / 2) / pixels);
          free (page.data);
        }
      free (horizontal.data);
    }
}

static uint32_t
random_word (uint32_t *state)
{
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

static void
conservative_skips (void)
{
  struct sanei_sp1130n_post_options options = defaults;
  options.deskew = 1;
  for (unsigned fixture = 0; fixture < 7; ++fixture)
    {
      struct sanei_sp1130n_page page = solid_page (512, 448, 255);
      uint32_t random = 98765;
      for (unsigned y = 0; y < page.height; ++y)
        {
          for (unsigned x = 0; x < page.width; ++x)
            {
              uint8_t *pixel = page.data + ((size_t) y * page.width + x) * 3;
              if (fixture == 0)
                {
                  /* Near-white sensor noise. */
                  memset (pixel, 235 + random_word (&random) % 21, 3);
                }
              else if (fixture == 1)
                {
                  /* Sparse, high-contrast salt noise, not text components. */
                  if (random_word (&random) % 16 == 0)
                    memset (pixel, 0, 3);
                }
              else if (fixture == 2)
                {
                  /* Smooth color photograph-like gradient. */
                  pixel[0] = (uint8_t) (x * 255 / (page.width - 1));
                  pixel[1] = (uint8_t) (y * 255 / (page.height - 1));
                  pixel[2] =
                    (uint8_t) ((x + y) * 255 / (page.width + page.height - 2));
                }
              else if (fixture == 3)
                {
                  /* Low-contrast grayscale photograph-like texture. */
                  memset (pixel, 180 + random_word (&random) % 76, 3);
                }
              else if (fixture == 4)
                {
                  /* Repeated rules have projection peaks but no glyphs. */
                  if (y >= 48 && y <= 400 && y % 32 < 3 && x >= 60 && x <= 450)
                    memset (pixel, 0, 3);
                }
              else if (fixture == 5)
                {
                  /* Solid rectangular shapes are not glyph-like components. */
                  if (y % 48 >= 10 && y % 48 <= 20 && x % 48 >= 10
                      && x % 48 <= 20)
                    memset (pixel, 0, 3);
                }
              else if (fixture == 6)
                {
                  memset (pixel, 0, 3);
                }
            }
        }
      struct sanei_sp1130n_page original = copy_page (&page), before = page;
      struct sanei_sp1130n_post_result result = { 1, 50, 1000 };
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert_same (&page, &before, original.data);
      assert (!result.deskew_tenths);
      assert_classification (&original, &options, &result);
      free (original.data);
      free (page.data);
    }
  struct sanei_sp1130n_page horizontal = text_page (3);
  const int uncertain_angles[] = { -70, -50, -2, -1, 0, 1, 2, 50, 70 };
  for (size_t i = 0; i < sizeof uncertain_angles / sizeof uncertain_angles[0];
       ++i)
    {
      struct sanei_sp1130n_page page =
        slant_fixture (&horizontal, uncertain_angles[i]);
      struct sanei_sp1130n_page original = copy_page (&page), before = page;
      struct sanei_sp1130n_post_result result;
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert_same (&page, &before, original.data);
      assert (!result.deskew_tenths);
      free (original.data);
      free (page.data);
    }
  free (horizontal.data);
}

static void
combined_deskew_crop (void)
{
  struct sanei_sp1130n_page horizontal = text_page (3);
  struct sanei_sp1130n_page page = slant_fixture (&horizontal, 32);
  struct sanei_sp1130n_page original = copy_page (&page);
  struct sanei_sp1130n_post_options options = defaults;
  options.deskew = options.autocrop = 1;
  options.crop_margin = 7;
  struct sanei_sp1130n_post_result result;
  assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) == SP_OK);
  assert (abs (result.deskew_tenths + 32) <= 4);
  assert (page.width < horizontal.width && page.height < horizontal.height);
  assert (page.width > 100 && page.height > 100);
  assert_classification (&original, &options, &result);
  unsigned left = page.width, top = page.height, right = 0, bottom = 0;
  for (unsigned y = 0; y < page.height; ++y)
    {
      for (unsigned x = 0; x < page.width; ++x)
        {
          if (reference_luma (page.data + ((size_t) y * page.width + x) * 3) >=
              240)
            continue;
          if (x < left)
            left = x;
          if (y < top)
            top = y;
          if (x > right)
            right = x;
          if (y > bottom)
            bottom = y;
        }
    }
  assert (left == 7 && top == 7);
  assert (page.width - 1 - right == 7 && page.height - 1 - bottom == 7);
  free (original.data);
  free (page.data);
  free (horizontal.data);
}

static void
assert_invalid (struct sanei_sp1130n_page *page,
                const struct sanei_sp1130n_post_options *options)
{
  struct sanei_sp1130n_page before = *page;
  struct sanei_sp1130n_post_result result = { 1, 50, 1000 };
  /* Invalid geometry may describe more memory than exists; compare the fixture only. */
  uint8_t bytes[12];
  if (page->data)
    memcpy (bytes, page->data, sizeof bytes);
  assert (sanei_sp1130n_postprocess (page, options, NULL, &result) ==
          SP_INVALID);
  assert_same (page, &before, NULL);
  if (page->data)
    assert (!memcmp (bytes, page->data, sizeof bytes));
  assert_zero_result (&result);
}

static void
invalid_inputs_and_precancel (void)
{
  struct sanei_sp1130n_post_result result = { 1, 50, 1000 };
  assert (sanei_sp1130n_postprocess (NULL, &defaults, NULL, &result) ==
          SP_INVALID);
  assert_zero_result (&result);
  assert (sanei_sp1130n_postprocess (NULL, NULL, NULL, NULL) == SP_INVALID);
  struct sanei_sp1130n_page page = solid_page (2, 2, 128), original = page;
  assert_invalid (&page, NULL);
  struct sanei_sp1130n_post_options options = defaults;
  options.autocrop = 2;
  assert_invalid (&page, &options);
  options = defaults;
  options.deskew = -1;
  assert_invalid (&page, &options);
  options = defaults;
  options.content_threshold = 256;
  assert_invalid (&page, &options);
  options = defaults;
  options.blank_per_mille = UINT_MAX;
  assert_invalid (&page, &options);
  const size_t sizes[] = { 0, 1, 4, 11, 13, SP_PAGE_LIMIT + 1, SIZE_MAX };
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i)
    {
      page = original;
      page.size = sizes[i];
      assert_invalid (&page, &defaults);
    }
  const unsigned dimensions[][2] = {
    {0, 2}, {2, 0}, {10001, 1}, {1, 20001}, {UINT_MAX, UINT_MAX},
    {10000, 20000}, {10000, 4474}, {2237, 20000}
  };
  for (size_t i = 0; i < sizeof dimensions / sizeof dimensions[0]; ++i)
    {
      page = original;
      page.width = dimensions[i][0];
      page.height = dimensions[i][1];
      page.size = SP_PAGE_LIMIT;
      assert_invalid (&page, &defaults);
    }
  page = original;
  page.data = NULL;
  assert_invalid (&page, &defaults);
  page = original;
  assert (sanei_sp1130n_postprocess (&page, &defaults, NULL, NULL) ==
          SP_INVALID);
  assert_same (&page, &original, NULL);
  atomic_bool cancelled = ATOMIC_VAR_INIT (1);
  for (unsigned flags = 0; flags < 4; ++flags)
    {
      options = defaults;
      options.autocrop = (flags & 1) != 0;
      options.deskew = (flags & 2) != 0;
      result = (struct sanei_sp1130n_post_result)
      { 1, 50, 1000 };
      assert (sanei_sp1130n_postprocess (&page, &options, &cancelled, &result)
              == SP_CANCELLED);
      assert_same (&page, &original, NULL);
      for (size_t i = 0; i < page.size; ++i)
        assert (page.data[i] == 128);
      assert_zero_result (&result);
    }
  atomic_store (&cancelled, 0);
  assert (sanei_sp1130n_postprocess (&page, &defaults, &cancelled, &result) ==
          SP_OK);
  assert_same (&page, &original, NULL);
  free (page.data);
}

static void
large_geometry_bound (void)
{
  /* Largest supported width/height and a page near the byte cap remain valid. */
  const unsigned dimensions[][2] = { {10000, 1}, {1, 20000}, {2236, 20000} };
  for (size_t i = 0; i < sizeof dimensions / sizeof dimensions[0]; ++i)
    {
      struct sanei_sp1130n_page page =
        solid_page (dimensions[i][0], dimensions[i][1], 255);
      struct sanei_sp1130n_page before = page;
      struct sanei_sp1130n_post_options options = defaults;
      options.autocrop = options.deskew = 1;
      struct sanei_sp1130n_post_result result;
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert_same (&page, &before, NULL);
      assert (result.blank && !result.dark_per_mille && !result.deskew_tenths);
      free (page.data);
    }
}

#ifdef SP_POST_TEST_ALLOC_FAILURE
static void
fault_preservation (void)
{
  struct sanei_sp1130n_page horizontal = text_page (3);
  struct sanei_sp1130n_page fixture = slant_fixture (&horizontal, 32);
  struct sanei_sp1130n_post_options options = defaults;
  options.deskew = options.autocrop = 1;
  struct sanei_sp1130n_page probe = copy_page (&fixture);
  struct sanei_sp1130n_post_result result;
  arm_allocations (0, 0, NULL);
  assert (sanei_sp1130n_postprocess (&probe, &options, NULL, &result) == SP_OK);
  unsigned total = allocation_number;
  assert (total == 5);          /* Counts, mask, points, rotation, crop. */
  assert (result.deskew_tenths && probe.width < fixture.width);
  arm_allocations (0, 0, NULL);
  free (probe.data);
  for (unsigned failure = 1; failure <= total; ++failure)
    {
      struct sanei_sp1130n_page page = copy_page (&fixture), before = page;
      result = (struct sanei_sp1130n_post_result)
      { 1, 50, 1000 };
      arm_allocations (failure, 0, NULL);
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_NOMEM);
      assert (allocation_number >= failure);
      assert_same (&page, &before, fixture.data);
      assert_zero_result (&result);
      arm_allocations (0, 0, NULL);
      assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
              SP_OK);
      assert (result.deskew_tenths);
      free (page.data);
    }
  for (unsigned cancellation = 1; cancellation <= total; ++cancellation)
    {
      struct sanei_sp1130n_page page = copy_page (&fixture), before = page;
      atomic_bool cancelled = ATOMIC_VAR_INIT (0);
      result = (struct sanei_sp1130n_post_result)
      { 1, 50, 1000 };
      arm_allocations (0, cancellation, &cancelled);
      assert (sanei_sp1130n_postprocess (&page, &options, &cancelled, &result)
              == SP_CANCELLED);
      assert (allocation_number >= cancellation);
      assert_same (&page, &before, fixture.data);
      assert_zero_result (&result);
      arm_allocations (0, 0, NULL);
      atomic_store (&cancelled, 0);
      assert (sanei_sp1130n_postprocess (&page, &options, &cancelled, &result)
              == SP_OK);
      free (page.data);
    }
  struct sanei_sp1130n_page page = copy_page (&fixture), before = page;
  arm_allocations (1, 0, NULL);
  assert (sanei_sp1130n_postprocess (&page, &defaults, NULL, &result) == SP_OK);
  assert (!allocation_number);
  assert_same (&page, &before, fixture.data);
  arm_allocations (0, 0, NULL);
  free (page.data);

  page = solid_page (128, 128, 255);
  put_gray (&page, 64, 64, 0);
  before = page;
  options = defaults;
  options.autocrop = 1;
  options.blank_per_mille = 0;
  arm_allocations (1, 0, NULL);
  assert (sanei_sp1130n_postprocess (&page, &options, NULL, &result) ==
          SP_NOMEM);
  assert_same (&page, &before, NULL);
  assert_zero_result (&result);
  arm_allocations (0, 0, NULL);
  atomic_bool cancelled = ATOMIC_VAR_INIT (0);
  arm_allocations (0, 1, &cancelled);
  assert (sanei_sp1130n_postprocess (&page, &options, &cancelled, &result) ==
          SP_CANCELLED);
  assert_same (&page, &before, NULL);
  assert_zero_result (&result);
  arm_allocations (0, 0, NULL);
  free (page.data);
  free (fixture.data);
  free (horizontal.data);
  puts
    ("Postprocess allocation/cancellation faults preserve the complete original page.");
}
#endif

int
main (void)
{
  validate_options ();
  default_identity_and_color_luma ();
  exact_blank_classification ();
  crop_bounds_and_margins ();
  blank_geometry_identity ();
  deskew_rendered_text ();
  deskew_sampling_bounds ();
  conservative_skips ();
  combined_deskew_crop ();
  invalid_inputs_and_precancel ();
  large_geometry_bound ();
#ifdef SP_POST_TEST_ALLOC_FAILURE
  fault_preservation ();
#endif
  puts
    ("Postprocess tests passed: exact RGB luma/fractions, bounded content crop, blank");
  puts
    ("geometry identity, real rendered text at both skew signs, confidence skips,");
  puts
    ("white-filled RGB rotation, invalid-input preservation and cancellation.");
  return 0;
}
