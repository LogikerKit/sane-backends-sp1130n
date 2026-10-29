/* SPDX-License-Identifier: MIT */
#include "sp1130n-image.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SP_IMAGE_TEST_ALLOC_FAILURE
/* Optional GNU/LLVM linker fault injection: -Wl,--wrap=malloc. */
static int fail_next_allocation;
void *__real_malloc (size_t);
void *
__wrap_malloc (size_t size)
{
  if (fail_next_allocation)
    {
      fail_next_allocation = 0;
      return NULL;
    }
  return __real_malloc (size);
}
#endif

static struct sanei_sp1130n_page
rgb_page (unsigned width, unsigned height, const uint8_t *data)
{
  size_t size = (size_t) width * height * 3;
  uint8_t *copy = malloc (size);
  assert (copy);
  memcpy (copy, data, size);
  return (struct sanei_sp1130n_page)
  { copy, size, width, height };
}

static struct sanei_sp1130n_page
mono_page (unsigned width, unsigned height, const uint8_t *samples)
{
  size_t pixels = (size_t) width * height;
  uint8_t *data = malloc (pixels * 3);
  assert (data);
  for (size_t i = 0; i < pixels; ++i)
    for (unsigned channel = 0; channel < 3; ++channel)
      data[i * 3 + channel] = samples[i];
  return (struct sanei_sp1130n_page)
  { data, pixels * 3, width, height };
}

static void
expect_page (const struct sanei_sp1130n_page *page, unsigned width,
             unsigned height, const uint8_t *expected, size_t size)
{
  assert (page->data && page->width == width && page->height == height);
  assert (page->size == size);
  assert (!memcmp (page->data, expected, size));
}

static uint8_t
reference_adjust (unsigned sample, int brightness, int contrast)
{
  long long value = ((long long) sample - 128) * (100 + contrast) / 100;
  value += 128 + (long long) brightness *255 / 100;
  return (uint8_t) (value < 0 ? 0 : value > 255 ? 255 : value);
}

static unsigned
reference_luma (const uint8_t *pixel)
{
  return (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2] + 128u) >> 8;
}

static void
options_and_stride (void)
{
  struct sanei_sp1130n_image_options options = { 0 };
  assert (sanei_sp1130n_image_options_validate (NULL) == SP_INVALID);
  assert (sanei_sp1130n_image_options_validate (&options) == SP_OK);
  for (int mode = SP_IMAGE_COLOR; mode <= SP_IMAGE_LINEART; ++mode)
    {
      options = (struct sanei_sp1130n_image_options)
      { (enum sanei_sp1130n_image_mode) mode, -100, 100, 255, 1 };
      assert (sanei_sp1130n_image_options_validate (&options) == SP_OK);
      options.brightness = 100;
      options.contrast = -100;
      assert (sanei_sp1130n_image_options_validate (&options) == SP_OK);
      assert (sanei_sp1130n_image_stride (0, options.mode) == 0);
    }
  const struct sanei_sp1130n_image_options invalid[] = {
    {.mode = (enum sanei_sp1130n_image_mode) -1},
    {.mode = (enum sanei_sp1130n_image_mode) 3},
    {.mode = (enum sanei_sp1130n_image_mode) INT_MAX},
    {.brightness = -101}, {.brightness = 101},
    {.brightness = INT_MIN}, {.brightness = INT_MAX},
    {.contrast = -101}, {.contrast = 101},
    {.contrast = INT_MIN}, {.contrast = INT_MAX},
    {.threshold = 256}, {.threshold = UINT_MAX},
    {.rotate180 = -1}, {.rotate180 = 2},
    {.rotate180 = INT_MIN}, {.rotate180 = INT_MAX}
  };
  for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
    assert (sanei_sp1130n_image_options_validate (&invalid[i]) == SP_INVALID);

  assert (sanei_sp1130n_image_stride (1, SP_IMAGE_COLOR) == 3);
  assert (sanei_sp1130n_image_stride (10000, SP_IMAGE_COLOR) == 30000);
  assert (sanei_sp1130n_image_stride (1, SP_IMAGE_GRAY) == 1);
  assert (sanei_sp1130n_image_stride (UINT_MAX, SP_IMAGE_GRAY) ==
          (size_t) UINT_MAX);
  const unsigned widths[] = { 1, 7, 8, 9, 15, 16, 17, 255, 256, 257, UINT_MAX };
  for (size_t i = 0; i < sizeof widths / sizeof widths[0]; ++i)
    {
      size_t width = widths[i];
      assert (sanei_sp1130n_image_stride (widths[i], SP_IMAGE_LINEART) ==
              width / 8 + (width % 8 != 0));
    }
  assert (sanei_sp1130n_image_stride (1, (enum sanei_sp1130n_image_mode) -1) ==
          0);
  assert (sanei_sp1130n_image_stride (1, (enum sanei_sp1130n_image_mode) 3) ==
          0);
  size_t largest_width = UINT_MAX;
  size_t expected = largest_width > SIZE_MAX / 3 ? 0 : largest_width * 3;
  assert (sanei_sp1130n_image_stride (UINT_MAX, SP_IMAGE_COLOR) == expected);
#if SIZE_MAX / 3 < UINT_MAX
  unsigned largest_color = (unsigned) (SIZE_MAX / 3);
  assert (sanei_sp1130n_image_stride (largest_color, SP_IMAGE_COLOR) ==
          (size_t) largest_color * 3);
  assert (sanei_sp1130n_image_stride (largest_color + 1, SP_IMAGE_COLOR) == 0);
#endif
}

static void
color_identity_and_gray (void)
{
  const uint8_t rgb[] = {
    255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0,
    255, 255, 255, 255, 255, 0, 0, 255, 255, 255, 0, 255,
    12, 34, 56, 1, 2, 3, 127, 128, 129, 128, 128, 128
  };
  const uint8_t gray[] =
    { 77, 149, 29, 0, 255, 226, 178, 106, 30, 2, 128, 128 };
  struct sanei_sp1130n_image_options options = { 0 };
  struct sanei_sp1130n_page page = rgb_page (4, 3, rgb);
  uint8_t *original = page.data;
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  assert (page.data == original);
  expect_page (&page, 4, 3, rgb, sizeof rgb);
  options.threshold = 255;
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  assert (page.data == original);
  expect_page (&page, 4, 3, rgb, sizeof rgb);
  options.mode = SP_IMAGE_GRAY;
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  expect_page (&page, 4, 3, gray, sizeof gray);
  free (page.data);

  for (int mode = SP_IMAGE_COLOR; mode <= SP_IMAGE_LINEART; ++mode)
    {
      options = (struct sanei_sp1130n_image_options)
      { (enum sanei_sp1130n_image_mode) mode, 7, 33, 143, 0 };
      page = rgb_page (4, 3, rgb);
      assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
      uint8_t expected[sizeof rgb] = { 0 };
      for (size_t i = 0; i < sizeof gray; ++i)
        {
          if (mode == SP_IMAGE_COLOR)
            {
              for (unsigned channel = 0; channel < 3; ++channel)
                expected[i * 3 + channel] =
                  reference_adjust (rgb[i * 3 + channel], 7, 33);
            }
          else
            {
              uint8_t adjusted =
                reference_adjust (reference_luma (rgb + i * 3), 7, 33);
              if (mode == SP_IMAGE_GRAY)
                expected[i] = adjusted;
              else if (adjusted < options.threshold)
                expected[i / 4] |= (uint8_t) (0x80u >> (i % 4));
            }
        }
      size_t size = sanei_sp1130n_image_stride (4, options.mode) * 3;
      expect_page (&page, 4, 3, expected, size);
      free (page.data);
    }
}

static void
lineart_rows_and_threshold (void)
{
  const uint8_t samples[] = {
    0, 127, 128, 255, 0, 128, 127, 255, 0,
    255, 128, 127, 0, 255, 127, 128, 0, 255
  };
  const uint8_t expected[] = { 0xca, 0x80, 0x35, 0x00 };
  struct sanei_sp1130n_image_options options = {.mode =
      SP_IMAGE_LINEART,.threshold = 128 };
  struct sanei_sp1130n_page page = mono_page (9, 2, samples);
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  expect_page (&page, 9, 2, expected, sizeof expected);
  free (page.data);

  uint8_t ramp[256];
  for (unsigned sample = 0; sample < 256; ++sample)
    ramp[sample] = (uint8_t) sample;
  for (unsigned threshold = 0; threshold < 256; ++threshold)
    {
      options.threshold = threshold;
      page = mono_page (256, 1, ramp);
      assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
      assert (page.size == 32);
      for (unsigned sample = 0; sample < 256; ++sample)
        assert (((page.data[sample / 8] >> (7 - sample % 8)) & 1u) ==
                (unsigned) (sample < threshold));
      free (page.data);
    }

  uint8_t solid[129];
  for (unsigned width = 1; width <= 43; ++width)
    {
      for (unsigned white = 0; white <= 1; ++white)
        {
          memset (solid, white ? 255 : 0, sizeof solid);
          options.threshold = 128;
          page = mono_page (width, 3, solid);
          assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
          size_t stride = sanei_sp1130n_image_stride (width, SP_IMAGE_LINEART);
          assert (page.size == stride * 3);
          for (unsigned row = 0; row < 3; ++row)
            {
              for (size_t byte = 0; byte < stride; ++byte)
                {
                  uint8_t bits = white ? 0 : 255;
                  if (!white && byte == stride - 1 && width % 8)
                    bits = (uint8_t) (0xffu << (8 - width % 8));
                  assert (page.data[row * stride + byte] == bits);
                }
            }
          free (page.data);
        }
    }
}

static void
adjustment_boundaries (void)
{
  const uint8_t samples[] = { 0, 1, 127, 128, 129, 254, 255 };
  const struct
  {
    int brightness, contrast;
    uint8_t expected[7];
  } cases[] = {
    {0, 0, {0, 1, 127, 128, 129, 254, 255}},
    {100, 0, {255, 255, 255, 255, 255, 255, 255}},
    {-100, 0, {0, 0, 0, 0, 0, 0, 0}},
    {0, -100, {128, 128, 128, 128, 128, 128, 128}},
    {0, 100, {0, 0, 126, 128, 130, 255, 255}},
    {1, 0, {2, 3, 129, 130, 131, 255, 255}},
    {-1, 0, {0, 0, 125, 126, 127, 252, 253}},
    {0, -99, {127, 127, 128, 128, 128, 129, 129}},
    {100, 100, {127, 129, 255, 255, 255, 255, 255}},
    {-100, 100, {0, 0, 0, 0, 0, 125, 127}}
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
      struct sanei_sp1130n_image_options options = {
        SP_IMAGE_GRAY, cases[i].brightness, cases[i].contrast, 128, 0
      };
      struct sanei_sp1130n_page page = mono_page (7, 1, samples);
      assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
      expect_page (&page, 7, 1, cases[i].expected, sizeof cases[i].expected);
      free (page.data);
    }
}

static void
exhaustive_adjustment (void)
{
  uint8_t samples[256];
  for (unsigned sample = 0; sample < 256; ++sample)
    samples[sample] = (uint8_t) sample;
  for (int brightness = -100; brightness <= 100; ++brightness)
    {
      for (int contrast = -100; contrast <= 100; ++contrast)
        {
          for (int mode = SP_IMAGE_COLOR; mode <= SP_IMAGE_LINEART; ++mode)
            {
              struct sanei_sp1130n_image_options options = {
                (enum sanei_sp1130n_image_mode) mode, brightness, contrast, 128,
                  0
              };
              struct sanei_sp1130n_page page = mono_page (256, 1, samples);
              assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
              assert (page.size ==
                      sanei_sp1130n_image_stride (256, options.mode));
              assert (page.width == 256 && page.height == 1);
              for (unsigned sample = 0; sample < 256; ++sample)
                {
                  uint8_t expected =
                    reference_adjust (sample, brightness, contrast);
                  if (mode == SP_IMAGE_COLOR)
                    {
                      for (unsigned channel = 0; channel < 3; ++channel)
                        assert (page.data[sample * 3 + channel] == expected);
                    }
                  else if (mode == SP_IMAGE_GRAY)
                    {
                      assert (page.data[sample] == expected);
                    }
                  else
                    {
                      assert (((page.
                                data[sample / 8] >> (7 - sample % 8)) & 1u) ==
                              (unsigned) (expected < 128));
                    }
                }
              free (page.data);
            }
        }
    }
}

static void
rotation (void)
{
  const uint8_t rgb[] =
    { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
  const uint8_t reversed[] =
    { 16, 17, 18, 13, 14, 15, 10, 11, 12, 7, 8, 9, 4, 5, 6, 1, 2, 3 };
  struct sanei_sp1130n_image_options options = {.rotate180 = 1 };
  struct sanei_sp1130n_page page = rgb_page (3, 2, rgb);
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  expect_page (&page, 3, 2, reversed, sizeof reversed);
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  expect_page (&page, 3, 2, rgb, sizeof rgb);
  free (page.data);

  const uint8_t samples[] = { 0, 255, 127, 128, 1, 254 };
  const uint8_t gray[] = { 254, 1, 128, 127, 255, 0 };
  const uint8_t bits[] = { 0x40, 0xa0 };
  options.mode = SP_IMAGE_GRAY;
  page = mono_page (3, 2, samples);
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  expect_page (&page, 3, 2, gray, sizeof gray);
  free (page.data);
  options.mode = SP_IMAGE_LINEART;
  options.threshold = 128;
  page = mono_page (3, 2, samples);
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  expect_page (&page, 3, 2, bits, sizeof bits);
  free (page.data);

  for (unsigned height = 1; height <= 5; ++height)
    {
      for (unsigned width = 1; width <= 11; ++width)
        {
          uint8_t source[11 * 5 * 3];
          size_t pixels = (size_t) width * height;
          for (size_t i = 0; i < pixels * 3; ++i)
            source[i] = (uint8_t) (i * 47);
          for (int mode = SP_IMAGE_COLOR; mode <= SP_IMAGE_LINEART; ++mode)
            {
              options = (struct sanei_sp1130n_image_options)
              { (enum sanei_sp1130n_image_mode) mode, -9, 17, 91, 1 };
              page = rgb_page (width, height, source);
              assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
              uint8_t expected[sizeof source] = { 0 };
              size_t stride = sanei_sp1130n_image_stride (width, options.mode);
              for (size_t i = 0; i < pixels; ++i)
                {
                  const uint8_t *pixel = source + (pixels - 1 - i) * 3;
                  if (mode == SP_IMAGE_COLOR)
                    {
                      for (unsigned channel = 0; channel < 3; ++channel)
                        expected[i * 3 + channel] =
                          reference_adjust (pixel[channel], -9, 17);
                    }
                  else
                    {
                      uint8_t value =
                        reference_adjust (reference_luma (pixel), -9, 17);
                      if (mode == SP_IMAGE_GRAY)
                        expected[i] = value;
                      else if (value < options.threshold)
                        expected[(i / width) * stride + (i % width) / 8] |=
                          (uint8_t) (0x80u >> ((i % width) % 8));
                    }
                }
              expect_page (&page, width, height, expected, stride * height);
              free (page.data);
            }
        }
    }
}

static void
expect_invalid_preserved (struct sanei_sp1130n_page *page,
                          const struct sanei_sp1130n_image_options *options)
{
  struct sanei_sp1130n_page before = *page;
  uint8_t bytes[12];
  if (before.data)
    memcpy (bytes, before.data, sizeof bytes);
  assert (sanei_sp1130n_image_process (page, options) == SP_INVALID);
  assert (page->data == before.data && page->size == before.size);
  assert (page->width == before.width && page->height == before.height);
  if (before.data)
    assert (!memcmp (page->data, bytes, sizeof bytes));
}

static void
invalid_inputs (void)
{
  const uint8_t rgb[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
  struct sanei_sp1130n_image_options options = { 0 };
  assert (sanei_sp1130n_image_process (NULL, &options) == SP_INVALID);
  assert (sanei_sp1130n_image_process (NULL, NULL) == SP_INVALID);
  struct sanei_sp1130n_page page = rgb_page (2, 2, rgb);
  expect_invalid_preserved (&page, NULL);
  const struct sanei_sp1130n_image_options invalid[] = {
    {.mode = (enum sanei_sp1130n_image_mode) -1}, {.mode =
                                                   (enum
                                                    sanei_sp1130n_image_mode)
                                                   3},
    {.brightness = -101}, {.brightness = 101}, {.brightness = INT_MIN},
    {.brightness = INT_MAX}, {.contrast = -101}, {.contrast = 101},
    {.contrast = INT_MIN}, {.contrast = INT_MAX}, {.threshold = 256},
    {.threshold = UINT_MAX}, {.rotate180 = -1}, {.rotate180 = 2}
  };
  for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
    expect_invalid_preserved (&page, &invalid[i]);

  struct sanei_sp1130n_page original = page;
  const size_t invalid_sizes[] =
    { 0, 1, 4, 11, 13, SP_PAGE_LIMIT + 1, SIZE_MAX };
  for (size_t i = 0; i < sizeof invalid_sizes / sizeof invalid_sizes[0]; ++i)
    {
      page = original;
      page.size = invalid_sizes[i];
      expect_invalid_preserved (&page, &options);
    }
  const unsigned invalid_dimensions[][2] = {
    {0, 2}, {2, 0}, {0, 0}, {10001, 1}, {1, 20001},
    {UINT_MAX, 1}, {1, UINT_MAX}, {UINT_MAX, UINT_MAX},
    {10000, 20000}, {10000, 4474}, {2237, 20000}
  };
  for (size_t i = 0;
       i < sizeof invalid_dimensions / sizeof invalid_dimensions[0]; ++i)
    {
      page = original;
      page.width = invalid_dimensions[i][0];
      page.height = invalid_dimensions[i][1];
      page.size = SP_PAGE_LIMIT;
      expect_invalid_preserved (&page, &options);
    }
  page = original;
  page.data = NULL;
  expect_invalid_preserved (&page, &options);
  free (original.data);

  uint8_t zeros[4 * 3] = { 0 };
  for (int mode = SP_IMAGE_COLOR; mode <= SP_IMAGE_LINEART; ++mode)
    {
      page = rgb_page (2, 2, zeros);
      options.mode = (enum sanei_sp1130n_image_mode) mode;
      assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
      page.size = SP_PAGE_LIMIT + 1;
      struct sanei_sp1130n_page before = page;
      assert (sanei_sp1130n_image_process (&page, &options) == SP_INVALID);
      assert (page.data == before.data && page.size == before.size);
      assert (page.width == before.width && page.height == before.height);
      free (page.data);
    }
}

#ifdef SP_IMAGE_TEST_ALLOC_FAILURE
static void
allocation_failure (void)
{
  const uint8_t rgb[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
  for (int mode = SP_IMAGE_COLOR; mode <= SP_IMAGE_LINEART; ++mode)
    {
      struct sanei_sp1130n_page page = rgb_page (2, 2, rgb);
      struct sanei_sp1130n_page original = page;
      struct sanei_sp1130n_image_options options =
        { (enum sanei_sp1130n_image_mode) mode, 1, 0, 128, 1 };
      fail_next_allocation = 1;
      assert (sanei_sp1130n_image_process (&page, &options) == SP_NOMEM);
      assert (!fail_next_allocation);
      assert (page.data == original.data);
      expect_page (&page, 2, 2, rgb, sizeof rgb);
      assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
      free (page.data);
    }
  struct sanei_sp1130n_page page = rgb_page (2, 2, rgb);
  struct sanei_sp1130n_image_options options = { 0 };
  fail_next_allocation = 1;
  assert (sanei_sp1130n_image_process (&page, &options) == SP_OK);
  assert (fail_next_allocation);
  fail_next_allocation = 0;
  expect_page (&page, 2, 2, rgb, sizeof rgb);
  free (page.data);
}
#endif

int
main (void)
{
  options_and_stride ();
  color_identity_and_gray ();
  lineart_rows_and_threshold ();
  adjustment_boundaries ();
  exhaustive_adjustment ();
  rotation ();
  invalid_inputs ();
#ifdef SP_IMAGE_TEST_ALLOC_FAILURE
  allocation_failure ();
  puts
    ("Allocation-failure tests passed: SP_NOMEM preservation, retry, allocation-free identity.");
#endif
  puts
    ("Image tests passed: primaries, RGB identity, luma-first adjustment, black-bit polarity,");
  puts
    ("row padding, all thresholds, 121203 adjustment combinations x 256 samples,");
  puts
    ("rotation, invalid-input preservation, geometry/page bounds, stride overflow checks.");
  return 0;
}
