/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#define _POSIX_C_SOURCE 200809L
#include "../include/sane/config.h"
#include "../include/sane/sanei_usb.h"
#include "sp1130n.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

struct sanei_sp1130n_usb
{
  SANE_Int device;
  int blocked;
};

/* The shared helper has process-wide device state and a common timeout. */
static pthread_mutex_t usb_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct sanei_sp1130n_device *listed_devices;
static size_t listed_count;
static SANE_Status list_status;

static SANE_Status
attach_device (SANE_String_Const name)
{
  if (list_status != SANE_STATUS_GOOD)
    return list_status;
  SANE_Word vendor, product;
  SANE_Status status =
    sanei_usb_get_vendor_product_byname (name, &vendor, &product);
  if (status != SANE_STATUS_GOOD)
    return list_status = status;
  if (!sanei_sp1130n_usb_matches (vendor, product))
    return SANE_STATUS_GOOD;
  char candidate[sizeof listed_devices[0].name];
  int length = !strncmp (name, "libusb:", 7) ?
    snprintf (candidate, sizeof candidate, "sp1130n:usb:%s", name + 7) :
    snprintf (candidate, sizeof candidate, "sp1130n:%s", name);
  if (length < 0 || (size_t) length >= sizeof candidate)
    return list_status = SANE_STATUS_INVAL;
  if (listed_count >= SIZE_MAX / sizeof *listed_devices - 1)
    return list_status = SANE_STATUS_NO_MEM;
  void *larger = realloc (listed_devices,
                         (listed_count + 1) * sizeof *listed_devices);
  if (!larger)
    return list_status = SANE_STATUS_NO_MEM;
  listed_devices = larger;
  strcpy (listed_devices[listed_count].name, candidate);
  listed_devices[listed_count++].vendor = vendor;
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_sp1130n_usb_list (struct sanei_sp1130n_device **result, size_t *count)
{
  if (!result || !count)
    return SANE_STATUS_INVAL;
  *result = NULL;
  *count = 0;
  pthread_mutex_lock (&usb_mutex);
  sanei_usb_init ();
  sanei_usb_scan_devices ();
  listed_devices = NULL;
  listed_count = 0;
  list_status = SANE_STATUS_GOOD;
  SANE_Status status =
    sanei_usb_find_devices (SP_VID, SP_PID, attach_device);
  if (status == SANE_STATUS_GOOD && list_status == SANE_STATUS_GOOD)
    status = sanei_usb_find_devices (SP_FUJITSU_VID, SP_FUJITSU_PID,
                                    attach_device);
  if (status == SANE_STATUS_GOOD)
    status = list_status;
  if (status == SANE_STATUS_GOOD)
    {
      *result = listed_devices;
      *count = listed_count;
    }
  else
    free (listed_devices);
  listed_devices = NULL;
  listed_count = 0;
  sanei_usb_exit ();
  pthread_mutex_unlock (&usb_mutex);
  return status;
}

SANE_Status
sanei_sp1130n_usb_open (const char *name, struct sanei_sp1130n_usb **result)
{
  if (!name || !result)
    return SANE_STATUS_INVAL;
  *result = NULL;
  const char *local = !strncmp (name, "sp1130n:", 8) ? name + 8 : name;
  char device_name[80];
  int length = !strncmp (local, "usb:", 4) ?
    snprintf (device_name, sizeof device_name, "libusb:%s", local + 4) :
    snprintf (device_name, sizeof device_name, "%s", local);
  if (length < 0 || (size_t) length >= sizeof device_name)
    return SANE_STATUS_INVAL;
  struct sanei_sp1130n_usb *u = calloc (1, sizeof *u);
  if (!u)
    return SANE_STATUS_NO_MEM;
  u->device = -1;
  pthread_mutex_lock (&usb_mutex);
  sanei_usb_init ();
  sanei_usb_scan_devices ();
  SANE_Word vendor, product;
  SANE_Status status =
    sanei_usb_get_vendor_product_byname (device_name, &vendor, &product);
  if (status == SANE_STATUS_GOOD && !sanei_sp1130n_usb_matches (vendor, product))
    status = SANE_STATUS_INVAL;
  if (status == SANE_STATUS_GOOD)
    status = sanei_usb_open (device_name, &u->device);
  if (status == SANE_STATUS_GOOD)
    {
      SANE_Int input = sanei_usb_get_endpoint (u->device,
          USB_DIR_IN | USB_ENDPOINT_TYPE_BULK);
      SANE_Int output = sanei_usb_get_endpoint (u->device,
          USB_DIR_OUT | USB_ENDPOINT_TYPE_BULK);
      if (input <= 0 || input > 255 || output <= 0 || output > 255 ||
          !(input & USB_DIR_IN) || !(input & USB_ENDPOINT_ADDRESS_MASK) ||
          (output & USB_DIR_IN) || !(output & USB_ENDPOINT_ADDRESS_MASK))
        status = SANE_STATUS_UNSUPPORTED;
    }
  pthread_mutex_unlock (&usb_mutex);
  if (status != SANE_STATUS_GOOD)
    {
      sanei_sp1130n_usb_close (u);
      return status;
    }
  *result = u;
  return SANE_STATUS_GOOD;
}

static SANE_Status
bulk (void *context, int input, uint8_t *data, size_t size, size_t *actual,
      unsigned timeout)
{
  struct sanei_sp1130n_usb *u = context;
  *actual = 0;
  if (u->blocked)
    return SANE_STATUS_IO_ERROR;
  if (!data || !size || size > INT_MAX || !timeout)
    return SANE_STATUS_INVAL;
  unsigned bounded = timeout > INT_MAX ? INT_MAX : timeout;
  size_t transferred = size;
  pthread_mutex_lock (&usb_mutex);
  sanei_usb_set_timeout ((SANE_Int) bounded);
  SANE_Status status = input ?
    sanei_usb_read_bulk (u->device, data, &transferred) :
    sanei_usb_write_bulk (u->device, data, &transferred);
  pthread_mutex_unlock (&usb_mutex);
  /* Failed helper calls discard partial counts: never guess a packet boundary. */
  if (status != SANE_STATUS_GOOD || !transferred || transferred > size)
    {
      u->blocked = 1;
      return status == SANE_STATUS_GOOD || status == SANE_STATUS_EOF ? SANE_STATUS_IO_ERROR : status;
    }
  *actual = transferred;
  return SANE_STATUS_GOOD;
}

struct sanei_sp1130n_io
sanei_sp1130n_usb_io (struct sanei_sp1130n_usb *u, const atomic_bool *cancelled)
{
  return (struct sanei_sp1130n_io)
  {.context = u,.bulk = bulk,.cancelled = cancelled };
}

void
sanei_sp1130n_usb_close (struct sanei_sp1130n_usb *u)
{
  if (!u)
    return;
  pthread_mutex_lock (&usb_mutex);
  if (u->device >= 0)
    sanei_usb_close (u->device);
  sanei_usb_exit ();
  pthread_mutex_unlock (&usb_mutex);
  free (u);
}

SANE_Status
sanei_sp1130n_token (uint8_t token[40])
{
  uint8_t random[16];
  size_t done = 0;
  while (done < sizeof random)
    {
      ssize_t n = getrandom (random + done, sizeof random - done, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return SANE_STATUS_IO_ERROR;
      done += (size_t) n;
    }
  random[6] = (random[6] & 15) | 64;
  random[8] = (random[8] & 63) | 128;
  memset (token, 0, 40);
  snprintf ((char *) token, 40,
            "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            random[0], random[1], random[2], random[3], random[4], random[5],
            random[6], random[7], random[8], random[9], random[10], random[11],
            random[12], random[13], random[14], random[15]);
  return SANE_STATUS_GOOD;
}
