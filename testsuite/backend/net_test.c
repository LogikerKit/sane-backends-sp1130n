/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE Project

   This file is part of the SANE package and is distributed under the
   terms of the GNU General Public License version 3 or later.

   net backend tests. */

#include "../../backend/net.c"

#include <stdio.h>
#include <string.h>

SANE_String_Const
sane_strstatus (SANE_Status status)
{
  (void) status;
  return "";
}

static int failures;

static void
test_parse_host_port_case (const char *name, const char *entry_str,
			   int expected_ret, const char *expected_host,
			   const char *expected_next)
{
  char entry[128];
  char *host = NULL, *next = NULL;
  int ret;

  if (snprintf (entry, sizeof (entry), "%s", entry_str) >= (int) sizeof (entry))
    {
      fprintf (stderr, "%s: entry too long\n", name);
      failures++;
      return;
    }

  ret = parse_host_port (entry, &host, &next);

  if (ret != expected_ret)
    {
      fprintf (stderr, "%s: \"%s\": expected return %d, got %d\n", name,
	       entry_str, expected_ret, ret);
      failures++;
      return;
    }

  if (expected_ret < 0)
    return;

  if (strcmp (host, expected_host))
    {
      fprintf (stderr, "%s: \"%s\": expected host \"%s\", got \"%s\"\n", name,
	       entry_str, expected_host, host);
      failures++;
    }

  if (strcmp (next, expected_next))
    {
      fprintf (stderr, "%s: \"%s\": expected next \"%s\", got \"%s\"\n", name,
	       entry_str, expected_next, next);
      failures++;
    }
}

static void
test_parse_host_port (void)
{
  test_parse_host_port_case ("plain host", "localhost", 6566, "localhost", "");
  test_parse_host_port_case ("host with port", "localhost:6566", 6566,
			     "localhost", "");
  test_parse_host_port_case ("host with port 1", "host:1", 1, "host", "");
  test_parse_host_port_case ("host with port 65535", "host:65535", 65535,
			     "host", "");
  test_parse_host_port_case ("host with port and next entry",
			     "host:7000:hostb", 7000, "host", "hostb");
  test_parse_host_port_case ("host with non-numeric suffix", "host:abc", 6566,
			     "host", "abc");
  test_parse_host_port_case ("host with digit prefix suffix", "host:6566x",
			     6566, "host", "6566x");
  test_parse_host_port_case ("host with empty next entry", "host:", 6566,
			     "host", "");
  test_parse_host_port_case ("bracketed v6", "[::1]", 6566, "::1", "");
  test_parse_host_port_case ("bracketed v6 with port", "[::1]:6566", 6566,
			     "::1", "");
  test_parse_host_port_case ("bracketed v6 with port and next entry",
			     "[::1]:16566:localhost", 16566, "::1",
			     "localhost");
  test_parse_host_port_case ("bracketed full v6 with port",
			     "[2001:db8::1]:6566", 6566, "2001:db8::1", "");
  test_parse_host_port_case ("unclosed bracket", "[::1", -1, NULL, NULL);
  test_parse_host_port_case ("garbage after bracket", "[::1]x", -1, NULL,
			     NULL);
  test_parse_host_port_case ("empty entry", "", -1, NULL, NULL);
  test_parse_host_port_case ("empty host with port", ":6566", -1, NULL,
			     NULL);
  test_parse_host_port_case ("port zero", "host:0", -1, NULL, NULL);
  test_parse_host_port_case ("port too large", "host:65536", -1, NULL,
			     NULL);
  test_parse_host_port_case ("port far too large", "host:99999999", -1,
			     NULL, NULL);
  test_parse_host_port_case ("bracketed with non-numeric suffix", "[::1]:abc",
			     6566, "::1", "abc");
  test_parse_host_port_case ("bracketed with digit prefix suffix",
			     "[::1]:192.168.0.2", 6566, "::1",
			     "192.168.0.2");
  test_parse_host_port_case ("bracketed with empty next entry", "[::1]:", 6566,
			     "::1", "");
#ifdef ENABLE_IPV6
  test_parse_host_port_case ("unbracketed v6", "::1", 6566, "::1", "");
  test_parse_host_port_case ("unbracketed full v6", "2001:db8::1", 6566,
			     "2001:db8::1", "");
  test_parse_host_port_case ("v6 ending in digits", "2001:db8::1:9999", 6566,
			     "2001:db8::1:9999", "");
#endif /* ENABLE_IPV6 */
  test_parse_host_port_case ("bracketed port too large",
			     "[::1]:99999999", -1, NULL, NULL);
}

static void
test_env_list_case (const char *name, const char *list_str,
		    const char **expected_hosts, const int *expected_ports,
		    int expected_count)
{
  char list[256];
  char *next, *entry;
  int i = 0;

  if (snprintf (list, sizeof (list), "%s", list_str) >= (int) sizeof (list))
    {
      fprintf (stderr, "%s: list too long\n", name);
      failures++;
      return;
    }

  next = list;
  while (next && *next)
    {
      char *host = NULL;
      int port;

      entry = next;
      port = parse_host_port (entry, &host, &next);
      if (port < 0)
	{
	  fprintf (stderr, "%s: \"%s\": entry %d rejected\n", name,
		   list_str, i);
	  failures++;
	  return;
	}
      if (i >= expected_count)
	{
	  fprintf (stderr, "%s: \"%s\": more entries than the expected %d\n",
		   name, list_str, expected_count);
	  failures++;
	  return;
	}
      if (strcmp (host, expected_hosts[i]) != 0
	  || port != expected_ports[i])
	{
	  fprintf (stderr, "%s: \"%s\": entry %d: expected host \"%s\""
		   " port %d, got host \"%s\" port %d\n", name, list_str, i,
		   expected_hosts[i], expected_ports[i], host, port);
	  failures++;
	  return;
	}
      i++;
    }

  if (i != expected_count)
    {
      fprintf (stderr, "%s: \"%s\": expected %d entries, got %d\n",
	       name, list_str, expected_count, i);
      failures++;
    }
}

static void
test_env_list (void)
{
  static const char *hosts1[] = { "hosta", "hostb" };
  static const int ports1[] = { 6566, 6566 };
  static const char *hosts2[] = { "hosta", "hostb", "hostc" };
  static const int ports2[] = { 6566, 6566, 6566 };
  static const char *hosts3[] =
    { "new.scanner.com", "::1", "192.168.0.2", "scanner.univ.edu" };
  static const int ports3[] = { 6566, 6566, 6566, 6566 };
  static const char *hosts4[] = { "localhost", "otherhost" };
  static const int ports4[] = { 6580, 6566 };

  test_env_list_case ("two hosts", "hosta:hostb", hosts1, ports1, 2);
  test_env_list_case ("three hosts", "hosta:hostb:hostc", hosts2, ports2, 3);
  test_env_list_case ("man page example",
		      "new.scanner.com:[::1]:192.168.0.2:scanner.univ.edu",
		      hosts3, ports3, 4);
  test_env_list_case ("host with port then host", "localhost:6580:otherhost",
		      hosts4, ports4, 2);
}

static void
test_add_device_port_identity (void)
{
  Net_Device *nd;
  int count = 0;

  first_device = NULL;
  if (add_device ("localhost", 6566, NULL) != SANE_STATUS_GOOD
      || add_device ("localhost", 6560, NULL) != SANE_STATUS_GOOD
      || add_device ("localhost", 6560, NULL) != SANE_STATUS_GOOD)
    {
      fprintf (stderr, "add_device identity: add_device failed\n");
      failures++;
      return;
    }

  for (nd = first_device; nd; nd = nd->next)
    count++;

  if (count != 2)
    {
      fprintf (stderr, "add_device identity: expected 2 devices, got %d\n",
	       count);
      failures++;
    }
}

int
main (void)
{
  alarm (60);

  test_parse_host_port ();
  test_env_list ();
  test_add_device_port_identity ();

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
