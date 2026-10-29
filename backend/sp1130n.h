/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#ifndef SP1130N_H
#define SP1130N_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define SP_VID 0x05ca
#define SP_PID 0x031e
#define SP_PARAM_SIZE 260
#define SP_NVRAM_SIZE 288
#define SP_PAGE_LIMIT (128u * 1024u * 1024u)
#define SP_COMPRESSED_LIMIT (64u * 1024u * 1024u)

/* Values intentionally match SANE status codes. */
enum sanei_sp1130n_status
{
  SP_OK, SP_UNSUPPORTED, SP_CANCELLED, SP_BUSY, SP_INVALID, SP_EOF,
  SP_JAMMED, SP_NO_DOCS, SP_COVER_OPEN, SP_IO, SP_NOMEM, SP_ACCESS
};
struct sanei_sp1130n_io
{
  void *context;
  int (*bulk) (void *, int, uint8_t *, size_t, size_t *, unsigned);
  const atomic_bool *cancelled;
  int (*begin_command) (struct sanei_sp1130n_io *, uint8_t, uint8_t);
  int network;
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
};
struct sanei_sp1130n_session
{
  int own_lock, started, exhausted, error;
  unsigned sheets;
};
struct sanei_sp1130n_device
{
  char name[64];
};

uint16_t sanei_sp1130n_le16 (const uint8_t *);
uint32_t sanei_sp1130n_le32 (const uint8_t *);
void sanei_sp1130n_put16 (uint8_t *, uint16_t);
void sanei_sp1130n_put32 (uint8_t *, uint32_t);
void sanei_sp1130n_header (uint8_t[16], uint8_t, uint8_t, uint16_t, uint32_t);
int sanei_sp1130n_parameters (uint8_t[SP_PARAM_SIZE],
                              const struct sanei_sp1130n_settings *);
int sanei_sp1130n_network_parameters (uint8_t[SP_PARAM_SIZE],
                                      const struct sanei_sp1130n_settings *);
int sanei_sp1130n_scan_geometry (const struct sanei_sp1130n_settings *,
                                 struct sanei_sp1130n_geometry *);
int sanei_sp1130n_exact (struct sanei_sp1130n_io *, int, uint8_t *, size_t,
                         unsigned);
int sanei_sp1130n_command (struct sanei_sp1130n_io *, uint8_t, uint8_t,
                           uint16_t, uint32_t, uint8_t *, size_t);
int sanei_sp1130n_device_error (uint32_t);
const char *sanei_sp1130n_strstatus (int);
int sanei_sp1130n_probe (struct sanei_sp1130n_io *, uint8_t[108]);
/* Opaque read-only snapshot; leaves output unchanged on error. */
int sanei_sp1130n_read_nvram (struct sanei_sp1130n_io *, const uint8_t[40],
                              uint8_t[SP_NVRAM_SIZE]);
int sanei_sp1130n_scan_sheet (struct sanei_sp1130n_io *,
                              const struct sanei_sp1130n_settings *,
                              const uint8_t[40],
                              struct sanei_sp1130n_sheet *);
/* Zero-initialize sessions. LAN keeps the acquisition alive between sheets. */
int sanei_sp1130n_scan_next (struct sanei_sp1130n_io *,
                             const struct sanei_sp1130n_settings *,
                             const uint8_t[40],
                             struct sanei_sp1130n_session *,
                             struct sanei_sp1130n_sheet *);
int sanei_sp1130n_scan_finish (struct sanei_sp1130n_io *, const uint8_t[40],
                               struct sanei_sp1130n_session *);
int sanei_sp1130n_decode_jpeg (uint8_t *, size_t, unsigned, unsigned,
                               struct sanei_sp1130n_page *);
int sanei_sp1130n_jpeg_geometry (const uint8_t *, size_t, unsigned *,
                                 unsigned *);
void sanei_sp1130n_sheet_free (struct sanei_sp1130n_sheet *);

int sanei_sp1130n_usb_list (struct sanei_sp1130n_device **, size_t *);
int sanei_sp1130n_usb_open (const char *, struct sanei_sp1130n_usb **);
struct sanei_sp1130n_io sanei_sp1130n_usb_io (struct sanei_sp1130n_usb *,
                                              const atomic_bool *);
void sanei_sp1130n_usb_close (struct sanei_sp1130n_usb *);
/* Explicit IPv4 only; default TCP port 16888. No discovery or configuration writes. */
int sanei_sp1130n_net_open (const char *, struct sanei_sp1130n_net **);
struct sanei_sp1130n_io sanei_sp1130n_net_io (struct sanei_sp1130n_net *,
                                              const atomic_bool *);
void sanei_sp1130n_net_idle (struct sanei_sp1130n_net *);
void sanei_sp1130n_net_close (struct sanei_sp1130n_net *);
int sanei_sp1130n_connection_open (const char *,
                                   struct sanei_sp1130n_connection **);
struct sanei_sp1130n_io sanei_sp1130n_connection_io (
    struct sanei_sp1130n_connection *, const atomic_bool *);
void sanei_sp1130n_connection_idle (struct sanei_sp1130n_connection *);
void sanei_sp1130n_connection_close (struct sanei_sp1130n_connection *);
int sanei_sp1130n_token (uint8_t[40]);
#endif
