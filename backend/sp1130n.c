/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#include "../include/sane/config.h"
#include "sp1130n.h"
#include "sp1130n-image.h"
#include "sp1130n-postprocess.h"
#include "../include/sane/sane.h"
#include "../include/sane/sanei_magic.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/sane/sanei_backend.h"

enum
{
  OPT_COUNT, OPT_MODE, OPT_SOURCE, OPT_DPI, OPT_WIDTH, OPT_HEIGHT,
  OPT_BRIGHTNESS, OPT_CONTRAST, OPT_THRESHOLD, OPT_BACK_ROTATE,
  OPT_TL_X, OPT_TL_Y, OPT_BR_X, OPT_BR_Y, OPT_AUTOCROP, OPT_DESKEW,
  OPT_SKIP_BLANK, OPT_BLANK_FRACTION, OPT_CROP_MARGIN,
  OPT_UPRIGHT_LOADING, OPT_BATCH_SCAN, N_OPTIONS
};
static const SANE_Word resolutions[] = { 2, 300, 600 };
static const SANE_Word network_resolutions[] = { 1, 300 };
static const SANE_String_Const modes[] = { "Color", "Gray", "Lineart", NULL };
static const SANE_String_Const sources[] =
  { "ADF Duplex", "ADF Front", "ADF Back", NULL };
static const SANE_Range widths =
  { SANE_FIX (50), SANE_FIX (216), SANE_FIX (1) };
static const SANE_Range heights =
  { SANE_FIX (50), SANE_FIX (356), SANE_FIX (1) };
static const SANE_Range adjustments = { -100, 100, 1 };
static const SANE_Range thresholds = { 0, 255, 1 };
static const SANE_Range horizontal = { 0, SANE_FIX (216), SANE_FIX (1) };
static const SANE_Range vertical = { 0, SANE_FIX (356), SANE_FIX (1) };
static const SANE_Range fractions = { 0, 1000, 1 };
static const SANE_Range margins = { 0, 10000, 1 };

static const SANE_Option_Descriptor options[N_OPTIONS] = {
  {"", SANE_I18N ("Number of options"), "", SANE_TYPE_INT, SANE_UNIT_NONE,
   sizeof (SANE_Word),
   SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_NONE, {NULL}
   },
  {"mode", SANE_I18N ("Scan mode"),
   SANE_I18N
   ("Gray and Lineart are converted on the computer after RGB scanning"),
   SANE_TYPE_STRING,
   SANE_UNIT_NONE, 16, SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_STRING_LIST, {.string_list = modes}
   },
  {"source", SANE_I18N ("Scan source"),
   SANE_I18N ("Both sides, front only or back only"), SANE_TYPE_STRING,
   SANE_UNIT_NONE, 32, SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_STRING_LIST, {.string_list = sources}
   },
  {"resolution", SANE_I18N ("Resolution"), "", SANE_TYPE_INT, SANE_UNIT_DPI,
   sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_WORD_LIST, {.word_list = resolutions}
   },
  {"page-width", SANE_I18N ("Page width"),
   SANE_I18N ("Centered in the feeder"),
   SANE_TYPE_FIXED, SANE_UNIT_MM,
   sizeof (SANE_Word), SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &widths}
   },
  {"page-height", SANE_I18N ("Page height"),
   SANE_I18N ("Physical paper height before optional software cropping"),
   SANE_TYPE_FIXED,
   SANE_UNIT_MM, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &heights}
   },
  {"brightness", SANE_I18N ("Brightness"),
   SANE_I18N ("Software brightness adjustment after scanning"), SANE_TYPE_INT,
   SANE_UNIT_PERCENT, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &adjustments}
   },
  {"contrast", SANE_I18N ("Contrast"),
   SANE_I18N ("Software contrast adjustment after scanning"), SANE_TYPE_INT,
   SANE_UNIT_PERCENT, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &adjustments}
   },
  {"threshold", SANE_I18N ("Black-and-white threshold"),
   SANE_I18N ("In Lineart mode, values below this become black"),
   SANE_TYPE_INT, SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT | SANE_CAP_INACTIVE,
   SANE_CONSTRAINT_RANGE, {.range = &thresholds}
   },
  {"back-rotate", SANE_I18N ("Rotate back side 180 degrees"),
   SANE_I18N ("Software rotation; front side remains unchanged"),
   SANE_TYPE_BOOL, SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_NONE, {NULL}
   },
  {"tl-x", SANE_I18N ("Top-left x"),
   SANE_I18N ("Scan-window origin within centered paper"), SANE_TYPE_FIXED,
   SANE_UNIT_MM, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &horizontal}
   },
  {"tl-y", SANE_I18N ("Top-left y"),
   SANE_I18N ("Scan-window origin within paper"), SANE_TYPE_FIXED,
   SANE_UNIT_MM, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &vertical}
   },
  {"br-x", SANE_I18N ("Bottom-right x"),
   SANE_I18N ("Right scan-window boundary; defaults to paper edge"),
   SANE_TYPE_FIXED,
   SANE_UNIT_MM, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &horizontal}
   },
  {"br-y", SANE_I18N ("Bottom-right y"),
   SANE_I18N ("Bottom scan-window boundary; defaults to paper edge"),
   SANE_TYPE_FIXED,
   SANE_UNIT_MM, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &vertical}
   },
  {"autocrop", SANE_I18N ("Crop to paper edges"),
   SANE_I18N ("Software paper-edge detection using SANE shared helpers"),
   SANE_TYPE_BOOL,
   SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_NONE, {NULL}
   },
  {"deskew", SANE_I18N ("Deskew"),
   SANE_I18N ("Software paper-edge deskew up to five degrees using SANE shared helpers"),
   SANE_TYPE_BOOL,
   SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_NONE, {NULL}
   },
  {"skip-blank", SANE_I18N ("Skip blank sides"),
   SANE_I18N
   ("May omit either side; reads at most 32 blank sheets per start"),
   SANE_TYPE_BOOL, SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_NONE, {NULL}
   },
  {"blank-per-mille", SANE_I18N ("Blank darkness limit"),
   SANE_I18N ("Maximum average darkness per thousand for a blank side"),
   SANE_TYPE_INT, SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_RANGE,
   {.range = &fractions}
   },
  {"crop-margin", SANE_I18N ("Paper-edge crop margin"),
   SANE_I18N ("Pixels retained around detected paper edges"), SANE_TYPE_INT,
   SANE_UNIT_PIXEL, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT,
   SANE_CONSTRAINT_RANGE, {.range = &margins}
   },
  {"upright-loading", SANE_I18N ("Front face up, text upright"),
   SANE_I18N
   ("Optional USB face-up loading: reverse side selection and rotate both images 180 degrees; leave disabled for face-down loading"),
   SANE_TYPE_BOOL, SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_NONE, {NULL}
   },
  {"batch-scan", SANE_I18N ("Return all sheets"),
   SANE_I18N
   ("When disabled, consume all loaded sheets but return only the first sheet; this is not single-sheet feeding"),
   SANE_TYPE_BOOL, SANE_UNIT_NONE, sizeof (SANE_Word),
   SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT, SANE_CONSTRAINT_NONE, {NULL}
   }
};

struct handle
{
  struct sanei_sp1130n_connection *connection;
  struct sanei_sp1130n_settings settings;
  struct sanei_sp1130n_image_options image;
  struct sanei_sp1130n_post_options post;
  int rotate_back, skip_blank, right_set, bottom_set, upright_loading;
  int batch_scan;
  unsigned right_mm, bottom_mm;
  SANE_Range horizontal_range, vertical_range;
  SANE_Option_Descriptor descriptors[N_OPTIONS];
  struct sanei_sp1130n_sheet sheet;
  struct sanei_sp1130n_session session;
  uint8_t token[40];
  unsigned side;
  size_t offset;
  int active, in_start, network;
  int cancel_reaped, closing, cleanup_exited;
  pthread_t cleanup_thread;
};
static struct handle *current;
/* Signal-handler cancellation never touches dynamically allocated storage. */
static _Atomic (SANE_Handle) cancellable_handle;
static atomic_bool cancel_requested;
_Static_assert (ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2,
                "SP-1130N signal cancellation requires lock-free atomics");
static SANE_Device **device_list;
static pthread_mutex_t state = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;

static void *cancellation_worker (void *);

static void
log_header (const char *phase, int status, const uint8_t *h, size_t size)
{
  if (size >= 32)
    DBG (3, "%s: status=%d error=0x%08x event=%u tag=0x%08x side=%u width=%u height=%u bytes=%u\n",
         phase, status, sanei_sp1130n_le32 (h),
         h[6], sanei_sp1130n_le32 (h + 8), h[11],
         sanei_sp1130n_le32 (h + 20), sanei_sp1130n_le32 (h + 16),
         size >= 64 ? sanei_sp1130n_le32 (h + 36) : 0);
  else
    DBG (3, "%s: status=%d\n", phase, status);
}

static void
free_devices (void)
{
  if (!device_list)
    return;
  for (size_t i = 0; device_list[i]; ++i)
    {
      free ((void *) device_list[i]->name);
      free (device_list[i]);
    }
  free (device_list);
  device_list = NULL;
}

SANE_Status
sane_sp1130n_init (SANE_Int *version, SANE_Auth_Callback callback)
{
  DBG_INIT ();
  sanei_magic_init ();
  DBG (1, "initializing SP-1130N backend\n");
  (void) callback;
  if (version)
    *version = SANE_VERSION_CODE (1, 0, 3);
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_sp1130n_get_devices (const SANE_Device ***result, SANE_Bool local_only)
{
  (void) local_only;
  if (!result)
    return SANE_STATUS_INVAL;
  *result = NULL;
  struct sanei_sp1130n_device *found = NULL;
  size_t count = 0;
  int status = sanei_sp1130n_usb_list (&found, &count);
  if (status != SP_OK)
    return (SANE_Status) status;
  free_devices ();
  device_list = calloc (count + 1, sizeof *device_list);
  if (!device_list)
    {
      free (found);
      return SANE_STATUS_NO_MEM;
    }
  for (size_t i = 0; i < count; ++i)
    {
      device_list[i] = calloc (1, sizeof **device_list);
      if (!device_list[i])
        {
          free (found);
          free_devices ();
          return SANE_STATUS_NO_MEM;
        }
      const char *local_name = found[i].name + strlen ("sp1130n:");
      size_t size = strlen (local_name) + 1;
      char *name = malloc (size);
      if (!name)
        {
          free (found);
          free_devices ();
          return SANE_STATUS_NO_MEM;
        }
      memcpy (name, local_name, size);
      *device_list[i] = (SANE_Device)
      {
      name, found[i].vendor == SP_FUJITSU_VID ? "Fujitsu" : "Ricoh",
        "SP-1130N", "sheetfed scanner"};
    }
  free (found);
  *result = (const SANE_Device **) device_list;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_sp1130n_open (SANE_String_Const name, SANE_Handle *result)
{
  if (!name || !result)
    return SANE_STATUS_INVAL;
  *result = NULL;
  pthread_mutex_lock (&state);
  if (current)
    {
      pthread_mutex_unlock (&state);
      return SANE_STATUS_DEVICE_BUSY;
    }
  char selected_name[64];
  int status = SP_OK;
  if (!*name)
    {
      struct sanei_sp1130n_device *found = NULL;
      size_t count = 0;
      status = sanei_sp1130n_usb_list (&found, &count);
      if (status == SP_OK && !count)
        status = SP_INVALID;
      if (status == SP_OK)
        {
          memcpy (selected_name, found[0].name, sizeof selected_name);
          selected_name[sizeof selected_name - 1] = '\0';
          name = selected_name;
        }
      free (found);
    }
  struct handle *h = NULL;
  if (status == SP_OK)
    {
      h = calloc (1, sizeof *h);
      if (!h)
        status = SP_NOMEM;
    }
  if (status == SP_OK)
    status = sanei_sp1130n_token (h->token);
  if (status == SP_OK)
    {
      char full_name[80];
      if (strncmp (name, "sp1130n:", 8))
        {
          int length =
            snprintf (full_name, sizeof full_name, "sp1130n:%s", name);
          if (length < 0 || (size_t) length >= sizeof full_name)
            status = SP_INVALID;
          else
            name = full_name;
        }
      if (status == SP_OK)
        status = sanei_sp1130n_connection_open (name, &h->connection);
    }
  if (status == SP_OK)
    {
      h->settings = (struct sanei_sp1130n_settings)
      {.dpi = 300,.width_mm = 210,.height_mm = 297,
        .source = SP_SOURCE_DUPLEX
      };
      h->image = (struct sanei_sp1130n_image_options)
      { SP_IMAGE_COLOR, 0, 0, 128, 0 };
      h->post = (struct sanei_sp1130n_post_options)
      {.blank_per_mille = 1,
        .crop_margin = 8
      };
      memcpy (h->descriptors, options, sizeof options);
      h->horizontal_range = horizontal;
      h->horizontal_range.max = SANE_FIX (h->settings.width_mm);
      h->vertical_range = vertical;
      h->vertical_range.max = SANE_FIX (h->settings.height_mm);
      h->descriptors[OPT_TL_X].constraint.range = &h->horizontal_range;
      h->descriptors[OPT_BR_X].constraint.range = &h->horizontal_range;
      h->descriptors[OPT_TL_Y].constraint.range = &h->vertical_range;
      h->descriptors[OPT_BR_Y].constraint.range = &h->vertical_range;
      atomic_store (&cancel_requested, 0);
      struct sanei_sp1130n_io io =
        sanei_sp1130n_connection_io (h->connection, &cancel_requested);
      h->network = io.network;
      h->upright_loading = SANE_FALSE;
      h->batch_scan = SANE_TRUE;
      if (h->network)
        {
          h->descriptors[OPT_UPRIGHT_LOADING].cap |= SANE_CAP_INACTIVE;
          h->descriptors[OPT_DPI].constraint.word_list = network_resolutions;
          for (unsigned n = 0; n < N_OPTIONS; ++n)
            if (n == OPT_WIDTH || n == OPT_HEIGHT ||
                (n >= OPT_TL_X && n <= OPT_BR_Y))
              h->descriptors[n].cap |= SANE_CAP_INACTIVE;
        }
      if (pthread_create (&h->cleanup_thread, NULL, cancellation_worker, h))
        status = SP_NOMEM;
      else
        {
          current = h;
          atomic_store (&cancellable_handle, h);
          *result = h;
        }
    }
  if (status != SP_OK && h)
    {
      sanei_sp1130n_connection_close (h->connection);
      free (h);
    }
  pthread_mutex_unlock (&state);
  return (SANE_Status) status;
}

static void
finish_scan (struct handle *h)
{
  struct sanei_sp1130n_io io =
    sanei_sp1130n_connection_io (h->connection, NULL);
  io.log_header = log_header;
  int status = sanei_sp1130n_scan_finish (&io, h->token, &h->session);
  if (status != SP_OK)
    DBG (1, "bounded acquisition cleanup failed: %d\n", status);
  sanei_sp1130n_connection_idle (h->connection);
}

/* Called with state held, never while the acquisition worker owns the sheet. */
static void
reset_scan (struct handle *h)
{
  finish_scan (h);
  h->session = (struct sanei_sp1130n_session) { 0 };
  sanei_sp1130n_sheet_free (&h->sheet);
  h->active = 0;
  h->side = 0;
  h->offset = 0;
}

static void
reap_cancellation (struct handle *h)
{
  if (!h->in_start && !h->cancel_reaped &&
      atomic_load (&cancel_requested))
    {
      reset_scan (h);
      h->cancel_reaped = 1;
    }
}

static void *
cancellation_worker (void *argument)
{
  struct handle *h = argument;
  /* Cancel can run in a signal handler, so it cannot take a mutex or do I/O.
   * Poll its lock-free flag even when the frontend makes no further call. */
  const struct timespec pause = { .tv_nsec = 100 * 1000 * 1000 };
  for (;;)
    {
      pthread_mutex_lock (&state);
      if (h->closing)
        {
          h->cleanup_exited = 1;
          pthread_cond_broadcast (&idle);
          pthread_mutex_unlock (&state);
          return NULL;
        }
      reap_cancellation (h);
      pthread_mutex_unlock (&state);
      nanosleep (&pause, NULL);
    }
}

static void
resize_window (unsigned page_mm, unsigned *origin, unsigned *edge,
               int explicit_edge, SANE_Range *range)
{
  range->max = SANE_FIX (page_mm);
  if (explicit_edge && *edge > page_mm)
    *edge = page_mm;
  unsigned boundary = explicit_edge ? *edge : page_mm;
  if (!boundary)
    boundary = *edge = 1;
  if (*origin >= boundary)
    *origin = boundary - 1;
}

void
sane_sp1130n_cancel (SANE_Handle handle)
{
  if (handle && handle == atomic_load (&cancellable_handle))
    atomic_store (&cancel_requested, 1);
}

void
sane_sp1130n_close (SANE_Handle handle)
{
  pthread_mutex_lock (&state);
  if (!handle || handle != current || current->closing)
    {
      pthread_mutex_unlock (&state);
      return;
    }
  atomic_store (&cancellable_handle, NULL);
  atomic_store (&cancel_requested, 1);
  current->closing = 1;
  while (current->in_start || !current->cleanup_exited)
    pthread_cond_wait (&idle, &state);
  pthread_join (current->cleanup_thread, NULL);
  finish_scan (current);
  sanei_sp1130n_sheet_free (&current->sheet);
  sanei_sp1130n_connection_close (current->connection);
  free (current);
  current = NULL;
  pthread_mutex_unlock (&state);
}

void
sane_sp1130n_exit (void)
{
  sane_sp1130n_close (current);
  free_devices ();
}

const SANE_Option_Descriptor *
sane_sp1130n_get_option_descriptor (SANE_Handle h, SANE_Int n)
{
  return h && h == current && n >= 0
    && n < N_OPTIONS ? &current->descriptors[n] : NULL;
}

SANE_Status
sane_sp1130n_control_option (SANE_Handle handle, SANE_Int n,
                             SANE_Action action, void *value, SANE_Int *info)
{
  if (info)
    *info = 0;
  pthread_mutex_lock (&state);
  struct handle *h = current;
  int status = SP_OK;
  if (!handle || handle != h || h->closing ||
      !value || n < 0 || n >= N_OPTIONS)
    {
      status = SP_INVALID;
      goto done;
    }
  if (action == SANE_ACTION_GET_VALUE)
    {
      switch (n)
        {
        case OPT_COUNT:
          *(SANE_Word *) value = N_OPTIONS;
          break;
        case OPT_MODE:
          strcpy (value, modes[h->image.mode]);
          break;
        case OPT_SOURCE:
          strcpy (value, h->settings.source == SP_SOURCE_DUPLEX ?
                  "ADF Duplex" : h->settings.source == SP_SOURCE_BACK ?
                  "ADF Back" : "ADF Front");
          break;
        case OPT_DPI:
          *(SANE_Word *) value = (SANE_Word) h->settings.dpi;
          break;
        case OPT_WIDTH:
          *(SANE_Word *) value = SANE_FIX (h->settings.width_mm);
          break;
        case OPT_HEIGHT:
          *(SANE_Word *) value = SANE_FIX (h->settings.height_mm);
          break;
        case OPT_BRIGHTNESS:
          *(SANE_Word *) value = h->image.brightness;
          break;
        case OPT_CONTRAST:
          *(SANE_Word *) value = h->image.contrast;
          break;
        case OPT_THRESHOLD:
          *(SANE_Word *) value = (SANE_Word) h->image.threshold;
          break;
        case OPT_BACK_ROTATE:
          *(SANE_Word *) value = h->rotate_back;
          break;
        case OPT_TL_X:
          *(SANE_Word *) value = SANE_FIX (h->settings.left_mm);
          break;
        case OPT_TL_Y:
          *(SANE_Word *) value = SANE_FIX (h->settings.top_mm);
          break;
        case OPT_BR_X:
          *(SANE_Word *) value =
            SANE_FIX (h->right_set ? h->right_mm : h->settings.width_mm);
          break;
        case OPT_BR_Y:
          *(SANE_Word *) value =
            SANE_FIX (h->bottom_set ? h->bottom_mm : h->settings.height_mm);
          break;
        case OPT_AUTOCROP:
          *(SANE_Word *) value = h->post.autocrop;
          break;
        case OPT_DESKEW:
          *(SANE_Word *) value = h->post.deskew;
          break;
        case OPT_SKIP_BLANK:
          *(SANE_Word *) value = h->skip_blank;
          break;
        case OPT_BLANK_FRACTION:
          *(SANE_Word *) value = (SANE_Word) h->post.blank_per_mille;
          break;
        case OPT_CROP_MARGIN:
          *(SANE_Word *) value = (SANE_Word) h->post.crop_margin;
          break;
        case OPT_UPRIGHT_LOADING:
          *(SANE_Word *) value = h->upright_loading;
          break;
        case OPT_BATCH_SCAN:
          *(SANE_Word *) value = h->batch_scan;
          break;
        }
      goto done;
    }
  if (action != SANE_ACTION_SET_VALUE || n == OPT_COUNT)
    {
      status = SP_INVALID;
      goto done;
    }
  reap_cancellation (h);
  if (h->in_start || h->session.started || h->session.own_lock ||
      (h->active && (h->offset < h->sheet.page[h->side].size ||
                     h->side + 1 < h->sheet.count)))
    {
      status = SP_BUSY;
      goto done;
    }
  if (h->descriptors[n].cap & SANE_CAP_INACTIVE)
    {
      status = SP_INVALID;
      goto done;
    }
  if (n == OPT_MODE)
    {
      enum sanei_sp1130n_image_mode mode;
      if (!strcmp (value, "Color"))
        mode = SP_IMAGE_COLOR;
      else if (!strcmp (value, "Gray"))
        mode = SP_IMAGE_GRAY;
      else if (!strcmp (value, "Lineart"))
        mode = SP_IMAGE_LINEART;
      else
        {
          status = SP_INVALID;
          goto done;
        }
      if (h->image.mode != mode && info)
        *info |= SANE_INFO_RELOAD_OPTIONS;
      h->image.mode = mode;
      if (mode == SP_IMAGE_LINEART)
        h->descriptors[OPT_THRESHOLD].cap &= ~SANE_CAP_INACTIVE;
      else
        h->descriptors[OPT_THRESHOLD].cap |= SANE_CAP_INACTIVE;
    }
  else if (n == OPT_SOURCE)
    {
      if (!strcmp (value, "ADF Duplex"))
        h->settings.source = SP_SOURCE_DUPLEX;
      else if (!strcmp (value, "ADF Front"))
        h->settings.source = SP_SOURCE_FRONT;
      else if (!strcmp (value, "ADF Back"))
        h->settings.source = SP_SOURCE_BACK;
      else
        status = SP_INVALID;
    }
  else if (n == OPT_DPI)
    {
      SANE_Word v = *(SANE_Word *) value;
      if ((v != 300 && v != 600) || (h->network && v != 300))
        status = SP_INVALID;
      else
        h->settings.dpi = (unsigned) v;
    }
  else if (n == OPT_BRIGHTNESS || n == OPT_CONTRAST)
    {
      SANE_Word v = *(SANE_Word *) value;
      if (v < -100 || v > 100)
        status = SP_INVALID;
      else if (n == OPT_BRIGHTNESS)
        h->image.brightness = v;
      else
        h->image.contrast = v;
    }
  else if (n == OPT_THRESHOLD)
    {
      SANE_Word v = *(SANE_Word *) value;
      if (h->image.mode != SP_IMAGE_LINEART || v < 0 || v > 255)
        status = SP_INVALID;
      else
        h->image.threshold = (unsigned) v;
    }
  else if (n == OPT_BACK_ROTATE || n == OPT_AUTOCROP || n == OPT_DESKEW
           || n == OPT_SKIP_BLANK || n == OPT_UPRIGHT_LOADING
           || n == OPT_BATCH_SCAN)
    {
      SANE_Word v = *(SANE_Word *) value;
      if (v != SANE_FALSE && v != SANE_TRUE)
        status = SP_INVALID;
      else if (n == OPT_BACK_ROTATE)
        h->rotate_back = v;
      else if (n == OPT_AUTOCROP)
        h->post.autocrop = v;
      else if (n == OPT_DESKEW)
        h->post.deskew = v;
      else if (n == OPT_UPRIGHT_LOADING)
        h->upright_loading = v;
      else if (n == OPT_BATCH_SCAN)
        h->batch_scan = v;
      else
        h->skip_blank = v;
    }
  else if (n == OPT_BLANK_FRACTION || n == OPT_CROP_MARGIN)
    {
      SANE_Word v = *(SANE_Word *) value;
      SANE_Word maximum =
        n == OPT_BLANK_FRACTION ? 1000 : 10000;
      if (v < 0 || v > maximum)
        status = SP_INVALID;
      else if (n == OPT_BLANK_FRACTION)
        h->post.blank_per_mille = (unsigned) v;
      else
        h->post.crop_margin = (unsigned) v;
    }
  else
    {
      SANE_Word v = *(SANE_Word *) value;
      unsigned maximum = n == OPT_WIDTH ? 216 : n == OPT_HEIGHT ? 356 :
        n == OPT_TL_X || n == OPT_BR_X ? h->settings.width_mm :
        h->settings.height_mm;
      unsigned minimum = n == OPT_WIDTH || n == OPT_HEIGHT ? 50 : 0;
      if (v < SANE_FIX (minimum) || v > SANE_FIX (maximum))
        {
          status = SP_INVALID;
          goto done;
        }
      unsigned mm =
        (unsigned) ((v + SANE_FIX (0.5)) >> SANE_FIXED_SCALE_SHIFT);
      if (v != SANE_FIX (mm) && info)
        *info |= SANE_INFO_INEXACT;
      *(SANE_Word *) value = SANE_FIX (mm);
      if (n == OPT_WIDTH)
        {
          h->settings.width_mm = mm;
          resize_window (mm, &h->settings.left_mm, &h->right_mm,
                         h->right_set, &h->horizontal_range);
          if (info)
            *info |= SANE_INFO_RELOAD_OPTIONS;
        }
      else if (n == OPT_HEIGHT)
        {
          h->settings.height_mm = mm;
          resize_window (mm, &h->settings.top_mm, &h->bottom_mm,
                         h->bottom_set, &h->vertical_range);
          if (info)
            *info |= SANE_INFO_RELOAD_OPTIONS;
        }
      else if (n == OPT_TL_X)
        h->settings.left_mm = mm;
      else if (n == OPT_TL_Y)
        h->settings.top_mm = mm;
      else if (n == OPT_BR_X)
        {
          h->right_mm = mm;
          h->right_set = 1;
        }
      else
        {
          h->bottom_mm = mm;
          h->bottom_set = 1;
        }
    }
  if (status == SP_OK)
    {
      if (h->active)
        {
          sanei_sp1130n_sheet_free (&h->sheet);
          h->active = 0;
        }
      if (info)
        *info |= SANE_INFO_RELOAD_PARAMS;
    }
done:
  pthread_mutex_unlock (&state);
  return (SANE_Status) status;
}

static int
scan_settings (const struct handle *h,
               struct sanei_sp1130n_settings *settings)
{
  unsigned right = h->right_set ? h->right_mm : h->settings.width_mm;
  unsigned bottom = h->bottom_set ? h->bottom_mm : h->settings.height_mm;
  if (right <= h->settings.left_mm || bottom <= h->settings.top_mm)
    return SP_INVALID;
  *settings = h->settings;
  settings->scan_width_mm = right - settings->left_mm;
  settings->scan_height_mm = bottom - settings->top_mm;
  struct sanei_sp1130n_geometry g;
  int status = sanei_sp1130n_scan_geometry (settings, &g);
  if (status != SP_OK)
    return status;
  if (h->upright_loading)
    {
      /* Translate logical upright coordinates and sides to the native feeder. */
      settings->left_mm = settings->width_mm - right;
      settings->top_mm = settings->height_mm - bottom;
      if (settings->source != SP_SOURCE_DUPLEX)
        settings->source = settings->source == SP_SOURCE_FRONT ?
          SP_SOURCE_BACK : SP_SOURCE_FRONT;
    }
  return SP_OK;
}

SANE_Status
sane_sp1130n_get_parameters (SANE_Handle handle, SANE_Parameters *p)
{
  if (!p)
    return SANE_STATUS_INVAL;
  pthread_mutex_lock (&state);
  struct handle *h = current;
  if (!handle || handle != h || h->closing)
    {
      pthread_mutex_unlock (&state);
      return SANE_STATUS_INVAL;
    }
  reap_cancellation (h);
  struct sanei_sp1130n_settings settings;
  struct sanei_sp1130n_geometry g;
  if (scan_settings (h, &settings) != SP_OK
      || sanei_sp1130n_scan_geometry (&settings, &g) != SP_OK)
    {
      pthread_mutex_unlock (&state);
      return SANE_STATUS_INVAL;
    }
  unsigned width = g.width, height = g.height;
  if (h->network)
    height = 3508;
  if (h->active)
    {
      width = h->sheet.page[h->side].width;
      height = h->sheet.page[h->side].height;
    }
  /* Each document side is an independent SANE image, not a color-component frame. */
  SANE_Frame format =
    h->image.mode == SP_IMAGE_COLOR ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
  SANE_Int depth = h->image.mode == SP_IMAGE_LINEART ? 1 : 8;
  *p = (SANE_Parameters)
  {
    format, SANE_TRUE,
    (SANE_Int) sanei_sp1130n_image_stride (width, h->image.mode),
    (SANE_Int) width, (SANE_Int) height, depth
  };
  if (!h->active && h->post.autocrop)
    p->lines = -1;
  pthread_mutex_unlock (&state);
  return SANE_STATUS_GOOD;
}

/* Retain the first sheet; complete the feeder job before exposing any image. */
static int
drain_feeder (struct handle *h, struct sanei_sp1130n_io *io,
              const struct sanei_sp1130n_settings *settings)
{
  struct timespec began, now;
  if (clock_gettime (CLOCK_MONOTONIC, &began))
    return SP_IO;
  unsigned sheets = 1;
  while (!h->session.exhausted)
    {
      if (atomic_load (&cancel_requested))
        return SP_CANCELLED;
      if (clock_gettime (CLOCK_MONOTONIC, &now))
        return SP_IO;
      if (sheets >= 100 || now.tv_sec - began.tv_sec > 120 ||
          (now.tv_sec - began.tv_sec == 120 && now.tv_nsec >= began.tv_nsec))
        {
          DBG (1, "first-sheet feeder drain exceeded its sheet/time limit\n");
          return SP_IO;
        }
      struct sanei_sp1130n_sheet discarded = { 0 };
      int status = sanei_sp1130n_scan_next (io, settings, h->token,
                                          &h->session, &discarded);
      unsigned expected = settings->source == SP_SOURCE_DUPLEX ? 2 : 1;
      if (status == SP_OK && discarded.count != expected)
        status = SP_IO;
      DBG (3, "first-sheet feeder drain: status=%d sides=%u\n",
           status, discarded.count);
      sanei_sp1130n_sheet_free (&discarded);
      if (!h->session.started)
        sanei_sp1130n_connection_idle (h->connection);
      if (status == SP_NO_DOCS)
        return SP_OK;
      if (status != SP_OK)
        return status;
      ++sheets;
    }
  return SP_OK;
}

SANE_Status
sane_sp1130n_start (SANE_Handle handle)
{
  pthread_mutex_lock (&state);
  struct handle *h = current;
  if (!handle || handle != h || h->closing)
    {
      pthread_mutex_unlock (&state);
      return SANE_STATUS_INVAL;
    }
  if (h->in_start)
    {
      pthread_mutex_unlock (&state);
      return SANE_STATUS_DEVICE_BUSY;
    }
  /* Clear the old request before cleanup; a new signal remains observable. */
  if (atomic_exchange (&cancel_requested, 0) && !h->cancel_reaped)
    reset_scan (h);
  h->cancel_reaped = 0;
  if (h->active && h->offset < h->sheet.page[h->side].size)
    {
      pthread_mutex_unlock (&state);
      return SANE_STATUS_DEVICE_BUSY;
    }
  if (h->active && h->side + 1 < h->sheet.count)
    {
      ++h->side;
      h->offset = 0;
      pthread_mutex_unlock (&state);
      return SANE_STATUS_GOOD;
    }
  struct sanei_sp1130n_settings settings;
  if (scan_settings (h, &settings) != SP_OK)
    {
      pthread_mutex_unlock (&state);
      return SANE_STATUS_INVAL;
    }
  sanei_sp1130n_sheet_free (&h->sheet);
  h->active = 0;
  h->side = 0;
  h->offset = 0;
  h->in_start = 1;
  struct sanei_sp1130n_io io =
    sanei_sp1130n_connection_io (h->connection, &cancel_requested);
  io.log_header = log_header;
  pthread_mutex_unlock (&state);
  int status = SP_IO;
  for (unsigned attempt = 0; attempt < 32; ++attempt)
    {
      if (atomic_load (&cancel_requested))
        {
          status = SP_CANCELLED;
          break;
        }
      status = sanei_sp1130n_scan_next (&io, &settings, h->token,
                                        &h->session, &h->sheet);
      DBG (3, "sheet acquisition: status=%d sides=%u\n", status,
           h->sheet.count);
      if (!h->session.started)
        sanei_sp1130n_connection_idle (h->connection);
      if (status != SP_OK)
        break;
      unsigned expected = settings.source == SP_SOURCE_DUPLEX ? 2 : 1;
      if (h->sheet.count != expected)
        {
          status = SP_IO;
          break;
        }
      if (!h->batch_scan)
        {
          status = drain_feeder (h, &io, &settings);
          if (status != SP_OK)
            break;
        }
      if (h->upright_loading && expected == 2)
        {
          struct sanei_sp1130n_page first = h->sheet.page[0];
          h->sheet.page[0] = h->sheet.page[1];
          h->sheet.page[1] = first;
        }
      unsigned kept = 0;
      for (unsigned side = 0; side < expected; ++side)
        {
          if (atomic_load (&cancel_requested))
            {
              status = SP_CANCELLED;
              break;
            }
          struct sanei_sp1130n_post_result result = { 0 };
          if (h->post.autocrop || h->post.deskew || h->skip_blank)
            status =
              sanei_sp1130n_postprocess (&h->sheet.page[side], &h->post,
                                         settings.dpi, &cancel_requested,
                                         &result);
          if (status != SP_OK)
            break;
          if (h->skip_blank && result.blank)
            {
              free (h->sheet.page[side].data);
              h->sheet.page[side] = (struct sanei_sp1130n_page)
              { 0 };
              continue;
            }
          struct sanei_sp1130n_image_options image = h->image;
          int logical_back = h->settings.source == SP_SOURCE_BACK ||
            (h->settings.source == SP_SOURCE_DUPLEX && side == 1);
          image.rotate180 = h->upright_loading !=
            (h->rotate_back && logical_back);
          status = sanei_sp1130n_image_process (&h->sheet.page[side], &image);
          if (status != SP_OK)
            break;
          if (kept != side)
            {
              h->sheet.page[kept] = h->sheet.page[side];
              h->sheet.page[side] = (struct sanei_sp1130n_page)
              { 0 };
            }
          ++kept;
        }
      if (status != SP_OK)
        break;
      h->sheet.count = kept;
      if (kept)
        break;
      sanei_sp1130n_sheet_free (&h->sheet);
      status = SP_IO;           /* A run of blank sheets must not create an unbounded feed loop. */
    }
  if (status != SP_OK)
    {
      finish_scan (h);
      if (status != SP_NO_DOCS)
        h->session.error = status;
      sanei_sp1130n_sheet_free (&h->sheet);
    }
  pthread_mutex_lock (&state);
  if (atomic_load (&cancel_requested))
    {
      /* Keep close waiting while cancelling a successfully acquired session. */
      pthread_mutex_unlock (&state);
      finish_scan (h);
      pthread_mutex_lock (&state);
      h->session = (struct sanei_sp1130n_session)
      { 0 };
      sanei_sp1130n_sheet_free (&h->sheet);
      h->cancel_reaped = 1;
      status = SP_CANCELLED;
    }
  h->active = status == SP_OK;
  h->in_start = 0;
  pthread_cond_broadcast (&idle);
  pthread_mutex_unlock (&state);
  return (SANE_Status) status;
}

SANE_Status
sane_sp1130n_read (SANE_Handle handle, SANE_Byte *data, SANE_Int max,
                   SANE_Int *length)
{
  if (!length)
    return SANE_STATUS_INVAL;
  *length = 0;
  if (!data || max <= 0)
    return SANE_STATUS_INVAL;
  pthread_mutex_lock (&state);
  struct handle *h = current;
  int status = SP_OK;
  if (!handle || handle != h || h->closing)
    {
      status = SP_INVALID;
      goto done;
    }
  if (atomic_load (&cancel_requested))
    {
      reap_cancellation (h);
      status = SP_CANCELLED;
      goto done;
    }
  if (h->in_start || !h->active)
    {
      status = SP_INVALID;
      goto done;
    }
  struct sanei_sp1130n_page *page = &h->sheet.page[h->side];
  size_t n = page->size - h->offset;
  if (!n)
    {
      status = SP_EOF;
      goto done;
    }
  if (n > (size_t) max)
    n = (size_t) max;
  memcpy (data, page->data + h->offset, n);
  h->offset += n;
  *length = (SANE_Int) n;
done:
  pthread_mutex_unlock (&state);
  return (SANE_Status) status;
}

SANE_Status
sane_sp1130n_set_io_mode (SANE_Handle h, SANE_Bool nonblocking)
{
  if (!h || h != current)
    return SANE_STATUS_INVAL;
  return nonblocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status
sane_sp1130n_get_select_fd (SANE_Handle h, SANE_Int *fd)
{
  if (!h || h != current || !fd)
    return SANE_STATUS_INVAL;
  return SANE_STATUS_UNSUPPORTED;
}
