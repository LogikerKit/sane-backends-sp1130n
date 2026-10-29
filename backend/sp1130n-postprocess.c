/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#include "../include/sane/config.h"
#include "sp1130n-postprocess.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SKEW_WIDTH 640u
#define SKEW_HEIGHT 1024u
#define SKEW_ROWS (SKEW_HEIGHT + SKEW_WIDTH + 8u)

struct content
{
  uint64_t dark, strong, colored;
  unsigned left, top, right, bottom;
};
struct point
{
  uint16_t x, y;
};

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
      options->content_threshold > 255 || options->blank_per_mille > 1000)
    return SP_INVALID;
  /* Even UINT_MAX is a safe margin: it simply retains the whole page. */
  return SP_OK;
}

static unsigned
luma (const uint8_t *pixel)
{
  return (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2] + 128u) >> 8;
}

static int
rgb_size (const struct sanei_sp1130n_page *page, size_t *size)
{
  if (!page || !page->data || !page->width || !page->height ||
      page->width > 10000 || page->height > 20000)
    return SP_INVALID;
  size_t stride = (size_t) page->width;
  if (stride > SIZE_MAX / 3)
    return SP_INVALID;
  stride *= 3;
  if (stride > SIZE_MAX / page->height)
    return SP_INVALID;
  *size = stride * page->height;
  return *size <= SP_PAGE_LIMIT && page->size == *size ? SP_OK : SP_INVALID;
}

static int
inspect (const struct sanei_sp1130n_page *page, unsigned threshold,
         const atomic_bool *cancelled, struct content *content)
{
  *content = (struct content)
  {.left = page->width,.top = page->height };
  for (unsigned y = 0; y < page->height; ++y)
    {
      if (is_cancelled (cancelled))
        return SP_CANCELLED;
      const uint8_t *pixel = page->data + (size_t) y * page->width * 3;
      for (unsigned x = 0; x < page->width; ++x, pixel += 3)
        {
          unsigned value = luma (pixel);
          if (value >= threshold)
            continue;
          unsigned low = pixel[0], high = pixel[0];
          for (unsigned channel = 1; channel < 3; ++channel)
            {
              if (pixel[channel] < low)
                low = pixel[channel];
              if (pixel[channel] > high)
                high = pixel[channel];
            }
          ++content->dark;
          content->strong += value < 128;
          content->colored += high - low > 24;
          if (x < content->left)
            content->left = x;
          if (y < content->top)
            content->top = y;
          if (x > content->right)
            content->right = x;
          if (y > content->bottom)
            content->bottom = y;
        }
    }
  return is_cancelled (cancelled) ? SP_CANCELLED : SP_OK;
}

/*
 * Eight-connected glyph-like components are only a conservative eligibility
 * test, not OCR. Counts storage is reused as the bounded flood-fill queue.
 */
static int
text_components (uint8_t *mask, uint32_t *queue, unsigned width,
                 unsigned height, size_t points,
                 const atomic_bool *cancelled, int *eligible)
{
  size_t glyph_pixels = 0, glyphs = 0, largest = 0;
  size_t cells = (size_t) width * height;
  *eligible = 0;
  for (size_t start = 0; start < cells; ++start)
    {
      if (!(start % 4096) && is_cancelled (cancelled))
        return SP_CANCELLED;
      if (mask[start] != 1)
        continue;
      size_t head = 0, tail = 1;
      unsigned left = (unsigned) (start % width), right = left;
      unsigned top = (unsigned) (start / width), bottom = top;
      queue[0] = (uint32_t) start;
      mask[start] = 2;
      while (head < tail)
        {
          if (!(head % 1024) && is_cancelled (cancelled))
            return SP_CANCELLED;
          size_t index = queue[head++];
          unsigned x = (unsigned) (index % width);
          unsigned y = (unsigned) (index / width);
          if (x < left)
            left = x;
          if (x > right)
            right = x;
          if (y < top)
            top = y;
          if (y > bottom)
            bottom = y;
          unsigned y0 = y ? y - 1 : y;
          unsigned y1 = y + 1 < height ? y + 1 : y;
          unsigned x0 = x ? x - 1 : x;
          unsigned x1 = x + 1 < width ? x + 1 : x;
          for (unsigned ny = y0; ny <= y1; ++ny)
            {
              for (unsigned nx = x0; nx <= x1; ++nx)
                {
                  size_t neighbor = (size_t) ny * width + nx;
                  if (mask[neighbor] != 1)
                    continue;
                  mask[neighbor] = 2;
                  queue[tail++] = (uint32_t) neighbor;
                }
            }
        }
      if (tail > largest)
        largest = tail;
      unsigned cw = right - left + 1, ch = bottom - top + 1;
      size_t area = (size_t) cw * ch;
      if (cw >= 2 && ch >= 3 && cw <= width / 6 + 2 &&
          ch <= height / 12 + 2 && tail >= 4 &&
          tail * 100 >= area * 15 && tail * 100 <= area * 90)
        {
          ++glyphs;
          glyph_pixels += tail;
        }
    }
  *eligible = glyphs >= 12 && glyph_pixels * 2 >= points &&
    largest * 3 <= points;
  return is_cancelled (cancelled) ? SP_CANCELLED : SP_OK;
}

static int
projection (const struct point *points, size_t count,
            unsigned width, unsigned height, int tenths,
            const atomic_bool *cancelled, uint32_t histogram[SKEW_ROWS],
            uint64_t *score, unsigned *rows)
{
  unsigned padding = width / 16 + 4;
  *rows = height + 2 * padding;
  memset (histogram, 0, *rows * sizeof *histogram);
  double slope = tan (tenths * (3.14159265358979323846 / 1800.0));
  double center = ((double) width - 1) / 2;
  for (size_t i = 0; i < count; ++i)
    {
      if (!(i % 4096) && is_cancelled (cancelled))
        return SP_CANCELLED;
      double row = points[i].y - slope * (points[i].x - center) + padding;
      unsigned bin = (unsigned) floor (row + 0.5);
      /* Padding bounds every +/-5 degree projection on this thumbnail. */
      if (bin >= *rows)
        return SP_INVALID;
      ++histogram[bin];
    }
  *score = 0;
  for (unsigned y = 0; y < *rows; ++y)
    *score += (uint64_t) histogram[y] * histogram[y];
  return is_cancelled (cancelled) ? SP_CANCELLED : SP_OK;
}

static int
text_bands (const uint32_t *histogram, unsigned rows, unsigned width,
            unsigned height, size_t points)
{
  unsigned peak = 0;
  for (unsigned y = 0; y < rows; ++y)
    if (histogram[y] > peak)
      peak = histogram[y];
  if (peak < width / 12 || peak < 8)
    return 0;
  unsigned threshold = peak / 8;
  if (threshold < 4)
    threshold = 4;
  unsigned bands = 0;
  size_t covered = 0;
  for (unsigned y = 0; y < rows;)
    {
      if (histogram[y] < threshold)
        {
          ++y;
          continue;
        }
      unsigned first = y, last = y, local_peak = 0;
      size_t ink = 0;
      for (; y < rows && histogram[y] >= threshold; ++y)
        {
          if (histogram[y] > local_peak)
            local_peak = histogram[y];
          ink += histogram[y];
          last = y;
        }
      if (last - first + 1 <= height / 12 + 2 && local_peak >= width / 12)
        {
          ++bands;
          covered += ink;
        }
    }
  return bands >= 3 && bands <= 80 && covered * 5 >= points * 4;
}

static int
estimate_skew (const struct sanei_sp1130n_page *page,
               const struct sanei_sp1130n_post_options *options,
               const struct content *content, const atomic_bool *cancelled,
               int *rotation)
{
  uint64_t pixels = (uint64_t) page->width * page->height;
  *rotation = 0;
  if (page->width < 96 || page->height < 64 || pixels < 4096 ||
      content->dark * 200 < pixels || content->dark * 4 > pixels ||
      content->strong * 5 < content->dark * 4 ||
      content->colored * 10 > content->dark)
    return SP_OK;

  unsigned step_x = page->width / SKEW_WIDTH + (page->width % SKEW_WIDTH != 0);
  unsigned step_y =
    page->height / SKEW_HEIGHT + (page->height % SKEW_HEIGHT != 0);
  unsigned step = step_x > step_y ? step_x : step_y;
  unsigned width = page->width / step + (page->width % step != 0);
  unsigned height = page->height / step + (page->height % step != 0);
  size_t cells = (size_t) width * height;
  uint32_t *counts = malloc (cells * sizeof *counts);
  uint8_t *mask = malloc (cells);
  struct point *points = malloc (cells * sizeof *points);
  int status = SP_OK;
  if (!counts || !mask || !points)
    {
      status = SP_NOMEM;
      goto done;
    }
  memset (counts, 0, cells * sizeof *counts);
  for (unsigned y = 0; y < page->height; ++y)
    {
      if (is_cancelled (cancelled))
        {
          status = SP_CANCELLED;
          goto done;
        }
      const uint8_t *pixel = page->data + (size_t) y * page->width * 3;
      size_t row = (size_t) (y / step) * width;
      for (unsigned x = 0; x < page->width; ++x, pixel += 3)
        if (luma (pixel) < options->content_threshold)
          ++counts[row + x / step];
    }
  size_t count = 0;
  for (unsigned y = 0; y < height; ++y)
    {
      if (is_cancelled (cancelled))
        {
          status = SP_CANCELLED;
          goto done;
        }
      unsigned block_height = page->height - y * step;
      if (block_height > step)
        block_height = step;
      for (unsigned x = 0; x < width; ++x)
        {
          unsigned block_width = page->width - x * step;
          if (block_width > step)
            block_width = step;
          size_t index = (size_t) y * width + x;
          mask[index] = counts[index] * 4 >= block_width * block_height;
          if (mask[index])
            points[count++] = (struct point)
            { (uint16_t) x, (uint16_t) y };
        }
    }
  if (count < 100 || count * 200 < cells || count * 3 > cells)
    goto done;
  int eligible;
  status =
    text_components (mask, counts, width, height, count, cancelled, &eligible);
  if (status != SP_OK || !eligible)
    goto done;

  uint32_t histogram[SKEW_ROWS];
  unsigned rows;
  uint64_t scores[21], best_score, zero_score;
  status = projection (points, count, width, height, 0, cancelled,
                       histogram, &zero_score, &rows);
  if (status != SP_OK)
    goto done;
  best_score = zero_score;
  int best = 0;
  /* At most 21 coarse and 9 fine projections, never full-resolution sweeps. */
  for (int angle = -50; angle <= 50; angle += 5)
    {
      uint64_t score;
      status = projection (points, count, width, height, angle, cancelled,
                           histogram, &score, &rows);
      if (status != SP_OK)
        goto done;
      scores[(angle + 50) / 5] = score;
      if (score > best_score
          || (score == best_score && abs (angle) < abs (best)))
        {
          best_score = score;
          best = angle;
        }
    }
  int coarse = best;
  for (int angle = coarse - 4; angle <= coarse + 4; ++angle)
    {
      if (angle < -50 || angle > 50)
        continue;
      uint64_t score;
      status = projection (points, count, width, height, angle, cancelled,
                           histogram, &score, &rows);
      if (status != SP_OK)
        goto done;
      if (score > best_score
          || (score == best_score && abs (angle) < abs (best)))
        {
          best_score = score;
          best = angle;
        }
    }
  /* Boundary maxima might be a larger, unsupported skew; leave them alone. */
  if (abs (best) < 3 || abs (best) >= 49 || best_score * 100 < zero_score * 115)
    goto done;
  uint64_t competitor = 0;
  for (int angle = -50; angle <= 50; angle += 5)
    if (abs (angle - best) >= 10 && scores[(angle + 50) / 5] > competitor)
      competitor = scores[(angle + 50) / 5];
  if (best_score * 100 < competitor * 110)
    goto done;
  status = projection (points, count, width, height, best, cancelled,
                       histogram, &best_score, &rows);
  if (status != SP_OK)
    goto done;
  if (best_score * rows * 2 < (uint64_t) count * count * 5 ||
      !text_bands (histogram, rows, width, height, count))
    goto done;
  *rotation = -best;
done:
  free (points);
  free (mask);
  free (counts);
  return is_cancelled (cancelled) ? SP_CANCELLED : status;
}

static int
rotate (const struct sanei_sp1130n_page *input, int tenths,
        const atomic_bool *cancelled, struct sanei_sp1130n_page *output)
{
  uint8_t *data = malloc (input->size);
  if (!data)
    return SP_NOMEM;
  double angle = tenths * (3.14159265358979323846 / 1800.0);
  double cosine = cos (angle), sine = sin (angle);
  double cx = ((double) input->width - 1) / 2;
  double cy = ((double) input->height - 1) / 2;
  for (unsigned y = 0; y < input->height; ++y)
    {
      if (is_cancelled (cancelled))
        {
          free (data);
          return SP_CANCELLED;
        }
      double sx = -cosine * cx + sine * ((double) y - cy) + cx;
      double sy = sine * cx + cosine * ((double) y - cy) + cy;
      uint8_t *pixel = data + (size_t) y * input->width * 3;
      for (unsigned x = 0; x < input->width;
           ++x, pixel += 3, sx += cosine, sy -= sine)
        {
          if (sx < 0 || sy < 0 || sx > input->width - 1
              || sy > input->height - 1)
            {
              memset (pixel, 255, 3);
              continue;
            }
          unsigned x0 = (unsigned) sx, y0 = (unsigned) sy;
          unsigned x1 = x0 + 1 < input->width ? x0 + 1 : x0;
          unsigned y1 = y0 + 1 < input->height ? y0 + 1 : y0;
          double fx = sx - x0, fy = sy - y0;
          const uint8_t *p00 =
            input->data + ((size_t) y0 * input->width + x0) * 3;
          const uint8_t *p10 =
            input->data + ((size_t) y0 * input->width + x1) * 3;
          const uint8_t *p01 =
            input->data + ((size_t) y1 * input->width + x0) * 3;
          const uint8_t *p11 =
            input->data + ((size_t) y1 * input->width + x1) * 3;
          for (unsigned channel = 0; channel < 3; ++channel)
            {
              double upper = p00[channel] * (1 - fx) + p10[channel] * fx;
              double lower = p01[channel] * (1 - fx) + p11[channel] * fx;
              pixel[channel] = (uint8_t) (upper * (1 - fy) + lower * fy + 0.5);
            }
        }
    }
  *output = (struct sanei_sp1130n_page)
  { data, input->size, input->width, input->height };
  return SP_OK;
}

static int
crop (const struct sanei_sp1130n_page *input, const struct content *content,
      unsigned margin, const atomic_bool *cancelled,
      struct sanei_sp1130n_page *output)
{
  unsigned left = content->left > margin ? content->left - margin : 0;
  unsigned top = content->top > margin ? content->top - margin : 0;
  unsigned right = margin >= input->width - 1 - content->right ?
    input->width - 1 : content->right + margin;
  unsigned bottom = margin >= input->height - 1 - content->bottom ?
    input->height - 1 : content->bottom + margin;
  unsigned width = right - left + 1, height = bottom - top + 1;
  if (width == input->width && height == input->height)
    return SP_OK;
  size_t stride = (size_t) width * 3, size = stride * height;
  uint8_t *data = malloc (size);
  if (!data)
    return SP_NOMEM;
  for (unsigned y = 0; y < height; ++y)
    {
      if (is_cancelled (cancelled))
        {
          free (data);
          return SP_CANCELLED;
        }
      const uint8_t *row =
        input->data + ((size_t) (top + y) * input->width + left) * 3;
      memcpy (data + (size_t) y * stride, row, stride);
    }
  *output = (struct sanei_sp1130n_page)
  { data, size, width, height };
  return SP_OK;
}

int
sanei_sp1130n_postprocess (struct sanei_sp1130n_page *page,
                           const struct sanei_sp1130n_post_options *options,
                           const atomic_bool *cancelled,
                           struct sanei_sp1130n_post_result *result)
{
  if (result)
    *result = (struct sanei_sp1130n_post_result)
    { 0 };
  size_t size;
  if (!result || sanei_sp1130n_post_options_validate (options) != SP_OK ||
      rgb_size (page, &size) != SP_OK)
    return SP_INVALID;
  if (is_cancelled (cancelled))
    return SP_CANCELLED;
  struct content content;
  int status = inspect (page, options->content_threshold, cancelled, &content);
  if (status != SP_OK)
    return status;
  uint64_t pixels = (uint64_t) page->width * page->height;
  struct sanei_sp1130n_post_result answer = {
    .blank =
      content.dark * 1000 <= (uint64_t) options->blank_per_mille * pixels,
    .dark_per_mille = (unsigned) ((content.dark * 1000 + pixels / 2) / pixels)
  };
  struct sanei_sp1130n_page working = *page;
  if (options->deskew && !answer.blank)
    {
      status =
        estimate_skew (page, options, &content, cancelled,
                       &answer.deskew_tenths);
      if (status != SP_OK)
        goto done;
      if (answer.deskew_tenths)
        {
          status = rotate (page, answer.deskew_tenths, cancelled, &working);
          if (status != SP_OK)
            goto done;
          if (options->autocrop)
            {
              status =
                inspect (&working, options->content_threshold, cancelled,
                         &content);
              if (status != SP_OK)
                goto done;
            }
        }
    }
  if (options->autocrop && !answer.blank && content.dark)
    {
      struct sanei_sp1130n_page cropped = working;
      status =
        crop (&working, &content, options->crop_margin, cancelled, &cropped);
      if (status != SP_OK)
        goto done;
      if (cropped.data != working.data)
        {
          if (working.data != page->data)
            free (working.data);
          working = cropped;
        }
    }
  if (is_cancelled (cancelled))
    {
      status = SP_CANCELLED;
      goto done;
    }
  if (working.data != page->data)
    {
      free (page->data);
      *page = working;
    }
  *result = answer;
  return SP_OK;
done:
  if (working.data != page->data)
    free (working.data);
  return status;
}
