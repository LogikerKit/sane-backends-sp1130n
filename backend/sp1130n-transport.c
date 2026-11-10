/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#include "../include/sane/config.h"
#include "sp1130n.h"
#include <stdlib.h>
#include <string.h>

struct sanei_sp1130n_connection
{
  struct sanei_sp1130n_usb *usb;
  struct sanei_sp1130n_net *net;
};
SANE_Status
sanei_sp1130n_connection_open (const char *name,
                               struct sanei_sp1130n_connection **result)
{
  if (!name || !result)
    return SANE_STATUS_INVAL;
  *result = NULL;
  const char *local = !strncmp (name, "sp1130n:", 8) ? name + 8 : name;
  struct sanei_sp1130n_connection *c = calloc (1, sizeof *c);
  if (!c)
    return SANE_STATUS_NO_MEM;
  SANE_Status status =
    !strncmp (local, "tcp:", 4) ? sanei_sp1130n_net_open (name,
                                                          &c->
                                                          net) :
    sanei_sp1130n_usb_open (name, &c->usb);
  if (status != SANE_STATUS_GOOD)
    {
      free (c);
      return status;
    }
  *result = c;
  return SANE_STATUS_GOOD;
}

struct sanei_sp1130n_io
sanei_sp1130n_connection_io (struct sanei_sp1130n_connection *c,
                             const atomic_bool *cancelled)
{
  return c->net ? sanei_sp1130n_net_io (c->net,
                                        cancelled) : sanei_sp1130n_usb_io (c->
                                                                           usb,
                                                                           cancelled);
}

void
sanei_sp1130n_connection_idle (struct sanei_sp1130n_connection *c)
{
  /* Keep the USB claim, but release an idle scanner TCP session. */
  if (c)
    sanei_sp1130n_net_idle (c->net);
}

void
sanei_sp1130n_connection_close (struct sanei_sp1130n_connection *c)
{
  if (!c)
    return;
  sanei_sp1130n_net_close (c->net);
  if (c->usb)
    sanei_sp1130n_usb_close (c->usb);
  free (c);
}
