/* sane - Scanner Access Now Easy.
   This file implements a SANE backend for V4L2 video devices.

   This file is part of the SANE package.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3 of the License, or (at your
   option) any later version.
 */

#define BUILD 6

#include "../include/sane/config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <libv4l2.h>

#define BACKEND_NAME v4l
#include "../include/sane/sanei_backend.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/saneopts.h"
#include "v4l.h"

#define V4L_CONFIG_FILE "v4l.conf"
#define V4L_CAPTURE_TIMEOUT_MS 5000
#define V4L_PROBE_TIMEOUT_MS 3000

static const SANE_Device **devlist;
static V4L_Device *first_dev;
static V4L_Scanner *first_handle;
static int num_devices;

void sane_cancel (SANE_Handle handle);

static const SANE_String_Const mode_list[] = {
  SANE_VALUE_SCAN_MODE_GRAY, SANE_VALUE_SCAN_MODE_COLOR, NULL
};

static const SANE_Range u8_range = { 0, 255, 0 };

static void
free_channels (V4L_Scanner *s)
{
  int i;
  for (i = 0; i < MAX_CHANNELS; i++)
    {
      free ((void *) s->channel[i]);
      s->channel[i] = NULL;
    }
}

static V4L_Scanner *
find_handle (SANE_Handle handle)
{
  V4L_Scanner *s;
  for (s = first_handle; s; s = s->next)
    if (s == handle)
      return s;
  return NULL;
}

static int
xioctl (int fd, unsigned long request, void *arg)
{
  int result;
  do
    result = v4l2_ioctl (fd, request, arg);
  while (result == -1 && errno == EINTR);
  return result;
}

static void
debug_format (int level, const char *label, const struct v4l2_pix_format *format)
{
  DBG (level, "%s: %c%c%c%c %ux%u, bytesperline=%u, sizeimage=%u\n",
       label,
       format->pixelformat & 0xff,
       (format->pixelformat >> 8) & 0xff,
       (format->pixelformat >> 16) & 0xff,
       (format->pixelformat >> 24) & 0xff,
       format->width, format->height, format->bytesperline,
       format->sizeimage);
}

static SANE_Status
wait_for_frame (int fd, int timeout_ms)
{
  struct pollfd poll_fd;
  int result;

  poll_fd.fd = fd;
  poll_fd.events = POLLIN;
  poll_fd.revents = 0;
  do
    result = poll (&poll_fd, 1, timeout_ms);
  while (result == -1 && errno == EINTR);
  if (result <= 0
      || !(poll_fd.revents & POLLIN)
      || (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    return SANE_STATUS_IO_ERROR;
  return SANE_STATUS_GOOD;
}

static SANE_Status
dequeue_frame (int fd, struct v4l2_buffer *buf, int timeout_ms)
{
  struct timespec start;
  struct timespec now;

  if (clock_gettime (CLOCK_MONOTONIC, &start) == -1)
    return SANE_STATUS_IO_ERROR;

  while (1)
    {
      int elapsed;
      int wait_ms;

      if (clock_gettime (CLOCK_MONOTONIC, &now) == -1)
        return SANE_STATUS_IO_ERROR;
      elapsed = (int) ((now.tv_sec - start.tv_sec) * 1000
                       + (now.tv_nsec - start.tv_nsec) / 1000000);
      if (elapsed >= timeout_ms)
        break;
      wait_ms = MIN (250, timeout_ms - elapsed);

      if (wait_for_frame (fd, wait_ms) == SANE_STATUS_GOOD)
        {
          if (xioctl (fd, VIDIOC_DQBUF, buf) == 0)
            return SANE_STATUS_GOOD;

          if (errno != EAGAIN)
            return SANE_STATUS_IO_ERROR;

          DBG (4, "mmap capture: VIDIOC_DQBUF returned EAGAIN, retrying\n");
        }
    }

  DBG (3, "mmap capture: timed out waiting for a dequeued frame\n");
  return SANE_STATUS_IO_ERROR;
}

static ssize_t
read_frame (int fd, void *buffer, size_t size, int timeout_ms)
{
  int elapsed = 0;

  while (elapsed < timeout_ms)
    {
      if (wait_for_frame (fd, 250) == SANE_STATUS_GOOD)
        {
          ssize_t result = v4l2_read (fd, buffer, size);
          if (result > 0)
            return result;

          if (result == -1 && errno != EAGAIN)
            return -1;

          DBG (4, "capture read path: v4l2_read returned EAGAIN, retrying\n");
        }

      elapsed += 250;
    }

  DBG (3, "capture read path: timed out waiting for a readable frame\n");
  errno = ETIMEDOUT;
  return -1;
}

static SANE_Status
query_capability (int fd, struct v4l2_capability *cap)
{
  __u32 capabilities;
  if (xioctl (fd, VIDIOC_QUERYCAP, cap) == -1)
    return SANE_STATUS_INVAL;
  capabilities = (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
    ? cap->device_caps : cap->capabilities;
  if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE)
      || !(capabilities & (V4L2_CAP_READWRITE | V4L2_CAP_STREAMING)))
    return SANE_STATUS_UNSUPPORTED;
  return SANE_STATUS_GOOD;
}

static SANE_Bool
has_supported_format (int fd)
{
  struct v4l2_fmtdesc format;
  unsigned int index;

  for (index = 0; index < 256; index++)
    {
      memset (&format, 0, sizeof (format));
      format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      format.index = index;
      if (xioctl (fd, VIDIOC_ENUM_FMT, &format) == -1)
        break;
      if (format.pixelformat == V4L2_PIX_FMT_MJPEG
          || format.pixelformat == V4L2_PIX_FMT_YUYV
          || format.pixelformat == V4L2_PIX_FMT_RGB24
          || format.pixelformat == V4L2_PIX_FMT_GREY)
        return SANE_TRUE;
    }
  return SANE_FALSE;
}

static SANE_Bool
probe_capture (int fd)
{
  struct v4l2_capability capability;
  struct v4l2_format format;
  struct v4l2_requestbuffers request;
  struct v4l2_buffer buffer;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  void *mapping = MAP_FAILED;
  size_t size;
  ssize_t result;
  SANE_Bool streaming = SANE_FALSE;
  SANE_Bool mmap_capture = SANE_FALSE;
  SANE_Bool success = SANE_FALSE;
  __u32 capabilities;

  memset (&capability, 0, sizeof (capability));
  if (query_capability (fd, &capability) != SANE_STATUS_GOOD)
    return SANE_FALSE;
  capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
    ? capability.device_caps : capability.capabilities;
  memset (&format, 0, sizeof (format));
  format.type = type;
  if (xioctl (fd, VIDIOC_G_FMT, &format) == -1)
    return SANE_FALSE;
  debug_format (4, "probe format", &format.fmt.pix);

  if (capabilities & V4L2_CAP_READWRITE)
    {
      DBG (4, "probe: trying read capture\n");
      size = format.fmt.pix.sizeimage;
      if (!size)
        size = (size_t) format.fmt.pix.width * format.fmt.pix.height * 4;
      mapping = malloc (size);
      if (mapping == NULL)
        return SANE_FALSE;
      if (wait_for_frame (fd, V4L_PROBE_TIMEOUT_MS) == SANE_STATUS_GOOD)
        {
          result = v4l2_read (fd, mapping, size);
          success = result > 0;
          DBG (4, "probe: read returned %zd bytes\n", result);
        }
      else
        DBG (3, "probe: read frame timed out: %s\n",
             strerror (errno));
      if (success)
        goto cleanup;
      free (mapping);
      mapping = MAP_FAILED;
    }

  if (capabilities & V4L2_CAP_STREAMING)
    {
      DBG (4, "probe: trying mmap streaming\n");
      memset (&request, 0, sizeof (request));
      request.count = 1;
      request.type = type;
      request.memory = V4L2_MEMORY_MMAP;
      if (xioctl (fd, VIDIOC_REQBUFS, &request) == -1 || request.count < 1)
        return SANE_FALSE;
      memset (&buffer, 0, sizeof (buffer));
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = 0;
      if (xioctl (fd, VIDIOC_QUERYBUF, &buffer) == -1)
        return SANE_FALSE;
      mapping = v4l2_mmap (NULL, buffer.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, buffer.m.offset);
      if (mapping == MAP_FAILED)
        return SANE_FALSE;
      mmap_capture = SANE_TRUE;
      if (xioctl (fd, VIDIOC_QBUF, &buffer) == -1
          || xioctl (fd, VIDIOC_STREAMON, &type) == -1)
        goto cleanup;
      streaming = SANE_TRUE;
      if (wait_for_frame (fd, V4L_PROBE_TIMEOUT_MS) == SANE_STATUS_GOOD
          && xioctl (fd, VIDIOC_DQBUF, &buffer) == 0)
        success = SANE_TRUE;
      else
        DBG (3, "probe: first mmap frame timed out or dequeue failed: %s\n",
             strerror (errno));
    }

cleanup:
  if (streaming)
    xioctl (fd, VIDIOC_STREAMOFF, &type);
  if (mapping != MAP_FAILED)
    {
      if (mmap_capture)
        v4l2_munmap (mapping, buffer.length);
      else
        free (mapping);
    }
  return success;
}

static SANE_Status
attach (const char *devname)
{
  V4L_Device *dev;
  struct v4l2_capability cap;
  int fd;

  for (dev = first_dev; dev; dev = dev->next)
    if (strcmp (dev->sane.name, devname) == 0)
      return SANE_STATUS_GOOD;

  fd = v4l2_open (devname, O_RDWR | O_NONBLOCK);
  if (fd == -1)
    return SANE_STATUS_INVAL;
  memset (&cap, 0, sizeof (cap));
  if (query_capability (fd, &cap) != SANE_STATUS_GOOD)
    {
      v4l2_close (fd);
      return SANE_STATUS_UNSUPPORTED;
    }
  if (!has_supported_format (fd))
    {
      v4l2_close (fd);
      return SANE_STATUS_UNSUPPORTED;
    }
  if (!probe_capture (fd))
    {
      v4l2_close (fd);
      return SANE_STATUS_UNSUPPORTED;
    }
  v4l2_close (fd);

  dev = calloc (1, sizeof (*dev));
  if (!dev)
    return SANE_STATUS_NO_MEM;
  dev->sane.name = strdup (devname);
  dev->sane.model = strdup ((const char *) cap.card);
  if (!dev->sane.name || !dev->sane.model)
    {
      free ((void *) dev->sane.name);
      free ((void *) dev->sane.model);
      free (dev);
      return SANE_STATUS_NO_MEM;
    }
  dev->sane.vendor = "Video4Linux2";
  dev->sane.type = "video capture device";
  dev->next = first_dev;
  first_dev = dev;
  num_devices++;
  return SANE_STATUS_GOOD;
}

static void
attach_video_devices (void)
{
  char devname[PATH_MAX];
  int index;

  for (index = 0; index < 64; index++)
    {
      snprintf (devname, sizeof (devname), "/dev/video%d", index);
      attach (devname);
    }
}

static void
set_default_format (V4L_Scanner *s)
{
  memset (&s->format, 0, sizeof (s->format));
  s->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (s->fd, VIDIOC_G_FMT, &s->format) == -1)
    {
      s->format.fmt.pix.width = 640;
      s->format.fmt.pix.height = 480;
      s->format.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
      s->format.fmt.pix.field = V4L2_FIELD_ANY;
    }
}

static void
add_frame_size (V4L_Scanner *s, __u32 width, __u32 height)
{
  unsigned i;
  unsigned existing_width;
  unsigned existing_height;
  if (!width || !height || s->frame_size_count >= MAX_FRAME_SIZES)
    return;
  for (i = 0; i < s->frame_size_count; i++)
    if (sscanf (s->frame_size_list[i], "%ux%u",
                &existing_width, &existing_height) == 2
        && existing_width == width && existing_height == height)
      return;
  snprintf (s->frame_size_names[s->frame_size_count],
            sizeof (s->frame_size_names[0]), "%ux%u", width, height);
  s->frame_size_list[s->frame_size_count]
    = s->frame_size_names[s->frame_size_count];
  s->frame_size_count++;
}

static void
query_frame_sizes (V4L_Scanner *s)
{
  struct v4l2_fmtdesc format;
  struct v4l2_frmsizeenum size;
  unsigned format_index;
  unsigned size_index;

  s->frame_size_count = 0;
  for (format_index = 0; format_index < 256; format_index++)
    {
      memset (&format, 0, sizeof (format));
      format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      format.index = format_index;
      if (xioctl (s->fd, VIDIOC_ENUM_FMT, &format) == -1)
        break;
      if (format.pixelformat != V4L2_PIX_FMT_MJPEG
          && format.pixelformat != V4L2_PIX_FMT_YUYV
          && format.pixelformat != V4L2_PIX_FMT_RGB24
          && format.pixelformat != V4L2_PIX_FMT_GREY)
        continue;
      for (size_index = 0; size_index < 256; size_index++)
        {
          memset (&size, 0, sizeof (size));
          size.index = size_index;
          size.pixel_format = format.pixelformat;
          if (xioctl (s->fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1)
            break;
          if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            add_frame_size (s, size.discrete.width, size.discrete.height);
          else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE
                   || size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
            {
              add_frame_size (s, size.stepwise.min_width,
                              size.stepwise.min_height);
              add_frame_size (s, size.stepwise.max_width,
                              size.stepwise.max_height);
              break;
            }
        }
    }
  if (s->frame_size_count == 0)
    add_frame_size (s, s->format.fmt.pix.width, s->format.fmt.pix.height);
  s->frame_size_list[s->frame_size_count] = NULL;
  s->frame_width = s->format.fmt.pix.width;
  s->frame_height = s->format.fmt.pix.height;
  if (s->frame_size_count > 0)
    {
      s->frame_width = 0;
      s->frame_height = 0;
      sscanf (s->frame_size_list[0], "%ux%u",
              &s->frame_width, &s->frame_height);
    }
}

static SANE_Bool
gray_fallback_as_rgb (const V4L_Scanner *s)
{
  return s->gray_mode
    && s->format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24;
}

static SANE_Status
update_parameters (V4L_Scanner *s)
{
  SANE_Word width;
  SANE_Word height;
  if (s->frame_width == 0 || s->frame_height == 0
      || s->frame_width > INT_MAX || s->frame_height > INT_MAX)
    return SANE_STATUS_INVAL;
  if (s->val[OPT_TL_X].w > s->val[OPT_BR_X].w
      || s->val[OPT_TL_Y].w > s->val[OPT_BR_Y].w)
    return SANE_STATUS_INVAL;
  width = SANE_UNFIX (s->val[OPT_BR_X].w - s->val[OPT_TL_X].w);
  height = SANE_UNFIX (s->val[OPT_BR_Y].w - s->val[OPT_TL_Y].w);
  if (width <= 0 || height <= 0)
    return SANE_STATUS_INVAL;
  s->params.format = gray_fallback_as_rgb (s)
    ? SANE_FRAME_RGB : (s->gray_mode ? SANE_FRAME_GRAY : SANE_FRAME_RGB);
  s->params.last_frame = SANE_TRUE;
  s->params.pixels_per_line = width;
  s->params.lines = height;
  s->params.bytes_per_line = s->params.pixels_per_line
    * ((s->params.format == SANE_FRAME_GRAY) ? 1 : 3);
  s->params.depth = 8;
  return SANE_STATUS_GOOD;
}

static int
query_control (const V4L_Scanner *s, unsigned id, SANE_Word *value)
{
  struct v4l2_queryctrl query;
  struct v4l2_control control;
  memset (&query, 0, sizeof (query));
  query.id = id;
  if (xioctl (s->fd, VIDIOC_QUERYCTRL, &query) == -1
      || (query.flags & V4L2_CTRL_FLAG_DISABLED))
    return -1;
  memset (&control, 0, sizeof (control));
  control.id = id;
  if (xioctl (s->fd, VIDIOC_G_CTRL, &control) == -1)
    return -1;
  *value = query.maximum == query.minimum ? 0
    : (control.value - query.minimum) * 255
      / (query.maximum - query.minimum);
  return 0;
}

static void
query_control_option (V4L_Scanner *s, int option, unsigned id)
{
  if (query_control (s, id, &s->val[option].w) != 0)
    {
      s->opt[option].cap |= SANE_CAP_INACTIVE;
      DBG (4, "control option %s is unavailable (V4L2 id=0x%x)\n",
           s->opt[option].name, id);
    }
}

static SANE_Status
set_control (const V4L_Scanner *s, unsigned id, SANE_Word value)
{
  struct v4l2_queryctrl query;
  struct v4l2_control control;
  memset (&query, 0, sizeof (query));
  query.id = id;
  if (xioctl (s->fd, VIDIOC_QUERYCTRL, &query) == -1
      || (query.flags & V4L2_CTRL_FLAG_DISABLED))
    return SANE_STATUS_UNSUPPORTED;
  memset (&control, 0, sizeof (control));
  control.id = id;
  control.value = query.minimum
    + value * (query.maximum - query.minimum) / 255;
  return xioctl (s->fd, VIDIOC_S_CTRL, &control) == -1
    ? SANE_STATUS_INVAL : SANE_STATUS_GOOD;
}

static SANE_Status
init_options (V4L_Scanner *s)
{
  int i;
  memset (s->opt, 0, sizeof (s->opt));
  memset (s->val, 0, sizeof (s->val));
  for (i = 0; i < NUM_OPTIONS; i++)
    {
      s->opt[i].size = sizeof (SANE_Word);
      s->opt[i].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    }

  s->opt[OPT_NUM_OPTS] = (SANE_Option_Descriptor) {
    SANE_NAME_NUM_OPTIONS, SANE_TITLE_NUM_OPTIONS, SANE_DESC_NUM_OPTIONS,
    SANE_TYPE_INT, SANE_UNIT_NONE, sizeof (SANE_Word), SANE_CAP_SOFT_DETECT,
    SANE_CONSTRAINT_NONE, { 0 }
  };
  s->val[OPT_NUM_OPTS].w = NUM_OPTIONS;
  s->opt[OPT_MODE_GROUP] = (SANE_Option_Descriptor) {
    "mode-group", "Scan Mode", "", SANE_TYPE_GROUP, SANE_UNIT_NONE,
    0, 0, SANE_CONSTRAINT_NONE, { 0 }
  };
  s->opt[OPT_MODE] = (SANE_Option_Descriptor) {
    SANE_NAME_SCAN_MODE, SANE_TITLE_SCAN_MODE, SANE_DESC_SCAN_MODE,
    SANE_TYPE_STRING, SANE_UNIT_NONE, 6,
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
    SANE_CONSTRAINT_STRING_LIST, { .string_list = mode_list }
  };
  s->val[OPT_MODE].s = strdup (SANE_VALUE_SCAN_MODE_GRAY);
  if (!s->val[OPT_MODE].s)
    return SANE_STATUS_NO_MEM;

  s->opt[OPT_CHANNEL] = (SANE_Option_Descriptor) {
    "channel", "Channel", "Selects the V4L2 input channel",
    SANE_TYPE_STRING, SANE_UNIT_NONE,
    sizeof (((struct v4l2_input *) 0)->name),
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
    SANE_CONSTRAINT_STRING_LIST, { .string_list = s->channel }
  };
  if (s->channel[0])
    {
      s->val[OPT_CHANNEL].s = strdup (s->channel[0]);
      if (!s->val[OPT_CHANNEL].s)
        return SANE_STATUS_NO_MEM;
    }
  else
    s->opt[OPT_CHANNEL].cap |= SANE_CAP_INACTIVE;

  s->opt[OPT_FRAME_SIZE] = (SANE_Option_Descriptor) {
    "frame-size", "Frame size", "Selects the V4L2 capture dimensions",
    SANE_TYPE_STRING, SANE_UNIT_NONE, sizeof (s->frame_size_names[0]),
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
    SANE_CONSTRAINT_STRING_LIST, { .string_list = s->frame_size_list }
  };
  s->val[OPT_FRAME_SIZE].s = strdup (s->frame_size_list[0]);
  if (!s->val[OPT_FRAME_SIZE].s)
    return SANE_STATUS_NO_MEM;

  s->opt[OPT_GEOMETRY_GROUP] = (SANE_Option_Descriptor) {
    "geometry-group", "Geometry", "", SANE_TYPE_GROUP, SANE_UNIT_NONE,
    0, SANE_CAP_ADVANCED, SANE_CONSTRAINT_NONE, { 0 }
  };
  s->geometry_range[0] = (SANE_Range) { 0, SANE_FIX (s->frame_width - 1), SANE_FIX (1) };
  s->geometry_range[1] = (SANE_Range) { 0, SANE_FIX (s->frame_height - 1), SANE_FIX (1) };
  s->geometry_range[2] = (SANE_Range) { 1, SANE_FIX (s->frame_width), SANE_FIX (1) };
  s->geometry_range[3] = (SANE_Range) { 1, SANE_FIX (s->frame_height), SANE_FIX (1) };
  s->opt[OPT_TL_X] = (SANE_Option_Descriptor) {
    SANE_NAME_SCAN_TL_X, "Top-left X", "Left edge in pixels",
    SANE_TYPE_INT, SANE_UNIT_PIXEL, sizeof (SANE_Word),
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_RANGE,
    { .range = &s->geometry_range[0] }
  };
  s->opt[OPT_TL_Y] = s->opt[OPT_TL_X];
  s->opt[OPT_TL_Y].name = SANE_NAME_SCAN_TL_Y;
  s->opt[OPT_TL_Y].title = "Top-left Y";
  s->opt[OPT_TL_Y].desc = "Top edge in pixels";
  s->opt[OPT_TL_Y].constraint.range = &s->geometry_range[1];
  s->opt[OPT_BR_X] = s->opt[OPT_TL_X];
  s->opt[OPT_BR_X].name = SANE_NAME_SCAN_BR_X;
  s->opt[OPT_BR_X].title = "Bottom-right X";
  s->opt[OPT_BR_X].desc = "Right edge in pixels";
  s->opt[OPT_BR_X].constraint.range = &s->geometry_range[2];
  s->opt[OPT_BR_Y] = s->opt[OPT_TL_X];
  s->opt[OPT_BR_Y].name = SANE_NAME_SCAN_BR_Y;
  s->opt[OPT_BR_Y].title = "Bottom-right Y";
  s->opt[OPT_BR_Y].desc = "Bottom edge in pixels";
  s->opt[OPT_BR_Y].constraint.range = &s->geometry_range[3];
  s->val[OPT_BR_X].w = SANE_FIX (s->frame_width);
  s->val[OPT_BR_Y].w = SANE_FIX (s->frame_height);
  s->opt[OPT_ENHANCEMENT_GROUP] = (SANE_Option_Descriptor) {
    "enhancement-group", "Enhancement", "", SANE_TYPE_GROUP,
    SANE_UNIT_NONE, 0, 0, SANE_CONSTRAINT_NONE, { 0 }
  };
  s->opt[OPT_BRIGHTNESS] = (SANE_Option_Descriptor) {
    SANE_NAME_BRIGHTNESS, SANE_TITLE_BRIGHTNESS, SANE_DESC_BRIGHTNESS,
    SANE_TYPE_INT, SANE_UNIT_NONE, sizeof (SANE_Word),
    SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_RANGE,
    { .range = &u8_range }
  };
  s->opt[OPT_HUE] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_HUE].name = SANE_NAME_HUE;
  s->opt[OPT_HUE].title = SANE_TITLE_HUE;
  s->opt[OPT_HUE].desc = SANE_DESC_HUE;
  s->opt[OPT_COLOR] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_COLOR].name = "color";
  s->opt[OPT_COLOR].title = SANE_I18N ("Picture color");
  s->opt[OPT_COLOR].desc = SANE_I18N ("Sets the picture's color.");
  s->opt[OPT_CONTRAST] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_CONTRAST].name = SANE_NAME_CONTRAST;
  s->opt[OPT_CONTRAST].title = SANE_TITLE_CONTRAST;
  s->opt[OPT_CONTRAST].desc = SANE_DESC_CONTRAST;
  s->opt[OPT_WHITE_LEVEL] = s->opt[OPT_BRIGHTNESS];
  s->opt[OPT_WHITE_LEVEL].name = SANE_NAME_WHITE_LEVEL;
  s->opt[OPT_WHITE_LEVEL].title = SANE_TITLE_WHITE_LEVEL;
  s->opt[OPT_WHITE_LEVEL].desc = SANE_DESC_WHITE_LEVEL;
  query_control_option (s, OPT_BRIGHTNESS, V4L2_CID_BRIGHTNESS);
  query_control_option (s, OPT_HUE, V4L2_CID_HUE);
  query_control_option (s, OPT_COLOR, V4L2_CID_SATURATION);
  query_control_option (s, OPT_CONTRAST, V4L2_CID_CONTRAST);
  query_control_option (s, OPT_WHITE_LEVEL, V4L2_CID_WHITENESS);
  return SANE_STATUS_GOOD;
}

static void
cleanup_capture (V4L_Scanner *s)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (s->streaming)
    {
      xioctl (s->fd, VIDIOC_STREAMOFF, &type);
      s->streaming = SANE_FALSE;
    }
  if (s->buffer && s->is_mmap)
    v4l2_munmap (s->buffer, s->buffer_size);
  else if (s->buffer)
    free (s->buffer);
  s->buffer = NULL;
  s->buffer_size = 0;
  s->frame_size = 0;
  s->offset = 0;
  s->scanning = SANE_FALSE;
}

static SANE_Status
convert_rgb_to_gray (V4L_Scanner *s)
{
  size_t width = s->format.fmt.pix.width;
  size_t height = s->format.fmt.pix.height;
  size_t stride;
  size_t rows;
  unsigned char *data;

  size_t pixels;

  if (!gray_fallback_as_rgb (s))
    {
      DBG (4, "conversion: no RGB-to-gray fallback required\n");
      return SANE_STATUS_GOOD;
    }
  DBG (4, "conversion: RGB24 to grayscale-compatible RGB output, "
       "width=%zu height=%zu stride=%zu\n", width, height,
       MAX (s->format.fmt.pix.bytesperline, width * 3));
  if (width == 0 || height == 0 || width > SIZE_MAX / 3)
    {
      s->frame_size = 0;
      return SANE_STATUS_INVAL;
    }
  stride = MAX (s->format.fmt.pix.bytesperline, width * 3);
  if (stride > SIZE_MAX / height
      || stride * height > s->frame_size
      || width > SIZE_MAX / height
      || width * height > SIZE_MAX / 3
      || width * height * 3 > s->buffer_size)
    {
      s->frame_size = 0;
      return SANE_STATUS_IO_ERROR;
    }
  rows = height;
  data = s->buffer;
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < width; column++)
      {
        size_t source = row * stride + column * 3;
        size_t destination = (row * width + column) * 3;
        unsigned char gray = (unsigned char) ((77 * data[source]
                                               + 150 * data[source + 1]
                                               + 29 * data[source + 2]) >> 8);
        data[destination] = gray;
        data[destination + 1] = gray;
        data[destination + 2] = gray;
      }
  pixels = rows * width * 3;
  s->frame_size = pixels;
  return SANE_STATUS_GOOD;
}

static SANE_Status
crop_frame (V4L_Scanner *s)
{
  size_t source_width = s->format.fmt.pix.width;
  size_t source_height = s->format.fmt.pix.height;
  size_t left = (size_t) SANE_UNFIX (s->val[OPT_TL_X].w);
  size_t top = (size_t) SANE_UNFIX (s->val[OPT_TL_Y].w);
  size_t width = (size_t) SANE_UNFIX
    (s->val[OPT_BR_X].w - s->val[OPT_TL_X].w);
  size_t height = (size_t) SANE_UNFIX
    (s->val[OPT_BR_Y].w - s->val[OPT_TL_Y].w);
  size_t bytes_per_pixel = s->params.format == SANE_FRAME_GRAY ? 1 : 3;
  size_t source_stride;
  size_t row;

  DBG (4, "crop: input=%zux%zu, area=%zu,%zu %zux%zu, bytes_per_pixel=%zu\n",
       source_width, source_height, left, top, width, height,
       bytes_per_pixel);

  if (source_width > SIZE_MAX / bytes_per_pixel)
    {
      s->frame_size = 0;
      return SANE_STATUS_INVAL;
    }
  source_stride = source_width * bytes_per_pixel;
  if (left >= source_width || top >= source_height || width == 0 || height == 0
      || left + width > source_width || top + height > source_height
      || source_stride > SIZE_MAX / source_height
      || source_stride * source_height > s->buffer_size
      || width > SIZE_MAX / height
      || width * height > SIZE_MAX / bytes_per_pixel)
    {
      s->frame_size = 0;
      return SANE_STATUS_INVAL;
    }
  for (row = 0; row < height; row++)
    memmove ((unsigned char *) s->buffer + row * width * bytes_per_pixel,
             (unsigned char *) s->buffer
               + (top + row) * source_stride + left * bytes_per_pixel,
             width * bytes_per_pixel);
  s->frame_size = width * height * bytes_per_pixel;
  return SANE_STATUS_GOOD;
}

static SANE_Status
start_capture (V4L_Scanner *s)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  size_t size;
  ssize_t result;
  SANE_Status status;

  cleanup_capture (s);
  debug_format (4, "capture requested format", &s->format.fmt.pix);

  /* libv4l2 can emulate read() for streaming-only devices and convert
     formats such as MJPEG and YUYV to the RGB24 or GREY format requested by
     sane_start().  Prefer this path so that the conversion layer is not
     bypassed by direct mmap access. */
  size = MAX((size_t) s->format.fmt.pix.width * s->format.fmt.pix.height * 3,
             MAX((size_t) s->format.fmt.pix.bytesperline * s->format.fmt.pix.height,
                 s->format.fmt.pix.sizeimage));
  s->buffer = malloc (size);
  if (!s->buffer)
    return SANE_STATUS_NO_MEM;
  s->buffer_size = size;
  s->is_mmap = SANE_FALSE;
  /* libv4l2 may implement read() using an internal streaming queue.  Mark
     the capture active before calling it so cleanup_capture() releases that
     queue when the next scan starts or the handle is cancelled. */
  s->streaming = SANE_TRUE;
  result = read_frame (s->fd, s->buffer, size, V4L_CAPTURE_TIMEOUT_MS);
  DBG (4, "capture read path: result=%zd, buffer_size=%zu\n", result, size);
  if (result > 0)
    {
      s->frame_size = (size_t) result;
      status = convert_rgb_to_gray (s);
      if (status != SANE_STATUS_GOOD)
        {
          cleanup_capture (s);
          return status;
        }
      if (crop_frame (s) != SANE_STATUS_GOOD)
        {
          cleanup_capture (s);
          return SANE_STATUS_INVAL;
        }
      s->scanning = SANE_TRUE;
      return SANE_STATUS_GOOD;
    }
  cleanup_capture (s);
  DBG (3, "capture read path unavailable; trying mmap fallback\n");

  /* Some libv4l2/device combinations expose streaming but do not provide
     the read emulation.  Keep a direct mmap fallback for those devices. */
  if (s->format_converted)
    {
      DBG (3, "capture read path unavailable; negotiated format requires "
           "libv4l2 conversion, mmap fallback is not safe\n");
      return SANE_STATUS_UNSUPPORTED;
    }
  memset (&req, 0, sizeof (req));
  req.count = 1;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl (s->fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 1)
    {
      return SANE_STATUS_IO_ERROR;
    }
  memset (&buf, 0, sizeof (buf));
  buf.type = type;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;
  if (xioctl (s->fd, VIDIOC_QUERYBUF, &buf) == -1)
    return SANE_STATUS_IO_ERROR;
  s->buffer_size = buf.length;
  s->buffer = v4l2_mmap (NULL, buf.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, s->fd, buf.m.offset);
  if (s->buffer == MAP_FAILED)
    {
      s->buffer = NULL;
      return SANE_STATUS_IO_ERROR;
    }
  s->is_mmap = SANE_TRUE;
  if (xioctl (s->fd, VIDIOC_QBUF, &buf) == -1
      || xioctl (s->fd, VIDIOC_STREAMON, &type) == -1)
    {
      cleanup_capture (s);
      return SANE_STATUS_IO_ERROR;
    }
  s->streaming = SANE_TRUE;
  if (dequeue_frame (s->fd, &buf, V4L_CAPTURE_TIMEOUT_MS)
      != SANE_STATUS_GOOD)
    {
      cleanup_capture (s);
      return SANE_STATUS_IO_ERROR;
    }
  s->frame_size = MIN(buf.bytesused, s->buffer_size);
  DBG (4, "capture mmap path: bytesused=%u, buffer_size=%zu\n",
       buf.bytesused, s->buffer_size);
  status = convert_rgb_to_gray (s);
  if (status != SANE_STATUS_GOOD)
    {
      cleanup_capture (s);
      return status;
    }
  if (crop_frame (s) != SANE_STATUS_GOOD)
    {
      cleanup_capture (s);
      return SANE_STATUS_INVAL;
    }
  s->scanning = SANE_TRUE;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_init (SANE_Int *version_code, SANE_Auth_Callback authorize)
{
  char line[PATH_MAX];
  FILE *fp;
  char *end;
  (void) authorize;
  DBG_INIT ();
  DBG (3, "sane_init\n");
  if (version_code)
    *version_code = SANE_VERSION_CODE (SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR, BUILD);
  attach_video_devices ();
  fp = sanei_config_open (V4L_CONFIG_FILE);
  if (!fp)
    return SANE_STATUS_GOOD;
  while (sanei_config_read (line, sizeof (line), fp))
    {
      if (line[0] == '#')
        continue;
      end = line;
      while (*end && *end != '#' && *end != ' ' && *end != '\t')
        end++;
      *end = '\0';
      if (*line)
        attach (line);
    }
  fclose (fp);
  return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
  V4L_Device *dev, *next;
  while (first_handle)
    sane_close (first_handle);
  free (devlist);
  devlist = NULL;
  for (dev = first_dev; dev; dev = next)
    {
      next = dev->next;
      free ((void *) dev->sane.name);
      free ((void *) dev->sane.model);
      free (dev);
    }
  first_dev = NULL;
  num_devices = 0;
}

SANE_Status
sane_get_devices (const SANE_Device ***device_list, SANE_Bool local_only)
{
  V4L_Device *dev;
  int i = 0;
  (void) local_only;
  if (!device_list)
    return SANE_STATUS_INVAL;
  free (devlist);
  devlist = calloc ((size_t) num_devices + 1, sizeof (*devlist));
  if (!devlist)
    return SANE_STATUS_NO_MEM;
  for (dev = first_dev; dev; dev = dev->next)
    devlist[i++] = &dev->sane;
  *device_list = devlist;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const devname, SANE_Handle *handle)
{
  V4L_Device *dev;
  V4L_Scanner *s;
  struct v4l2_input input;
  int fd, i;
  SANE_Status status = SANE_STATUS_GOOD;

  if (!devname || !handle)
    return SANE_STATUS_INVAL;
  for (dev = first_dev; dev; dev = dev->next)
    if (strcmp (dev->sane.name, devname) == 0)
      break;
  if (!dev)
    return SANE_STATUS_INVAL;
  fd = v4l2_open (devname, O_RDWR | O_NONBLOCK);
  if (fd == -1)
    return SANE_STATUS_INVAL;
  s = calloc (1, sizeof (*s));
  if (!s)
    {
      v4l2_close (fd);
      return SANE_STATUS_NO_MEM;
    }
  s->fd = fd;
  s->devicename = devname;
  status = query_capability (fd, &s->capability);
  if (status != SANE_STATUS_GOOD)
    goto fail;
  for (i = 0; i < MAX_CHANNELS; i++)
    {
      memset (&input, 0, sizeof (input));
      input.index = i;
      if (xioctl (fd, VIDIOC_ENUMINPUT, &input) == -1)
        break;
      s->channel[i] = strdup ((const char *) input.name);
      if (!s->channel[i])
        {
          status = SANE_STATUS_NO_MEM;
          goto fail;
        }
    }
  set_default_format (s);
  query_frame_sizes (s);
  status = init_options (s);
  if (status != SANE_STATUS_GOOD)
    goto fail;
  status = update_parameters (s);
  if (status != SANE_STATUS_GOOD)
    goto fail;
  s->next = first_handle;
  first_handle = s;
  *handle = s;
  return SANE_STATUS_GOOD;

fail:
  free_channels (s);
  free ((void *) s->val[OPT_MODE].s);
  free ((void *) s->val[OPT_CHANNEL].s);
  free ((void *) s->val[OPT_FRAME_SIZE].s);
  v4l2_close (fd);
  free (s);
  return status;
}

void
sane_close (SANE_Handle handle)
{
  V4L_Scanner **p;
  V4L_Scanner *s = find_handle (handle);
  if (!s)
    return;
  sane_cancel (handle);
  for (p = &first_handle; *p && *p != s; p = &(*p)->next)
    ;
  if (*p)
    *p = s->next;
  free_channels (s);
  free ((void *) s->val[OPT_MODE].s);
  free ((void *) s->val[OPT_CHANNEL].s);
  free ((void *) s->val[OPT_FRAME_SIZE].s);
  if (s->fd >= 0)
    {
      v4l2_close (s->fd);
      s->fd = -1;
    }
  free (s);
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
  V4L_Scanner *s = find_handle (handle);
  if (!s || option < 0 || option >= NUM_OPTIONS)
    return NULL;
  return &s->opt[option];
}

SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option, SANE_Action action,
                     void *value, SANE_Int *info)
{
  V4L_Scanner *s = find_handle (handle);
  char *copy;
  const SANE_Word *word;
  if (!s || !value || option < 0 || option >= NUM_OPTIONS)
    return SANE_STATUS_INVAL;
  if (info)
    *info = 0;
  if (action == SANE_ACTION_GET_VALUE)
    {
      if (option == OPT_MODE || option == OPT_CHANNEL || option == OPT_FRAME_SIZE)
        strcpy (value, s->val[option].s ? s->val[option].s : "");
      else if (option == OPT_NUM_OPTS
               || (option >= OPT_TL_X && option <= OPT_BR_Y)
               || option >= OPT_BRIGHTNESS)
        *(SANE_Word *) value = s->val[option].w;
      else
        return SANE_STATUS_INVAL;
      return SANE_STATUS_GOOD;
    }
  if (action != SANE_ACTION_SET_VALUE
      || !SANE_OPTION_IS_SETTABLE (s->opt[option].cap))
    return SANE_STATUS_INVAL;
  if (option == OPT_MODE || option == OPT_CHANNEL || option == OPT_FRAME_SIZE)
    {
      copy = strdup (value);
      if (!copy)
        return SANE_STATUS_NO_MEM;
      if (option == OPT_FRAME_SIZE)
        {
          unsigned i, width, height;
          for (i = 0; i < s->frame_size_count; i++)
            if (strcmp (s->frame_size_list[i], copy) == 0)
              break;
          if (i == s->frame_size_count
              || sscanf (copy, "%ux%u", &width, &height) != 2)
            {
              free (copy);
              return SANE_STATUS_INVAL;
            }
          s->frame_width = width;
          s->frame_height = height;
          s->geometry_range[0].max = SANE_FIX (width - 1);
          s->geometry_range[1].max = SANE_FIX (height - 1);
          s->geometry_range[2].max = SANE_FIX (width);
          s->geometry_range[3].max = SANE_FIX (height);
          s->val[OPT_TL_X].w = 0;
          s->val[OPT_TL_Y].w = 0;
          s->val[OPT_BR_X].w = SANE_FIX (width);
          s->val[OPT_BR_Y].w = SANE_FIX (height);
          free ((void *) s->val[option].s);
          s->val[option].s = copy;
          if (info)
            *info |= SANE_INFO_RELOAD_PARAMS;
          return SANE_STATUS_GOOD;
        }
      if (option == OPT_CHANNEL)
        {
          int i;
          struct v4l2_input input;
          for (i = 0; i < MAX_CHANNELS && s->channel[i]; i++)
            if (strcmp (s->channel[i], copy) == 0)
              {
                if (s->val[option].s && strcmp (s->val[option].s, copy) == 0)
                  {
                    free (copy);
                    return SANE_STATUS_GOOD;
                  }
                memset (&input, 0, sizeof (input));
                input.index = i;
                if (xioctl (s->fd, VIDIOC_S_INPUT, &input) == -1)
                  {
                    free (copy);
                    return SANE_STATUS_INVAL;
                  }
                free ((void *) s->val[option].s);
                s->val[option].s = copy;
                return SANE_STATUS_GOOD;
              }
          free (copy);
          return SANE_STATUS_INVAL;
        }
      free ((void *) s->val[option].s);
      s->val[option].s = copy;
      if (info)
        *info |= SANE_INFO_RELOAD_PARAMS;
      return SANE_STATUS_GOOD;
    }
  word = value;
  if (option >= OPT_TL_X && option <= OPT_BR_Y)
    {
      if (*word < s->opt[option].constraint.range->min
          || *word > s->opt[option].constraint.range->max)
        return SANE_STATUS_INVAL;
      if ((option == OPT_TL_X && *word > s->val[OPT_BR_X].w)
          || (option == OPT_TL_Y && *word > s->val[OPT_BR_Y].w)
          || (option == OPT_BR_X && *word < s->val[OPT_TL_X].w)
          || (option == OPT_BR_Y && *word < s->val[OPT_TL_Y].w))
        return SANE_STATUS_INVAL;
      s->val[option].w = *word;
      if (info)
        *info |= SANE_INFO_RELOAD_PARAMS;
      return SANE_STATUS_GOOD;
    }
  if (option < OPT_BRIGHTNESS || option > OPT_WHITE_LEVEL)
    return SANE_STATUS_INVAL;
  switch (option)
    {
    case OPT_BRIGHTNESS: return set_control (s, V4L2_CID_BRIGHTNESS, *word);
    case OPT_HUE: return set_control (s, V4L2_CID_HUE, *word);
    case OPT_COLOR: return set_control (s, V4L2_CID_SATURATION, *word);
    case OPT_CONTRAST: return set_control (s, V4L2_CID_CONTRAST, *word);
    case OPT_WHITE_LEVEL: return set_control (s, V4L2_CID_WHITENESS, *word);
    default: return SANE_STATUS_INVAL;
    }
}

SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters *params)
{
  V4L_Scanner *s = find_handle (handle);
  if (!s || !params)
    return SANE_STATUS_INVAL;
  if (update_parameters (s) != SANE_STATUS_GOOD)
    return SANE_STATUS_INVAL;
  *params = s->params;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_start (SANE_Handle handle)
{
  V4L_Scanner *s = find_handle (handle);
  struct v4l2_format format;
  struct v4l2_capability capability;
  __u32 pixel_format;
  int fd;
  SANE_Status status;
  if (!s)
    return SANE_STATUS_INVAL;
  cleanup_capture (s);
  if (s->fd >= 0)
    {
      v4l2_close (s->fd);
      s->fd = -1;
    }
  fd = v4l2_open (s->devicename, O_RDWR | O_NONBLOCK);
  if (fd == -1)
    return SANE_STATUS_IO_ERROR;
  s->fd = fd;
  memset (&capability, 0, sizeof (capability));
  status = query_capability (s->fd, &capability);
  if (status != SANE_STATUS_GOOD)
    goto fail;
  s->capability = capability;
  set_default_format (s);
  s->gray_mode = !(s->val[OPT_MODE].s
                   && strcmp (s->val[OPT_MODE].s,
                              SANE_VALUE_SCAN_MODE_COLOR) == 0);
  s->format.fmt.pix.width = s->frame_width;
  s->format.fmt.pix.height = s->frame_height;
  pixel_format = V4L2_PIX_FMT_RGB24;
  format = s->format;
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.pixelformat = pixel_format;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  if (xioctl (s->fd, VIDIOC_S_FMT, &format) == -1)
    {
      status = SANE_STATUS_INVAL;
      goto fail;
    }
  s->format_converted = format.fmt.pix.pixelformat != pixel_format;
  if (s->format_converted)
    DBG (3, "negotiated native format differs from requested RGB24; "
         "using libv4l2 conversion\n");
  /* Keep the format requested from libv4l2 as the logical SANE frame format.
     A direct mmap fallback is disabled above when conversion is required. */
  format.fmt.pix.pixelformat = pixel_format;
  s->format = format;
  debug_format (3, "negotiated format", &s->format.fmt.pix);
  status = update_parameters (s);
  if (status != SANE_STATUS_GOOD)
    goto fail;
  status = start_capture (s);
  if (status == SANE_STATUS_GOOD)
    return status;

fail:
  cleanup_capture (s);
  if (s->fd >= 0)
    {
      v4l2_close (s->fd);
      s->fd = -1;
    }
  return status;
}

SANE_Status
sane_read (SANE_Handle handle, SANE_Byte *buf, SANE_Int max_len, SANE_Int *lenp)
{
  V4L_Scanner *s = find_handle (handle);
  size_t remaining, count;
  if (!s || !buf || !lenp || max_len < 0)
    return SANE_STATUS_INVAL;
  if (!s->scanning || s->offset >= s->frame_size)
    {
      *lenp = 0;
      return SANE_STATUS_EOF;
    }
  remaining = s->frame_size - s->offset;
  count = MIN(remaining, (size_t) max_len);
  memcpy (buf, (unsigned char *) s->buffer + s->offset, count);
  s->offset += count;
  *lenp = (SANE_Int) count;
  if (s->offset >= s->frame_size)
    cleanup_capture (s);
  return SANE_STATUS_GOOD;
}

void
sane_cancel (SANE_Handle handle)
{
  V4L_Scanner *s = find_handle (handle);
  if (s)
    cleanup_capture (s);
}

SANE_Status
sane_set_io_mode (SANE_Handle __sane_unused__ handle, SANE_Bool non_blocking)
{
  return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status
sane_get_select_fd (SANE_Handle __sane_unused__ handle,
                    SANE_Int __sane_unused__ *fd)
{
  return SANE_STATUS_UNSUPPORTED;
}
