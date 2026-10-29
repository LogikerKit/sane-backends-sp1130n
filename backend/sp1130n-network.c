/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 sp1130n-open contributors.
 * Ricoh SP-1130N scanner backend.
 * See sp1130n-license.txt for license terms.
 */
#define _POSIX_C_SOURCE 200809L
#include "../include/sane/config.h"
#include "sp1130n.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct sanei_sp1130n_net
{
  struct sockaddr_in address;
  int fd, blocks, inquiry;
  size_t received;
  const atomic_bool *cancelled;
};

static uint64_t
now_ms (void)
{
  struct timespec t;
  if (clock_gettime (CLOCK_MONOTONIC, &t))
    return 0;
  return (uint64_t) t.tv_sec * 1000 + (unsigned long) t.tv_nsec / 1000000;
}

static int
stopped (const struct sanei_sp1130n_net *n)
{
  return n->cancelled && atomic_load (n->cancelled);
}

static void
disconnect (struct sanei_sp1130n_net *n)
{
  if (n->fd >= 0)
    {
      shutdown (n->fd, SHUT_RDWR);
      close (n->fd);
      n->fd = -1;
    }
  n->blocks = 0;
}

static int
wait_ready (struct sanei_sp1130n_net *n, short events, uint64_t deadline)
{
  for (;;)
    {
      if (stopped (n))
        return SP_CANCELLED;
      uint64_t now = now_ms ();
      if (now >= deadline)
        return SP_IO;
      int timeout = deadline - now > 50 ? 50 : (int) (deadline - now);
      struct pollfd p = { n->fd, events, 0 };
      int rc = poll (&p, 1, timeout);
      if (rc < 0 && errno == EINTR)
        continue;
      if (rc < 0)
        return SP_IO;
      if (!rc)
        continue;
      if (p.revents & events ||
          (events == POLLIN && p.revents & (POLLERR | POLLHUP)))
        return SP_OK;
      if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
        return SP_IO;
    }
}

int
sanei_sp1130n_net_open (const char *name, struct sanei_sp1130n_net **result)
{
  if (!name || !result)
    return SP_INVALID;
  *result = NULL;
  if (!strncmp (name, "sp1130n:", 8))
    name += 8;
  if (strncmp (name, "tcp:", 4))
    return SP_INVALID;
  name += 4;
  const char *colon = strchr (name, ':');
  size_t length = colon ? (size_t) (colon - name) : strlen (name);
  if (!length || length >= INET_ADDRSTRLEN)
    return SP_INVALID;
  char host[INET_ADDRSTRLEN];
  memcpy (host, name, length);
  host[length] = '\0';
  struct sockaddr_in address = {.sin_family = AF_INET,.sin_port =
      htons (16888)
  };
  if (inet_pton (AF_INET, host, &address.sin_addr) != 1)
    return SP_INVALID;
  uint32_t ip = ntohl (address.sin_addr.s_addr);
  if ((ip >> 24) == 0 || (ip >> 24) >= 224)
    return SP_INVALID;
  if (colon)
    {
      unsigned port = 0;
      const char *p = colon + 1;
      if (!*p)
        return SP_INVALID;
      for (; *p; ++p)
        {
          if (*p < '0' || *p > '9' || port > 6553)
            return SP_INVALID;
          port = port * 10 + (unsigned) (*p - '0');
          if (port > 65535)
            return SP_INVALID;
        }
      if (!port)
        return SP_INVALID;
      address.sin_port = htons ((uint16_t) port);
    }
  struct sanei_sp1130n_net *n = calloc (1, sizeof *n);
  if (!n)
    return SP_NOMEM;
  n->fd = -1;
  n->address = address;
  *result = n;
  return SP_OK;
}

static int
begin_command (struct sanei_sp1130n_io *io, uint8_t op, uint8_t sub)
{
  struct sanei_sp1130n_net *n = io->context;
  n->cancelled = io->cancelled;
  if (stopped (n))
    return SP_CANCELLED;
  /* All sheets in one acquisition share the image stream; controls reconnect. */
  int blocks = op == 0x28 && sub == 4;
  if (blocks && n->blocks && n->fd >= 0)
    return SP_OK;
  disconnect (n);
  n->inquiry = op == 0x12 && sub == 0x7a;
  n->received = 0;
  n->fd = socket (AF_INET, SOCK_STREAM, 0);
  if (n->fd < 0)
    return SP_IO;
  if (fcntl (n->fd, F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl (n->fd, F_SETFL, O_NONBLOCK) < 0)
    goto error;
  int one = 1;
  if (setsockopt (n->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one))
    goto error;
  int rc =
    connect (n->fd, (struct sockaddr *) &n->address, sizeof n->address);
  if (rc < 0 && errno != EINPROGRESS)
    goto error;
  if (rc < 0)
    {
      int status = wait_ready (n, POLLOUT, now_ms () + 5000);
      if (status != SP_OK)
        {
          disconnect (n);
          return status;
        }
      int error = 0;
      socklen_t size = sizeof error;
      if (getsockopt (n->fd, SOL_SOCKET, SO_ERROR, &error, &size) || error)
        goto error;
    }
  n->blocks = blocks;
  return SP_OK;
error:
  disconnect (n);
  return SP_IO;
}

static int
transfer (void *context, int input, uint8_t *data, size_t size,
          size_t *actual, unsigned timeout)
{
  struct sanei_sp1130n_net *n = context;
  if (!actual)
    return SP_INVALID;
  *actual = 0;
  if (n->fd < 0 || (!data && size) || !timeout || size > INT_MAX)
    return SP_INVALID;
  uint64_t deadline = now_ms () + timeout;
  for (;;)
    {
      int status = wait_ready (n, input ? POLLIN : POLLOUT, deadline);
      if (status != SP_OK)
        {
          disconnect (n);
          return status;
        }
      ssize_t count = input ? recv (n->fd, data, size, 0) :
        send (n->fd, data, size, MSG_NOSIGNAL);
      if (count < 0
          && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (count <= 0)
        {
          int status = input && count == 0 ? SP_EOF : SP_IO;
          disconnect (n);
          return status;
        }
      *actual = (size_t) count;
      /* Refuse any encryption indication; negotiation is not implemented. */
      if (input && n->inquiry)
        {
          if (n->received <= 30 && (size_t) count > 30 - n->received &&
              data[30 - n->received] != 0)
            {
              disconnect (n);
              return SP_UNSUPPORTED;
            }
          n->received += (size_t) count;
        }
      return SP_OK;
    }
}

struct sanei_sp1130n_io
sanei_sp1130n_net_io (struct sanei_sp1130n_net *n,
                      const atomic_bool *cancelled)
{
  n->cancelled = cancelled;
  return (struct sanei_sp1130n_io)
  {.context = n,.bulk = transfer,.cancelled = cancelled,
    .begin_command = begin_command,.network = 1
  };
}

void
sanei_sp1130n_net_idle (struct sanei_sp1130n_net *n)
{
  if (n)
    disconnect (n);
}

void
sanei_sp1130n_net_close (struct sanei_sp1130n_net *n)
{
  if (!n)
    return;
  disconnect (n);
  free (n);
}
