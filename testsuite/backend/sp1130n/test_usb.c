/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
 * Copyright (c) 2026 sp1130n-open contributors.
 * Authored shared-USB adapter tests; no scanner captures.
 */
#include "sp1130n.h"
#include "sane/sanei_usb.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned references, opened, closed, transfers, finds, scans;
static SANE_Status open_status, transfer_status, lookup_status, find_status;
static size_t transfer_size;
static SANE_Int last_timeout, input_endpoint, output_endpoint;
static int last_input, no_devices;
static const char *ricoh_name;

void
sanei_usb_init (void)
{
  ++references;
}

void
sanei_usb_exit (void)
{
  assert (references);
  --references;
}

void
sanei_usb_scan_devices (void)
{
  assert (references);
  ++scans;
}

SANE_Status
sanei_usb_get_vendor_product_byname (SANE_String_Const name,
                                    SANE_Word *vendor, SANE_Word *product)
{
  assert (references);
  if (lookup_status != SANE_STATUS_GOOD)
    return lookup_status;
  if (!strcmp (name, ricoh_name))
    {
      *vendor = SP_VID;
      *product = SP_PID;
    }
  else if (!strcmp (name, "libusb:001:002"))
    {
      *vendor = SP_FUJITSU_VID;
      *product = SP_FUJITSU_PID;
    }
  else
    {
      *vendor = 1;
      *product = 2;
    }
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_usb_find_devices (SANE_Int vendor, SANE_Int product,
                       SANE_Status (*attach) (SANE_String_Const))
{
  assert (references);
  ++finds;
  if (find_status != SANE_STATUS_GOOD)
    return find_status;
  if (!no_devices)
    {
      if (vendor == SP_VID && product == SP_PID)
        attach (ricoh_name);
      else
        {
          assert (vendor == SP_FUJITSU_VID && product == SP_FUJITSU_PID);
          attach ("libusb:001:002");
        }
    }
  /* Like the real helper, callback failure is not returned by this function. */
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_usb_open (SANE_String_Const name, SANE_Int *device)
{
  assert (references && !strcmp (name, "libusb:001:002"));
  if (open_status != SANE_STATUS_GOOD)
    return open_status;
  *device = 7;
  ++opened;
  return SANE_STATUS_GOOD;
}

SANE_Int
sanei_usb_get_endpoint (SANE_Int device, SANE_Int type)
{
  assert (device == 7 && opened > closed);
  return type & USB_DIR_IN ? input_endpoint : output_endpoint;
}

void
sanei_usb_close (SANE_Int device)
{
  assert (device == 7 && references && opened > closed);
  ++closed;
}

void
sanei_usb_set_timeout (SANE_Int timeout)
{
  assert (references && timeout > 0);
  last_timeout = timeout;
}

SANE_Status
sanei_usb_read_bulk (SANE_Int device, SANE_Byte *buffer, size_t *size)
{
  assert (device == 7 && buffer && *size);
  ++transfers;
  last_input = 1;
  if (transfer_size <= *size)
    memset (buffer, 0x5a, transfer_size);
  *size = transfer_size;
  return transfer_status;
}

SANE_Status
sanei_usb_write_bulk (SANE_Int device, const SANE_Byte *buffer, size_t *size)
{
  assert (device == 7 && buffer && *size);
  ++transfers;
  last_input = 0;
  *size = transfer_size;
  return transfer_status;
}

static void
reset (void)
{
  assert (!references && opened == closed);
  references = opened = closed = transfers = finds = scans = 0;
  open_status = transfer_status = lookup_status = find_status = SANE_STATUS_GOOD;
  transfer_size = 3;
  input_endpoint = 0x81;
  output_endpoint = 0x02;
  no_devices = 0;
  ricoh_name = "libusb:001:003";
}

static void
discovery (void)
{
  reset ();
  struct sanei_sp1130n_device *devices = NULL;
  size_t count = 99;
  assert (sanei_sp1130n_usb_list (&devices, &count) == SANE_STATUS_GOOD);
  assert (count == 2 && finds == 2 && scans == 1 && !references);
  assert (!strcmp (devices[0].name, "sp1130n:usb:001:003"));
  assert (devices[0].vendor == SP_VID);
  assert (!strcmp (devices[1].name, "sp1130n:usb:001:002"));
  assert (devices[1].vendor == SP_FUJITSU_VID);
  free (devices);
  no_devices = 1;
  assert (sanei_sp1130n_usb_list (&devices, &count) == SANE_STATUS_GOOD);
  assert (!devices && !count && !references);
  no_devices = 0;
  lookup_status = SANE_STATUS_IO_ERROR;
  assert (sanei_sp1130n_usb_list (&devices, &count) == SANE_STATUS_IO_ERROR);
  assert (!devices && !count && !references);
  lookup_status = SANE_STATUS_GOOD;
  find_status = SANE_STATUS_NO_MEM;
  assert (sanei_sp1130n_usb_list (&devices, &count) == SANE_STATUS_NO_MEM);
  assert (!devices && !count && !references);
  find_status = SANE_STATUS_GOOD;
  ricoh_name = "libusb:123456789012345678901234567890123456789012345678901234567890";
  assert (sanei_sp1130n_usb_list (&devices, &count) == SANE_STATUS_INVAL);
  assert (!devices && !count && !references);
}

static void
opening (void)
{
  struct sanei_sp1130n_usb *usb;
  reset ();
  assert (sanei_sp1130n_usb_open ("sp1130n:usb:001:002", &usb) == SANE_STATUS_GOOD);
  assert (references == 1 && opened == 1);
  struct sanei_sp1130n_device *devices;
  size_t count;
  assert (sanei_sp1130n_usb_list (&devices, &count) == SANE_STATUS_GOOD);
  assert (references == 1 && !closed);
  free (devices);
  sanei_sp1130n_usb_close (usb);
  assert (!references && closed == 1);
  sanei_sp1130n_usb_close (NULL);

  SANE_Status failures[] = {
    SANE_STATUS_ACCESS_DENIED, SANE_STATUS_DEVICE_BUSY, SANE_STATUS_INVAL
  };
  for (unsigned i = 0; i < sizeof failures / sizeof failures[0]; ++i)
    {
      reset ();
      open_status = failures[i];
      assert (sanei_sp1130n_usb_open ("usb:001:002", &usb) == (int) failures[i]);
      assert (!usb && !references && !opened && !closed);
    }
  reset ();
  assert (sanei_sp1130n_usb_open ("sp1130n:usb:999:999", &usb) == SANE_STATUS_INVAL);
  assert (!usb && !opened && !references);
  for (unsigned i = 0; i < 4; ++i)
    {
      reset ();
      if (i == 0)
        input_endpoint = 0;
      else if (i == 1)
        output_endpoint = 0x82;
      else if (i == 2)
        input_endpoint = -1;
      else
        output_endpoint = 256;
      assert (sanei_sp1130n_usb_open ("usb:001:002", &usb) == SANE_STATUS_UNSUPPORTED);
      assert (!usb && !references && opened == 1 && closed == 1);
    }
}

static void
bulk_transfers (void)
{
  reset ();
  struct sanei_sp1130n_usb *usb;
  assert (sanei_sp1130n_usb_open ("usb:001:002", &usb) == SANE_STATUS_GOOD);
  atomic_bool cancelled = ATOMIC_VAR_INIT (0);
  struct sanei_sp1130n_io io = sanei_sp1130n_usb_io (usb, &cancelled);
  assert (io.cancelled == &cancelled && !io.network);
  assert (!io.recover_inquiry && !io.recover_image && !io.image_recovery_done);
  uint8_t data[8] = { 0 };
  size_t actual;
  assert (io.bulk (io.context, 1, data, sizeof data, &actual, 123) == SANE_STATUS_GOOD);
  assert (actual == 3 && data[0] == 0x5a && last_input && last_timeout == 123);
  assert (io.bulk (io.context, 0, data, sizeof data, &actual, 456) == SANE_STATUS_GOOD);
  assert (actual == 3 && !last_input && last_timeout == 456);
  assert (io.bulk (io.context, 1, data, sizeof data, &actual, UINT_MAX) == SANE_STATUS_GOOD);
  assert (last_timeout == INT_MAX);
  unsigned before = transfers;
  assert (io.bulk (io.context, 1, NULL, sizeof data, &actual, 123) == SANE_STATUS_INVAL);
  assert (!actual && transfers == before);
  assert (io.bulk (io.context, 1, data, 0, &actual, 123) == SANE_STATUS_INVAL);
  assert (io.bulk (io.context, 1, data, sizeof data, &actual, 0) == SANE_STATUS_INVAL);
  assert (transfers == before);
  sanei_sp1130n_usb_close (usb);

  for (unsigned scenario = 0; scenario < 5; ++scenario)
    {
      reset ();
      assert (sanei_sp1130n_usb_open ("usb:001:002", &usb) == SANE_STATUS_GOOD);
      io = sanei_sp1130n_usb_io (usb, NULL);
      if (scenario == 0)
        transfer_status = SANE_STATUS_IO_ERROR;
      else if (scenario == 1)
        transfer_status = SANE_STATUS_EOF;
      else if (scenario == 2)
        transfer_size = 0;
      else if (scenario == 3)
        transfer_size = sizeof data + 1;
      else
        transfer_status = SANE_STATUS_ACCESS_DENIED;
      int expected = scenario == 4 ? SANE_STATUS_ACCESS_DENIED : SANE_STATUS_IO_ERROR;
      assert (io.bulk (io.context, scenario != 4, data, sizeof data,
                      &actual, 123) == expected);
      assert (!actual && transfers == 1);
      assert (io.bulk (io.context, 0, data, sizeof data, &actual, 123) == SANE_STATUS_IO_ERROR);
      assert (!actual && transfers == 1);
      sanei_sp1130n_usb_close (usb);
      assert (!references && closed == 1);
    }
}

int
main (void)
{
  discovery ();
  opening ();
  bulk_transfers ();
  puts ("Shared USB adapter tests passed: discovery, lifecycle, transfers and latched errors.");
  return 0;
}
