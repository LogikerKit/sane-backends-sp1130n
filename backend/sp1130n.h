/* SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#ifndef SP1130N_H
#define SP1130N_H
#include "../include/sane/sane.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define SP_VID 0x05ca
#define SP_PID 0x031e
#define SP_FUJITSU_VID 0x04c5
#define SP_FUJITSU_PID 0x1627
#define SP_PARAM_SIZE 260
#define SP_NVRAM_SIZE 288
#define SP_PAGE_LIMIT (128u * 1024u * 1024u)
#define SP_COMPRESSED_LIMIT (64u * 1024u * 1024u)

static inline int
sanei_sp1130n_usb_matches (uint16_t vendor, uint16_t product)
{
  return (vendor == SP_VID && product == SP_PID)
    || (vendor == SP_FUJITSU_VID && product == SP_FUJITSU_PID);
}

struct sanei_sp1130n_io
{
  void *context;
  SANE_Status (*bulk) (void *, int, uint8_t *, size_t, size_t *, unsigned);
  const atomic_bool *cancelled;
  SANE_Status (*begin_command) (struct sanei_sp1130n_io *, uint8_t, uint8_t);
  int network;
  SANE_Status (*retain_image) (struct sanei_sp1130n_io *);
  int (*closed_without_reply) (struct sanei_sp1130n_io *);
  /* USB-only: finish a known inquiry after timeout, never replay its command. */
  SANE_Status (*recover_inquiry) (void *, uint8_t *, size_t, unsigned);
  /* USB-only: read/discard a known image tail while blocked, then commit. */
  SANE_Status (*recover_image) (void *, uint8_t *, size_t, unsigned);
  SANE_Status (*image_recovery_done) (void *, int);
  /* LAN-only: discard the validated current payload tail before cancellation. */
  SANE_Status (*cancel_image_tail) (void *, size_t, unsigned);
  /* Fixed status headers only: never tokens, identifiers or image payloads. */
  void (*log_header) (const char *, SANE_Status, const uint8_t *, size_t);
};
struct sanei_sp1130n_usb;
struct sanei_sp1130n_net;
struct sanei_sp1130n_connection;
enum sanei_sp1130n_source
{
  SP_SOURCE_FRONT = 0, SP_SOURCE_DUPLEX = 1, SP_SOURCE_BACK = 2
};
struct sanei_sp1130n_settings
{
  unsigned dpi;
  unsigned width_mm;
  unsigned height_mm;
  enum sanei_sp1130n_source source;
  unsigned left_mm, top_mm;
  unsigned scan_width_mm, scan_height_mm;       /* Zero extends to the paper edge. */
};
struct sanei_sp1130n_geometry
{
  unsigned width, height, left, top, page_width, page_height;
};
struct sanei_sp1130n_page
{
  uint8_t *data;
  size_t size;
  unsigned width, height;
};
struct sanei_sp1130n_sheet
{
  struct sanei_sp1130n_page page[2];    /* Duplex: front then back. Simplex: selected side in page[0]. */
  unsigned count;
  int encoded;                       /* Internal LAN queue: JPEG rather than RGB. */
};
struct sanei_sp1130n_session
{
  /* USB event 2 continues the running acquisition; STOP precedes any unlock. */
  int own_lock, started, stop_pending, exhausted;
  SANE_Status error;
  unsigned sheets;
};
struct sanei_sp1130n_device
{
  char name[64];
  uint16_t vendor;
};

uint16_t sanei_sp1130n_le16 (const uint8_t *);
uint32_t sanei_sp1130n_le32 (const uint8_t *);
void sanei_sp1130n_put16 (uint8_t *, uint16_t);
void sanei_sp1130n_put32 (uint8_t *, uint32_t);
void sanei_sp1130n_header (uint8_t[16], uint8_t, uint8_t, uint16_t, uint32_t);
SANE_Status sanei_sp1130n_parameters (uint8_t[SP_PARAM_SIZE],
                              const struct sanei_sp1130n_settings *);
SANE_Status sanei_sp1130n_network_parameters (uint8_t[SP_PARAM_SIZE],
                                      const struct sanei_sp1130n_settings *);
SANE_Status sanei_sp1130n_scan_geometry (const struct sanei_sp1130n_settings *,
                                 struct sanei_sp1130n_geometry *);
SANE_Status sanei_sp1130n_exact (struct sanei_sp1130n_io *, int, uint8_t *, size_t,
                         unsigned);
SANE_Status sanei_sp1130n_command (struct sanei_sp1130n_io *, uint8_t, uint8_t,
                           uint16_t, uint32_t, uint8_t *, size_t);
SANE_Status sanei_sp1130n_device_error (uint32_t);
const char *sanei_sp1130n_strstatus (SANE_Status);
SANE_Status sanei_sp1130n_probe (struct sanei_sp1130n_io *, uint8_t[108]);
/* Opaque read-only snapshot; leaves output unchanged on error. */
SANE_Status sanei_sp1130n_read_nvram (struct sanei_sp1130n_io *, const uint8_t[40],
                              uint8_t[SP_NVRAM_SIZE]);
SANE_Status sanei_sp1130n_scan_sheet (struct sanei_sp1130n_io *,
                              const struct sanei_sp1130n_settings *,
                              const uint8_t[40],
                              struct sanei_sp1130n_sheet *);
/* Zero-initialize sessions. LAN streams persist; USB follows completion events. */
SANE_Status sanei_sp1130n_scan_next (struct sanei_sp1130n_io *,
                             const struct sanei_sp1130n_settings *,
                             const uint8_t[40],
                             struct sanei_sp1130n_session *,
                             struct sanei_sp1130n_sheet *);
/* LAN-only acquisition without pixel decoding; both sides remain validated
 * and decoded by the consumer before selecting a simplex output side. */
SANE_Status sanei_sp1130n_scan_next_encoded (struct sanei_sp1130n_io *,
                                     const struct sanei_sp1130n_settings *,
                                     const uint8_t[40],
                                     struct sanei_sp1130n_session *,
                                     struct sanei_sp1130n_sheet *);
SANE_Status sanei_sp1130n_scan_finish (struct sanei_sp1130n_io *, const uint8_t[40],
                               struct sanei_sp1130n_session *);
SANE_Status sanei_sp1130n_decode_jpeg (uint8_t *, size_t, unsigned, unsigned,
                               struct sanei_sp1130n_page *);
SANE_Status sanei_sp1130n_jpeg_geometry (const uint8_t *, size_t, unsigned *,
                                 unsigned *);
void sanei_sp1130n_sheet_free (struct sanei_sp1130n_sheet *);

SANE_Status sanei_sp1130n_usb_list (struct sanei_sp1130n_device **, size_t *);
SANE_Status sanei_sp1130n_usb_open (const char *, struct sanei_sp1130n_usb **);
struct sanei_sp1130n_io sanei_sp1130n_usb_io (struct sanei_sp1130n_usb *,
                                              const atomic_bool *);
void sanei_sp1130n_usb_close (struct sanei_sp1130n_usb *);
/* Explicit IPv4 only; default TCP port 16888. No discovery or configuration writes. */
SANE_Status sanei_sp1130n_net_open (const char *, struct sanei_sp1130n_net **);
struct sanei_sp1130n_io sanei_sp1130n_net_io (struct sanei_sp1130n_net *,
                                              const atomic_bool *);
void sanei_sp1130n_net_idle (struct sanei_sp1130n_net *);
void sanei_sp1130n_net_close (struct sanei_sp1130n_net *);
SANE_Status sanei_sp1130n_connection_open (const char *,
                                   struct sanei_sp1130n_connection **);
struct sanei_sp1130n_io sanei_sp1130n_connection_io (
    struct sanei_sp1130n_connection *, const atomic_bool *);
void sanei_sp1130n_connection_idle (struct sanei_sp1130n_connection *);
void sanei_sp1130n_connection_close (struct sanei_sp1130n_connection *);
SANE_Status sanei_sp1130n_token (uint8_t[40]);
#endif
