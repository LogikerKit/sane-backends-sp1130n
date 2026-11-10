/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception */
#include "sp1130n-postprocess.h"
#include "sane/sane.h"
#include "sane/sanei_magic.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct sanei_sp1130n_post_options defaults = {
  .blank_per_mille = 1, .crop_margin = 8
};

#ifdef SP_POST_TEST_ALLOC_FAILURE
static unsigned allocation_number, fail_allocation, cancel_allocation;
static atomic_bool *allocation_cancel_flag;
void *__real_malloc (size_t);
void *__real_calloc (size_t, size_t);

static int
allocation_fails (void)
{
  ++allocation_number;
  if (allocation_number == cancel_allocation && allocation_cancel_flag)
    atomic_store (allocation_cancel_flag, 1);
  return allocation_number == fail_allocation;
}

void *
__wrap_malloc (size_t size)
{
  return allocation_fails () ? NULL : __real_malloc (size);
}

void *
__wrap_calloc (size_t count, size_t size)
{
  return allocation_fails () ? NULL : __real_calloc (count, size);
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
  return (struct sanei_sp1130n_page) { data, size, width, height };
}

static struct sanei_sp1130n_page
copy_page (const struct sanei_sp1130n_page *page)
{
  struct sanei_sp1130n_page copy = solid_page (page->width, page->height, 255);
  memcpy (copy.data, page->data, page->size);
  return copy;
}

static SANE_Parameters
page_parameters (const struct sanei_sp1130n_page *page)
{
  return (SANE_Parameters) {
    SANE_FRAME_RGB, SANE_TRUE, (SANE_Int) page->width * 3,
    (SANE_Int) page->width, (SANE_Int) page->height, 8
  };
}

static void
assert_same (const struct sanei_sp1130n_page *page,
             const struct sanei_sp1130n_page *before, const uint8_t *bytes)
{
  assert (page->data == before->data && page->size == before->size);
  assert (page->width == before->width && page->height == before->height);
  if (bytes)
    assert (!memcmp (page->data, bytes, 12));
}

static void
assert_equal (const struct sanei_sp1130n_page *page,
              const struct sanei_sp1130n_page *expected)
{
  assert (page->width == expected->width && page->height == expected->height);
  assert (page->size == expected->size);
  assert (!memcmp (page->data, expected->data, page->size));
}

/* A white media rectangle on a dark scanner background, not a text box. */
static struct sanei_sp1130n_page
media_page (int tenths)
{
  struct sanei_sp1130n_page page = solid_page (640, 480, 0);
  double angle = tenths * (3.14159265358979323846 / 1800);
  double c = cos (angle), s = sin (angle);
  for (unsigned y = 0; y < page.height; ++y)
    for (unsigned x = 0; x < page.width; ++x)
      {
        double dx = (double) x - 320, dy = (double) y - 240;
        double sx = c * dx + s * dy + 320;
        double sy = -s * dx + c * dy + 240;
        if (sx >= 60 && sx < 580 && sy >= 40 && sy < 440)
          {
            uint8_t *pixel = page.data + ((size_t) y * page.width + x) * 3;
            memset (pixel, 255, 3);
            if (sx >= 180 && sx < 400 && sy >= 180 && sy < 260)
              {
                pixel[0] = 128;
                pixel[1] = 64;
                pixel[2] = 32;
              }
          }
      }
  return page;
}

static void
blank_and_identity (void)
{
  const uint8_t samples[] = { 0, 128, 239, 240, 254, 255 };
  const unsigned limits[] = { 0, 1, 4, 100, 500, 1000 };
  for (size_t i = 0; i < sizeof samples; ++i)
    for (size_t j = 0; j < sizeof limits / sizeof limits[0]; ++j)
      {
        struct sanei_sp1130n_page page = solid_page (19, 13, samples[i]);
        struct sanei_sp1130n_page before = page;
        struct sanei_sp1130n_post_options options = defaults;
        options.blank_per_mille = limits[j];
        SANE_Parameters params = page_parameters (&page);
        SANE_Status expected =
          sanei_magic_isBlank (&params, page.data, limits[j] / 10.0);
        struct sanei_sp1130n_post_result result = { 1 };
        assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL,
                                         &result) == SANE_STATUS_GOOD);
        assert_same (&page, &before, NULL);
        assert (result.blank == (expected == SANE_STATUS_NO_DOCS));
        free (page.data);
      }

  struct sanei_sp1130n_page page = solid_page (1000, 1000, 255), before = page;
  memset (page.data + page.size - 3, 0, 3);
  struct sanei_sp1130n_post_options options = defaults;
  options.blank_per_mille = 0;
  struct sanei_sp1130n_post_result result;
  assert (sanei_sp1130n_postprocess (&page, &options, 50, NULL, &result) == SANE_STATUS_GOOD);
  assert (!result.blank);
  assert_same (&page, &before, NULL);
  free (page.data);

  /* Chromatic pixels contribute channel darkness, not a private luma cutoff. */
  page = solid_page (20, 20, 0);
  for (size_t i = 0; i < page.size; i += 3)
    page.data[i] = 255;
  options.blank_per_mille = 500;
  assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL, &result) == SANE_STATUS_GOOD);
  assert (!result.blank);
  options.blank_per_mille = 700;
  assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL, &result) == SANE_STATUS_GOOD);
  assert (result.blank);
  free (page.data);
}

static void
no_edges_and_blank_geometry (void)
{
  const unsigned dimensions[][2] = { {1, 1}, {2, 2}, {31, 27}, {320, 240} };
  for (size_t i = 0; i < sizeof dimensions / sizeof dimensions[0]; ++i)
    for (unsigned sample = 128; sample <= 255; sample += 127)
      {
        struct sanei_sp1130n_page page =
          solid_page (dimensions[i][0], dimensions[i][1], (uint8_t) sample);
        struct sanei_sp1130n_page original = copy_page (&page), before = page;
        struct sanei_sp1130n_post_options options = defaults;
        options.autocrop = options.deskew = 1;
        options.crop_margin = 0;
        struct sanei_sp1130n_post_result result;
        assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL,
                                         &result) == SANE_STATUS_GOOD);
        assert_same (&page, &before, NULL);
        assert_equal (&page, &original);
        assert (result.blank == (sample == 255));
        free (original.data);
        free (page.data);
      }
}

static void
crop_shared_edges (void)
{
  struct sanei_sp1130n_page fixture = media_page (0);
  const unsigned margins[] = { 0, 1, 8, 10000, UINT_MAX };
  for (size_t i = 0; i < sizeof margins / sizeof margins[0]; ++i)
    {
      struct sanei_sp1130n_page expected = copy_page (&fixture);
      SANE_Parameters params = page_parameters (&expected);
      int top, bottom, left, right;
      assert (sanei_magic_findEdges (&params, expected.data, 300, 300,
                                    &top, &bottom, &left, &right) ==
              SANE_STATUS_GOOD);
      unsigned margin = margins[i];
      top = margin < (unsigned) top ? top - (int) margin : 0;
      left = margin < (unsigned) left ? left - (int) margin : 0;
      ++bottom;
      ++right;
      bottom = margin < (unsigned) (params.lines - bottom) ?
        bottom + (int) margin : params.lines;
      right = margin < (unsigned) (params.pixels_per_line - right) ?
        right + (int) margin : params.pixels_per_line;
      assert (sanei_magic_crop (&params, expected.data, top, bottom, left,
                               right) == SANE_STATUS_GOOD);
      expected.width = (unsigned) params.pixels_per_line;
      expected.height = (unsigned) params.lines;
      expected.size = (size_t) params.bytes_per_line * expected.height;
      struct sanei_sp1130n_page page = copy_page (&fixture);
      struct sanei_sp1130n_post_options options = defaults;
      options.autocrop = 1;
      options.crop_margin = margin;
      struct sanei_sp1130n_post_result result;
      assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL,
                                       &result) == SANE_STATUS_GOOD);
      assert_equal (&page, &expected);
      assert (!result.blank);
      if (!margin)
        assert (page.width < fixture.width && page.height < fixture.height);
      free (page.data);
      free (expected.data);
    }
  free (fixture.data);
}

static void
blank_after_crop (void)
{
  struct sanei_sp1130n_page page = media_page (0);
  for (size_t i = 0; i < page.size; i += 3)
    if (page.data[i])
      memset (page.data + i, 255, 3);
  struct sanei_sp1130n_post_options options = defaults;
  struct sanei_sp1130n_post_result result;
  assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL, &result) ==
          SANE_STATUS_GOOD);
  assert (!result.blank);       /* The dark scanner border is still present. */
  options.autocrop = 1;
  options.crop_margin = 0;
  assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL, &result) ==
          SANE_STATUS_GOOD);
  assert (result.blank && page.width < 640 && page.height < 480);
  for (size_t i = 0; i < page.size; ++i)
    assert (page.data[i] == 255);
  free (page.data);
}

static void
deskew_shared_media (void)
{
  const unsigned resolutions[] = { 50, 200, 300, 400, 600 };
  const int angles[] = { -30, 30, 120 };
  for (size_t d = 0; d < sizeof resolutions / sizeof resolutions[0]; ++d)
    for (size_t a = 0; a < sizeof angles / sizeof angles[0]; ++a)
      {
        struct sanei_sp1130n_page page = media_page (angles[a]);
        struct sanei_sp1130n_page expected = copy_page (&page), before = page;
        SANE_Parameters params = page_parameters (&expected);
        int cx = 0, cy = 0;
        double slope = 0;
        unsigned dpi = resolutions[d];
        SANE_Status status = sanei_magic_findSkew (&params, expected.data,
                                                  (int) dpi, (int) dpi,
                                                  &cx, &cy, &slope);
        assert (status == SANE_STATUS_GOOD ||
                status == SANE_STATUS_UNSUPPORTED);
        int rotate = status == SANE_STATUS_GOOD && isfinite (slope) &&
          fabs (atan (slope)) <= 5 * (3.14159265358979323846 / 180);
        if (rotate)
          assert (sanei_magic_rotate (&params, expected.data, cx, cy, slope,
                                     255) == SANE_STATUS_GOOD);
        struct sanei_sp1130n_post_options options = defaults;
        options.deskew = 1;
        struct sanei_sp1130n_post_result result;
        assert (sanei_sp1130n_postprocess (&page, &options, dpi, NULL,
                                         &result) == SANE_STATUS_GOOD);
        assert_equal (&page, &expected);
        assert (!result.blank);
        if (dpi == 300 && abs (angles[a]) == 30)
          assert (rotate && page.data != before.data);
        if (abs (angles[a]) == 120)
          assert (!rotate && page.data == before.data);
        free (expected.data);
        free (page.data);
      }
}

static void
invalid_and_cancelled (void)
{
  assert (sanei_sp1130n_post_options_validate (NULL) == SANE_STATUS_INVAL);
  const struct sanei_sp1130n_post_options invalid_options[] = {
    {.autocrop = -1}, {.autocrop = 2}, {.deskew = -1}, {.deskew = 2},
    {.blank_per_mille = 1001}, {.blank_per_mille = UINT_MAX}
  };
  struct sanei_sp1130n_page page = solid_page (2, 2, 128), original = page;
  uint8_t bytes[12];
  memcpy (bytes, page.data, sizeof bytes);
  struct sanei_sp1130n_post_result result;
  for (size_t i = 0; i < sizeof invalid_options / sizeof invalid_options[0]; ++i)
    {
      assert (sanei_sp1130n_post_options_validate (&invalid_options[i]) ==
              SANE_STATUS_INVAL);
      result.blank = 1;
      assert (sanei_sp1130n_postprocess (&page, &invalid_options[i], 300, NULL,
                                       &result) == SANE_STATUS_INVAL);
      assert_same (&page, &original, bytes);
      assert (!result.blank);
    }
  assert (sanei_sp1130n_postprocess (NULL, &defaults, 300, NULL, &result) ==
          SANE_STATUS_INVAL);
  assert (sanei_sp1130n_postprocess (&page, NULL, 300, NULL, &result) ==
          SANE_STATUS_INVAL);
  assert (sanei_sp1130n_postprocess (&page, &defaults, 300, NULL, NULL) ==
          SANE_STATUS_INVAL);
  const unsigned invalid_dpi[] = { 0, 49, 601, 1200, UINT_MAX };
  for (size_t i = 0; i < sizeof invalid_dpi / sizeof invalid_dpi[0]; ++i)
    {
      assert (sanei_sp1130n_postprocess (&page, &defaults, invalid_dpi[i], NULL,
                                       &result) == SANE_STATUS_INVAL);
      assert_same (&page, &original, bytes);
    }
  const unsigned dimensions[][2] = {
    {0, 2}, {2, 0}, {10001, 1}, {1, 20001}, {10000, 20000},
    {UINT_MAX, UINT_MAX}
  };
  for (size_t i = 0; i < sizeof dimensions / sizeof dimensions[0]; ++i)
    {
      page = original;
      page.width = dimensions[i][0];
      page.height = dimensions[i][1];
      page.size = SP_PAGE_LIMIT;
      struct sanei_sp1130n_page before = page;
      assert (sanei_sp1130n_postprocess (&page, &defaults, 300, NULL,
                                       &result) == SANE_STATUS_INVAL);
      assert_same (&page, &before, bytes);
    }
  const size_t sizes[] = { 0, 11, 13, SP_PAGE_LIMIT + 1, SIZE_MAX };
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i)
    {
      page = original;
      page.size = sizes[i];
      struct sanei_sp1130n_page before = page;
      assert (sanei_sp1130n_postprocess (&page, &defaults, 300, NULL,
                                       &result) == SANE_STATUS_INVAL);
      assert_same (&page, &before, bytes);
    }
  page = original;
  page.data = NULL;
  assert (sanei_sp1130n_postprocess (&page, &defaults, 300, NULL, &result) ==
          SANE_STATUS_INVAL);
  page = original;
  atomic_bool cancelled = ATOMIC_VAR_INIT (1);
  result.blank = 1;
  assert (sanei_sp1130n_postprocess (&page, &defaults, 300, &cancelled,
                                   &result) == SANE_STATUS_CANCELLED);
  assert_same (&page, &original, bytes);
  assert (!result.blank);
  free (original.data);
}

#ifdef SP_POST_TEST_ALLOC_FAILURE
static void
allocation_failure_and_cancel (void)
{
  struct sanei_sp1130n_page fixture = media_page (30);
  struct sanei_sp1130n_post_options options = defaults;
  options.autocrop = options.deskew = 1;
  struct sanei_sp1130n_page probe = copy_page (&fixture);
  struct sanei_sp1130n_post_result result;
  arm_allocations (0, 0, NULL);
  assert (sanei_sp1130n_postprocess (&probe, &options, 300, NULL, &result) ==
          SANE_STATUS_GOOD);
  unsigned allocations = allocation_number;
  assert (allocations > 12);
  arm_allocations (0, 0, NULL);
  free (probe.data);
  /* Cover early getLine allocations, its row array, rotation and final crop. */
  const unsigned failures[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 12,
    allocations - 5, allocations - 4, allocations - 3,
    allocations - 2, allocations - 1, allocations
  };
  for (size_t i = 0; i < sizeof failures / sizeof failures[0]; ++i)
    {
      struct sanei_sp1130n_page page = copy_page (&fixture), before = page;
      arm_allocations (failures[i], 0, NULL);
      result.blank = 1;
      assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL,
                                       &result) == SANE_STATUS_NO_MEM);
      arm_allocations (0, 0, NULL);
      assert_same (&page, &before, NULL);
      assert_equal (&page, &fixture);
      assert (!result.blank);
      assert (sanei_sp1130n_postprocess (&page, &options, 300, NULL,
                                       &result) == SANE_STATUS_GOOD);
      free (page.data);
    }
  const unsigned cancellations[] = { 1, 2, 4, allocations - 2, allocations };
  for (size_t i = 0;
       i < sizeof cancellations / sizeof cancellations[0]; ++i)
    {
      struct sanei_sp1130n_page page = copy_page (&fixture), before = page;
      atomic_bool cancelled = ATOMIC_VAR_INIT (0);
      arm_allocations (0, cancellations[i], &cancelled);
      result.blank = 1;
      assert (sanei_sp1130n_postprocess (&page, &options, 300, &cancelled,
                                       &result) == SANE_STATUS_CANCELLED);
      arm_allocations (0, 0, NULL);
      assert_same (&page, &before, NULL);
      assert_equal (&page, &fixture);
      assert (!result.blank);
      free (page.data);
    }
  free (fixture.data);
}
#endif

int
main (void)
{
  sanei_magic_init ();
  blank_and_identity ();
  no_edges_and_blank_geometry ();
  crop_shared_edges ();
  blank_after_crop ();
  deskew_shared_media ();
  invalid_and_cancelled ();
#ifdef SP_POST_TEST_ALLOC_FAILURE
  allocation_failure_and_cancel ();
#endif
  puts ("SANE shared crop/deskew/blank tests passed; geometry, bounds, identity,");
  puts ("density semantics, helper parity, cancellation and failure preservation.");
  return 0;
}
