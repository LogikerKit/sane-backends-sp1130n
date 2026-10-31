/* sane - Scanner Access Now Easy.
   Copyright (C) 1997 David Mosberger-Tang
   Updates and bugfixes (C) 2002. 2003 Henning Meier-Geinitz

   This file is part of the SANE package.

   As a special exception, the authors of SANE give permission for
   additional uses of the libraries contained in this release of SANE.

   The exception is that, if you link a SANE library with other files
   to produce an executable, this does not by itself cause the
   resulting executable to be covered by the GNU General Public
   License.  Your use of that executable is in no way restricted on
   account of linking the SANE library code into it.

   This exception does not, however, invalidate any other reasons why
   the executable file might be covered by the GNU General Public
   License.

   If you submit changes to SANE to the maintainers to be included in
   a subsequent release, you agree by submitting the changes that
   those changes may be distributed with this exception intact.

   If you write modifications of your own for SANE, it is your choice
   whether to permit this exception to apply to your modifications.
   If you do not wish that, delete this exception notice.
*/

#ifndef v4l_h
#define v4l_h

#include <../include/sane/sane.h>
#include <linux/videodev2.h>

#define MAX_CHANNELS 32
#define MAX_FRAME_SIZES 64
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

typedef enum
{
  V4L_RES_LOW = 0,
  V4L_RES_HIGH
}
V4L_Resolution;

typedef enum
{
  OPT_NUM_OPTS = 0,

  OPT_MODE_GROUP,
  OPT_MODE,
  OPT_CHANNEL,
  OPT_FRAME_SIZE,

  OPT_GEOMETRY_GROUP,
  OPT_TL_X,			/* top-left x */
  OPT_TL_Y,			/* top-left y */
  OPT_BR_X,			/* bottom-right x */
  OPT_BR_Y,			/* bottom-right y */

  OPT_ENHANCEMENT_GROUP,
  OPT_BRIGHTNESS,
  OPT_HUE,
  OPT_COLOR,
  OPT_CONTRAST,
  OPT_WHITE_LEVEL,

  /* must come last: */
  NUM_OPTIONS
}
V4L_Option;

typedef struct V4L_Device
{
  struct V4L_Device *next;
  SANE_Device sane;
}
V4L_Device;

typedef struct V4L_Scanner
{
  struct V4L_Scanner *next;

  SANE_Option_Descriptor opt[NUM_OPTIONS];
  Option_Value val[NUM_OPTIONS];
  V4L_Resolution resolution;
  SANE_Parameters params;
  SANE_String_Const devicename;	/* Name of the Device */
  int fd;			/* Filedescriptor */
  SANE_Int user_corner;		/* bitmask of user-selected coordinates */
  SANE_Bool scanning;
  SANE_Bool deliver_eof;
  SANE_Bool gray_mode;
  SANE_Bool is_mmap;
  SANE_Bool streaming;
  SANE_Bool format_converted;
  void *buffer;
  size_t buffer_size;
  size_t frame_size;
  size_t offset;
  struct v4l2_capability capability;
  struct v4l2_format format;
  SANE_String_Const channel[MAX_CHANNELS];
  SANE_String_Const frame_size_list[MAX_FRAME_SIZES + 1];
  char frame_size_names[MAX_FRAME_SIZES][32];
  SANE_Range geometry_range[4];
  unsigned frame_size_count;
  __u32 frame_width;
  __u32 frame_height;
}
V4L_Scanner;

#endif /* v4l_h */
