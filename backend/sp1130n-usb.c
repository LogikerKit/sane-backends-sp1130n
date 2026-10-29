/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#define _POSIX_C_SOURCE 200809L
#include "../include/sane/config.h"
#include "sp1130n.h"
#include <libusb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

struct sanei_sp1130n_usb
{
  libusb_context *context;
  libusb_device_handle *handle;
  int interface_number;
  uint8_t input, output;
};
static int
usb_status (int n)
{
  if (!n)
    return SP_OK;
  if (n == LIBUSB_ERROR_ACCESS)
    return SP_ACCESS;
  if (n == LIBUSB_ERROR_BUSY)
    return SP_BUSY;
  if (n == LIBUSB_ERROR_NO_MEM)
    return SP_NOMEM;
  return SP_IO;
}

int
sanei_sp1130n_usb_list (struct sanei_sp1130n_device **result, size_t *count)
{
  if (!result || !count)
    return SP_INVALID;
  *result = NULL;
  *count = 0;
  libusb_context *context = NULL;
  int rc = libusb_init (&context);
  if (rc)
    return usb_status (rc);
  libusb_device **devices = NULL;
  ssize_t n = libusb_get_device_list (context, &devices);
  if (n < 0)
    {
      libusb_exit (context);
      return usb_status ((int) n);
    }
  for (ssize_t i = 0; i < n; ++i)
    {
      struct libusb_device_descriptor d;
      if (libusb_get_device_descriptor (devices[i], &d) || d.idVendor != SP_VID
          || d.idProduct != SP_PID)
        continue;
      void *larger = realloc (*result, (*count + 1) * sizeof **result);
      if (!larger)
        {
          free (*result);
          *result = NULL;
          *count = 0;
          libusb_free_device_list (devices, 1);
          libusb_exit (context);
          return SP_NOMEM;
        }
      *result = larger;
      snprintf ((*result)[*count].name, sizeof (*result)[*count].name,
                "sp1130n:usb:%03u:%03u", libusb_get_bus_number (devices[i]),
                libusb_get_device_address (devices[i]));
      ++*count;
    }
  libusb_free_device_list (devices, 1);
  libusb_exit (context);
  return SP_OK;
}

int
sanei_sp1130n_usb_open (const char *name, struct sanei_sp1130n_usb **result)
{
  if (!name || !result)
    return SP_INVALID;
  *result = NULL;
  struct sanei_sp1130n_usb *u = calloc (1, sizeof *u);
  if (!u)
    return SP_NOMEM;
  u->interface_number = -1;
  int rc = libusb_init (&u->context);
  if (rc)
    {
      free (u);
      return usb_status (rc);
    }
  libusb_device **devices = NULL;
  ssize_t count = libusb_get_device_list (u->context, &devices);
  if (count < 0)
    {
      sanei_sp1130n_usb_close (u);
      return usb_status ((int) count);
    }
  int status = SP_INVALID;
  for (ssize_t i = 0; i < count; ++i)
    {
      struct libusb_device_descriptor d;
      char candidate[64];
      if (libusb_get_device_descriptor (devices[i], &d) || d.idVendor != SP_VID
          || d.idProduct != SP_PID)
        continue;
      snprintf (candidate, sizeof candidate, "sp1130n:usb:%03u:%03u",
                libusb_get_bus_number (devices[i]),
                libusb_get_device_address (devices[i]));
      if (strcmp (name, candidate))
        continue;
      status = usb_status (libusb_open (devices[i], &u->handle));
      if (status != SP_OK)
        break;
      struct libusb_config_descriptor *config = NULL;
      rc = libusb_get_active_config_descriptor (devices[i], &config);
      if (rc)
        {
          status = usb_status (rc);
          break;
        }
      int matches = 0, interface_number = -1;
      for (unsigned j = 0; j < config->bNumInterfaces; ++j)
        {
          const struct libusb_interface *interface = &config->interface[j];
          for (int k = 0; k < interface->num_altsetting; ++k)
            {
              const struct libusb_interface_descriptor *alt =
                &interface->altsetting[k];
              if (alt->bAlternateSetting != 0)
                continue;
              uint8_t input = 0, output = 0;
              unsigned inputs = 0, outputs = 0;
              for (unsigned e = 0; e < alt->bNumEndpoints; ++e)
                {
                  const struct libusb_endpoint_descriptor *ep =
                    &alt->endpoint[e];
                  if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                      LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                  if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                    {
                      input = ep->bEndpointAddress;
                      ++inputs;
                    }
                  else
                    {
                      output = ep->bEndpointAddress;
                      ++outputs;
                    }
                }
              if (inputs == 1 && outputs == 1)
                {
                  ++matches;
                  interface_number = alt->bInterfaceNumber;
                  u->input = input;
                  u->output = output;
                }
            }
        }
      libusb_free_config_descriptor (config);
      if (matches != 1)
        {
          status = SP_UNSUPPORTED;
          break;
        }
      status =
        usb_status (libusb_claim_interface (u->handle, interface_number));
      if (status == SP_OK)
        u->interface_number = interface_number;
      break;
    }
  libusb_free_device_list (devices, 1);
  if (status != SP_OK)
    {
      sanei_sp1130n_usb_close (u);
      return status;
    }
  *result = u;
  return SP_OK;
}

static int
bulk (void *context, int input, uint8_t *data, size_t size, size_t *actual,
      unsigned timeout)
{
  struct sanei_sp1130n_usb *u = context;
  int transferred = 0;
  int rc = libusb_bulk_transfer (u->handle, input ? u->input : u->output, data,
                                 (int) size, &transferred, timeout);
  *actual = transferred > 0 ? (size_t) transferred : 0;
  return usb_status (rc);
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
  if (u->handle)
    {
      if (u->interface_number >= 0)
        libusb_release_interface (u->handle, u->interface_number);
      libusb_close (u->handle);
    }
  if (u->context)
    libusb_exit (u->context);
  free (u);
}

int
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
        return SP_IO;
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
  return SP_OK;
}
