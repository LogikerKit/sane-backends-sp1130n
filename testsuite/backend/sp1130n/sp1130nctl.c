/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "sp1130n.h"
#include "sp1130n-image.h"
#include "sp1130n-postprocess.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_bool interrupted;
static void
interrupt_handler (int signal_number)
{
  (void) signal_number;
  atomic_store_explicit (&interrupted, 1, memory_order_relaxed);
}

static int
cancelled (void)
{
  return atomic_load_explicit (&interrupted, memory_order_relaxed);
}

struct scan_options
{
  struct sanei_sp1130n_settings settings;
  struct sanei_sp1130n_image_options image;
  struct sanei_sp1130n_post_options post;
  const char *device;
  const char *paths[2];
  unsigned path_count;
  int experimental, dry_run, rotate_back, skip_blank, help;
};
static const char *const modes[] = { "color", "gray", "lineart" };
static const char *const sources[] = { "front", "duplex", "back" };

static const char *
side_name (const struct scan_options *options, unsigned side)
{
  return options->settings.source == SP_SOURCE_BACK
    || side == 1 ? "back" : "front";
}

static void
usage (FILE *file, const char *program)
{
  fprintf (file,
           "Usage:\n"
           "  %s list\n"
           "  %s status DEVICE\n"
           "  %s maintenance DEVICE --experimental OUTPUT.bin\n"
           "  %s maintenance DEVICE --dry-run [OUTPUT.bin]\n"
           "  %s scan DEVICE --experimental [OPTIONS] OUTPUT.pnm [BACK.pnm]\n"
           "  %s scan DEVICE --dry-run [OPTIONS] [OUTPUT.pnm [BACK.pnm]]\n"
           "  %s scan-duplex DEVICE --experimental FRONT.ppm BACK.ppm\n"
           "\n"
           "Scan options (defaults in parentheses):\n"
           "  --source duplex|front|back (duplex; front/back need one output, duplex two)\n"
           "  --mode color|gray|lineart  (color; binary P6/P5/P4 output)\n"
           "  --resolution 300|600      (300 dpi)\n"
           "  --page-width 50..216      (210 integer mm)\n"
           "  --page-height 50..356     (297 integer mm)\n"
           "  --left 0..216             (0 mm from the physical paper's left edge)\n"
           "  --top 0..356              (0 mm from the physical paper's top edge)\n"
           "  --scan-width 0..216       (0: remaining paper width, integer mm)\n"
           "  --scan-height 0..356      (0: remaining paper height, integer mm)\n"
           "                           The scan area must fit within the physical paper\n"
           "  --brightness -100..100    (0)\n"
           "  --contrast -100..100      (0)\n"
           "  --threshold 0..255        (128; lineart black=1, white=0, MSB-first)\n"
           "  --rotate-back 0|180       (0; rotation applies only to the back)\n"
           "  --autocrop                Crop to content bounds, not physical paper edges\n"
           "  --deskew                  Apply conservative, bounded text deskew\n"
           "  --skip-blank              Retain an empty placeholder for each blank side\n"
           "  --content-threshold 0..255 (240; darker RGB luma counts as content)\n"
           "  --blank-per-mille 0..1000 (1; maximum content fraction for a blank side)\n"
           "  --crop-margin 0..10000    (8 pixels retained around content when cropping)\n"
           "  --experimental           Required before a scan or maintenance query\n"
           "  --dry-run                Validate target/settings offline and print JSON\n"
           "                           No device I/O, presence check or output files\n"
           "                           Paths and --experimental are optional\n"
           "  --help                   Show this help without opening USB\n"
           "  --                       End options (for paths beginning with '-')\n"
           "\n"
           "DEVICE: sp1130n:usb:BUS:ADDR or sp1130n:tcp:IPv4[:PORT] (16888).\n"
           "LAN is experimental and plaintext-only; reported encryption is rejected.\n"
           "LAN acquisition is restricted to full A4 at 300 dpi; source selection is local.\n"
           "Use a trusted wired LAN. No automatic discovery or address changes.\n"
           "\n"
           "The scanner supplies RGB8; crop/deskew/blank detection precede local image modes.\n"
           "Maintenance reads 288 opaque NVRAM bytes; no calibration, reset or NVRAM writes.\n"
           "Outputs must be distinct and new; existing files are never overwritten.\n",
           program, program, program, program, program, program, program);
}

static int
integer (const char *text, int minimum, int maximum, int *value)
{
  const char *p = text;
  if (*p == '-' || *p == '+')
    ++p;
  if (!*p)
    return 0;
  for (; *p; ++p)
    if (*p < '0' || *p > '9')
      return 0;
  errno = 0;
  char *end = NULL;
  long n = strtol (text, &end, 10);
  if (errno || *end || n < minimum || n > maximum)
    return 0;
  *value = (int) n;
  return 1;
}

static int
option_is (const char *argument, size_t length, const char *name)
{
  return strlen (name) == length && !strncmp (argument, name, length);
}

static int
same_paths (const char *front, const char *back)
{
  if (!strcmp (front, back))
    return 1;
  struct stat a, b;
  if (!stat (front, &a) && !stat (back, &b) &&
      a.st_dev == b.st_dev && a.st_ino == b.st_ino)
    return 1;
  char *paths[2] = { strdup (front), strdup (back) };
  if (!paths[0] || !paths[1])
    {
      free (paths[0]);
      free (paths[1]);
      return -1;
    }
  const char *directories[2], *names[2];
  for (unsigned i = 0; i < 2; ++i)
    {
      char *slash = strrchr (paths[i], '/');
      names[i] = slash ? slash + 1 : paths[i];
      directories[i] = slash ? (slash == paths[i] ? "/" : paths[i]) : ".";
      if (slash)
        *slash = '\0';
    }
  int same = !strcmp (names[0], names[1]) &&
    !stat (directories[0], &a) && !stat (directories[1], &b) &&
    a.st_dev == b.st_dev && a.st_ino == b.st_ino;
  free (paths[0]);
  free (paths[1]);
  return same;
}

static int
parse_scan (int argc, char **argv, struct scan_options *options)
{
  *options = (struct scan_options)
  {
    .settings = {.dpi = 300,.width_mm = 210,.height_mm = 297,
                 .source = SP_SOURCE_DUPLEX},
    .image = {.mode = SP_IMAGE_COLOR,.threshold = 128},
    .post = {.content_threshold = 240,.blank_per_mille = 1,.crop_margin = 8}
  };
  if (argc < 3 || !argv[2][0] || argv[2][0] == '-')
    {
      fprintf (stderr, "A DEVICE is required.\n");
      return 0;
    }
  options->device = argv[2];
  int positional = 0;
  for (int i = 3; i < argc; ++i)
    {
      const char *argument = argv[i];
      if (!positional && !strcmp (argument, "--"))
        {
          positional = 1;
          continue;
        }
      if (positional || argument[0] != '-')
        {
          size_t length = strlen (argument);
          if (!length || argument[length - 1] == '/'
              || options->path_count == 2)
            {
              fprintf (stderr,
                       "Expected at most two nonempty output file paths.\n");
              return 0;
            }
          options->paths[options->path_count++] = argument;
          continue;
        }
      const char *equals = strchr (argument, '=');
      size_t length = equals ? (size_t) (equals - argument) : strlen (argument);
      int *flag = NULL;
      if (option_is (argument, length, "--experimental"))
        flag = &options->experimental;
      else if (option_is (argument, length, "--dry-run"))
        flag = &options->dry_run;
      else if (option_is (argument, length, "--help"))
        flag = &options->help;
      else if (option_is (argument, length, "--autocrop"))
        flag = &options->post.autocrop;
      else if (option_is (argument, length, "--deskew"))
        flag = &options->post.deskew;
      else if (option_is (argument, length, "--skip-blank"))
        flag = &options->skip_blank;
      if (flag)
        {
          if (equals)
            {
              fprintf (stderr, "Flag does not take a value: %s\n", argument);
              return 0;
            }
          *flag = 1;
          continue;
        }
      static const char *const names[] = {
        "--source", "--mode", "--resolution", "--page-width", "--page-height",
        "--brightness", "--contrast", "--threshold", "--rotate-back",
        "--left", "--top", "--scan-width", "--scan-height",
        "--content-threshold", "--blank-per-mille", "--crop-margin"
      };
      unsigned option = 0;
      for (; option < sizeof names / sizeof names[0]; ++option)
        if (option_is (argument, length, names[option]))
          break;
      if (option == sizeof names / sizeof names[0])
        {
          fprintf (stderr, "Unknown option: %s\n", argument);
          return 0;
        }
      const char *value =
        equals ? equals + 1 : (i + 1 < argc ? argv[++i] : NULL);
      if (!value)
        {
          fprintf (stderr, "Missing value for %s.\n", names[option]);
          return 0;
        }
      int n = 0, valid = 1;
      switch (option)
        {
        case 0:
          {
            unsigned source = 0;
            for (; source < sizeof sources / sizeof sources[0]; ++source)
              if (!strcmp (value, sources[source]))
                break;
            valid = source < sizeof sources / sizeof sources[0];
            options->settings.source = (enum sanei_sp1130n_source) source;
            break;
          }
        case 1:
          {
            unsigned mode = 0;
            for (; mode < sizeof modes / sizeof modes[0]; ++mode)
              if (!strcmp (value, modes[mode]))
                break;
            valid = mode < sizeof modes / sizeof modes[0];
            options->image.mode = (enum sanei_sp1130n_image_mode) mode;
            break;
          }
        case 2:
          valid = integer (value, 300, 600, &n) && (n == 300 || n == 600);
          options->settings.dpi = (unsigned) n;
          break;
        case 3:
          valid = integer (value, 50, 216, &n);
          options->settings.width_mm = (unsigned) n;
          break;
        case 4:
          valid = integer (value, 50, 356, &n);
          options->settings.height_mm = (unsigned) n;
          break;
        case 5:
          valid = integer (value, -100, 100, &options->image.brightness);
          break;
        case 6:
          valid = integer (value, -100, 100, &options->image.contrast);
          break;
        case 7:
          valid = integer (value, 0, 255, &n);
          options->image.threshold = (unsigned) n;
          break;
        case 8:
          valid = integer (value, 0, 180, &n) && (n == 0 || n == 180);
          options->rotate_back = n;
          break;
        case 9:
          valid = integer (value, 0, 216, &n);
          options->settings.left_mm = (unsigned) n;
          break;
        case 10:
          valid = integer (value, 0, 356, &n);
          options->settings.top_mm = (unsigned) n;
          break;
        case 11:
          valid = integer (value, 0, 216, &n);
          options->settings.scan_width_mm = (unsigned) n;
          break;
        case 12:
          valid = integer (value, 0, 356, &n);
          options->settings.scan_height_mm = (unsigned) n;
          break;
        case 13:
          valid = integer (value, 0, 255, &n);
          options->post.content_threshold = (unsigned) n;
          break;
        case 14:
          valid = integer (value, 0, 1000, &n);
          options->post.blank_per_mille = (unsigned) n;
          break;
        case 15:
          valid = integer (value, 0, 10000, &n);
          options->post.crop_margin = (unsigned) n;
          break;
        }
      if (!valid)
        {
          fprintf (stderr, "Invalid value for %s: %s\n", names[option], value);
          return 0;
        }
    }
  if (options->help)
    return 1;
  if (!strcmp (argv[1], "scan-duplex")
      && options->settings.source != SP_SOURCE_DUPLEX)
    {
      fprintf (stderr, "scan-duplex requires --source duplex.\n");
      return 0;
    }
  if (sanei_sp1130n_image_options_validate (&options->image) != SP_OK)
    {
      fprintf (stderr, "Invalid image options.\n");
      return 0;
    }
  if (sanei_sp1130n_post_options_validate (&options->post) != SP_OK)
    {
      fprintf (stderr, "Invalid postprocessing options.\n");
      return 0;
    }
  struct sanei_sp1130n_geometry geometry;
  if (sanei_sp1130n_scan_geometry (&options->settings, &geometry) != SP_OK)
    {
      fprintf (stderr,
               "Invalid scan area: offsets and dimensions must fit the physical paper.\n");
      return 0;
    }
  unsigned count = options->settings.source == SP_SOURCE_DUPLEX ? 2 : 1;
  if (options->path_count != count
      && !(options->dry_run && !options->path_count))
    {
      fprintf (stderr, "Source %s requires exactly %u output path%s"
               " (or none for --dry-run).\n",
               sources[options->settings.source], count, count == 1 ? "" : "s");
      return 0;
    }
  if (options->path_count == 2)
    {
      int same = same_paths (options->paths[0], options->paths[1]);
      if (same)
        {
          fprintf (stderr, "%s\n",
                   same <
                   0 ? "Out of memory validating paths." :
                   "Front and back output paths must be distinct.");
          return 0;
        }
    }
  if (!options->experimental && !options->dry_run)
    {
      fprintf (stderr,
               "Physical scans require explicit --experimental opt-in.\n");
      return 0;
    }
  return 1;
}

static void
json_string (const char *text)
{
  putchar ('"');
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    {
      if (*p == '"' || *p == '\\')
        printf ("\\%c", *p);
      else if (*p < 0x20)
        printf ("\\u%04x", (unsigned) *p);
      else
        putchar (*p);
    }
  putchar ('"');
}

/* Validate names without USB enumeration or a TCP connection. */
static int
validate_target (const char *name, int *network)
{
  const char *local = !strncmp (name, "sp1130n:", 8) ? name + 8 : name;
  *network = !strncmp (local, "tcp:", 4);
  if (*network)
    {
      struct sanei_sp1130n_net *net = NULL;
      int status = sanei_sp1130n_net_open (name, &net);
      sanei_sp1130n_net_close (net);
      return status;
    }
  /* USB open compares the name with this exact enumeration format. */
  if (strlen (name) != 19 || strncmp (name, "sp1130n:usb:", 12)
      || name[15] != ':')
    return SP_INVALID;
  unsigned bus = 0, address = 0;
  for (unsigned i = 0; i < 3; ++i)
    {
      unsigned char b = (unsigned char) name[12 + i];
      unsigned char a = (unsigned char) name[16 + i];
      if (b < '0' || b > '9' || a < '0' || a > '9')
        return SP_INVALID;
      bus = bus * 10 + b - '0';
      address = address * 10 + a - '0';
    }
  return bus <= 255 && address <= 255 ? SP_OK : SP_INVALID;
}

static int
dry_run (const struct scan_options *options)
{
  int network = 0;
  int status = validate_target (options->device, &network);
  uint8_t parameters[SP_PARAM_SIZE];
  if (status == SP_OK)
    status = network ?
      sanei_sp1130n_network_parameters (parameters, &options->settings) :
      sanei_sp1130n_parameters (parameters, &options->settings);
  if (status != SP_OK)
    {
      fprintf (stderr, "%s\n", sanei_sp1130n_strstatus (status));
      return 1;
    }
  printf ("{\"device\":");
  json_string (options->device);
  printf (",\"source\":\"%s\",\"mode\":\"%s\",\"resolution\":%u,"
          "\"width_mm\":%u,\"height_mm\":%u,\"left_mm\":%u,\"top_mm\":%u,"
          "\"scan_width_mm\":%u,\"scan_height_mm\":%u,\"brightness\":%d,\"contrast\":%d,"
          "\"threshold\":%u,\"rotate_back\":%d,\"autocrop\":%s,\"deskew\":%s,"
          "\"skip_blank\":%s,\"content_threshold\":%u,\"blank_per_mille\":%u,"
          "\"crop_margin\":%u,\"wire_mode\":\"color\",\"outputs\":[",
          sources[options->settings.source], modes[options->image.mode],
          options->settings.dpi, options->settings.width_mm,
          options->settings.height_mm, options->settings.left_mm,
          options->settings.top_mm, options->settings.scan_width_mm,
          options->settings.scan_height_mm, options->image.brightness,
          options->image.contrast, options->image.threshold,
          options->rotate_back, options->post.autocrop ? "true" : "false",
          options->post.deskew ? "true" : "false",
          options->skip_blank ? "true" : "false",
          options->post.content_threshold, options->post.blank_per_mille,
          options->post.crop_margin);
  for (unsigned i = 0; i < options->path_count; ++i)
    {
      if (i)
        putchar (',');
      json_string (options->paths[i]);
    }
  puts ("]}");
  return ferror (stdout) ? 1 : 0;
}

static int
reserve_output (const char *path, FILE **file, int *created)
{
  int fd =
    open (path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      fprintf (stderr, "Cannot reserve output '%s': %s\n", path,
               strerror (errno));
      return SP_IO;
    }
  *created = 1;
  if (fchmod (fd, 0600) || !(*file = fdopen (fd, "wb")))
    {
      int error = errno;
      close (fd);
      fprintf (stderr, "Cannot prepare output '%s': %s\n", path,
               strerror (error));
      return SP_IO;
    }
  return SP_OK;
}

static int
write_pnm (FILE *file, const struct sanei_sp1130n_page *page,
           enum sanei_sp1130n_image_mode mode)
{
  size_t stride = sanei_sp1130n_image_stride (page->width, mode);
  if (!page->data || !page->height || !stride
      || page->height > SIZE_MAX / stride
      || page->size != stride * page->height)
    return SP_IO;
  if (cancelled ())
    return SP_CANCELLED;
  static const char *const magic[] = { "P6", "P5", "P4" };
  if (fprintf (file, "%s\n%u %u\n", magic[mode], page->width, page->height) < 0
      || (mode != SP_IMAGE_LINEART && fprintf (file, "255\n") < 0))
    return SP_IO;
  for (unsigned row = 0; row < page->height; ++row)
    {
      if (cancelled ())
        return SP_CANCELLED;
      /* Packed lineart already uses PBM/SANE polarity; never invert these bits. */
      if (fwrite (page->data + row * stride, 1, stride, file) != stride)
        return SP_IO;
    }
  return cancelled ()? SP_CANCELLED : SP_OK;
}

static int
install_handlers (struct sigaction *old_int, struct sigaction *old_term)
{
  atomic_init (&interrupted, 0);
  if (!atomic_is_lock_free (&interrupted))
    {
      fprintf (stderr,
               "This platform cannot safely use the cancellation signal handler.\n");
      return 1;
    }
  struct sigaction action = { 0 };
  action.sa_handler = interrupt_handler;
  sigemptyset (&action.sa_mask);
  if (sigaction (SIGINT, &action, old_int))
    {
      fprintf (stderr, "Cannot install SIGINT cancellation handler: %s\n",
               strerror (errno));
      return 1;
    }
  if (sigaction (SIGTERM, &action, old_term))
    {
      fprintf (stderr, "Cannot install SIGTERM cancellation handler: %s\n",
               strerror (errno));
      sigaction (SIGINT, old_int, NULL);
      return 1;
    }
  return 0;
}

static int
scan (const struct scan_options *options)
{
  FILE *files[2] = { NULL, NULL };
  int created[2] = { 0, 0 };
  int skipped[2] = { 0, 0 };
  struct sanei_sp1130n_connection *connection = NULL;
  struct sanei_sp1130n_sheet sheet = { 0 };
  struct sigaction old_int, old_term;
  if (install_handlers (&old_int, &old_term))
    return 1;
  int status = SP_OK;
  for (unsigned side = 0; side < options->path_count; ++side)
    {
      if (cancelled ())
        {
          status = SP_CANCELLED;
          goto done;
        }
      status =
        reserve_output (options->paths[side], &files[side], &created[side]);
      if (status != SP_OK)
        goto done;
    }
  if (cancelled ())
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = sanei_sp1130n_connection_open (options->device, &connection);
  if (status != SP_OK)
    goto done;
  struct sanei_sp1130n_io io =
    sanei_sp1130n_connection_io (connection, &interrupted);
  fprintf (stderr, "Experimental %s scan: load one sheet%s.\n",
           sources[options->settings.source],
           options->settings.source ==
           SP_SOURCE_DUPLEX ? " with FRONT/BACK labels" : "");
  uint8_t token[40];
  if (cancelled ())
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = sanei_sp1130n_token (token);
  if (status == SP_OK && cancelled ())
    status = SP_CANCELLED;
  if (status == SP_OK)
    status = sanei_sp1130n_scan_sheet (&io, &options->settings, token, &sheet);
  sanei_sp1130n_connection_idle (connection);
  if (status == SP_OK && cancelled ())
    status = SP_CANCELLED;
  if (status == SP_OK && sheet.count != options->path_count)
    status = SP_IO;
  for (unsigned side = 0; status == SP_OK && side < options->path_count; ++side)
    {
      if (cancelled ())
        {
          status = SP_CANCELLED;
          break;
        }
      struct sanei_sp1130n_post_result post = { 0 };
      status =
        sanei_sp1130n_postprocess (&sheet.page[side], &options->post,
                                   &interrupted, &post);
      if (status != SP_OK)
        break;
      if (cancelled ())
        {
          status = SP_CANCELLED;
          break;
        }
      if (options->skip_blank && post.blank)
        {
          skipped[side] = 1;
          continue;
        }
      struct sanei_sp1130n_image_options image = options->image;
      image.rotate180 = (options->settings.source == SP_SOURCE_BACK
                         || side == 1) && options->rotate_back == 180;
      status = sanei_sp1130n_image_process (&sheet.page[side], &image);
      if (status == SP_OK && cancelled ())
        status = SP_CANCELLED;
    }
  for (unsigned side = 0; status == SP_OK && side < options->path_count; ++side)
    {
      if (cancelled ())
        {
          status = SP_CANCELLED;
          break;
        }
      if (skipped[side])
        {
          fprintf (stderr,
                   "Skipped blank %s side; reserved empty placeholder retained: %s\n",
                   side_name (options, side), options->paths[side]);
        }
      else
        {
          status =
            write_pnm (files[side], &sheet.page[side], options->image.mode);
        }
    }
done:
  sanei_sp1130n_sheet_free (&sheet);
  sanei_sp1130n_connection_close (connection);
  for (unsigned side = 0; side < options->path_count; ++side)
    {
      if (files[side] && fclose (files[side]))
        {
          fprintf (stderr, "Cannot finish output '%s': %s\n",
                   options->paths[side], strerror (errno));
          if (status == SP_OK)
            status = SP_IO;
        }
    }
  if (status == SP_OK && cancelled ())
    status = SP_CANCELLED;
  sigaction (SIGTERM, &old_term, NULL);
  sigaction (SIGINT, &old_int, NULL);
  if (status != SP_OK)
    {
      for (unsigned side = 0; side < options->path_count; ++side)
        {
          if (created[side])
            fprintf (stderr,
                     "Created output retained (possibly incomplete): %s\n",
                     options->paths[side]);
        }
      fprintf (stderr, "%s\n", sanei_sp1130n_strstatus (status));
    }
  return status == SP_OK ? 0 : 1;
}

static int
maintenance (int argc, char **argv)
{
  if (argc < 3 || !argv[2][0] || argv[2][0] == '-')
    {
      fprintf (stderr, "A DEVICE is required.\n");
      return 2;
    }
  const char *path = NULL;
  int experimental = 0, dry = 0, positional = 0, help = 0;
  for (int i = 3; i < argc; ++i)
    {
      const char *arg = argv[i];
      if (!positional && !strcmp (arg, "--"))
        positional = 1;
      else if (!positional && !strcmp (arg, "--experimental"))
        experimental = 1;
      else if (!positional && !strcmp (arg, "--dry-run"))
        dry = 1;
      else if (!positional && !strcmp (arg, "--help"))
        help = 1;
      else
        {
          size_t length = strlen (arg);
          if (path || !length || arg[length - 1] == '/' ||
              (!positional && arg[0] == '-'))
            {
              fprintf (stderr,
                       "Maintenance accepts flags and one new output file path.\n");
              return 2;
            }
          path = arg;
        }
    }
  if (help)
    {
      usage (stdout, argv[0]);
      return 0;
    }
  if ((!path && !dry) || (!experimental && !dry))
    {
      fprintf (stderr,
               "Maintenance requires --experimental and OUTPUT.bin, or --dry-run.\n");
      return 2;
    }
  if (dry)
    {
      int network = 0;
      int status = validate_target (argv[2], &network);
      if (status != SP_OK)
        {
          fprintf (stderr, "%s\n", sanei_sp1130n_strstatus (status));
          return 1;
        }
      printf ("{\"device\":");
      json_string (argv[2]);
      printf
        (",\"operation\":\"maintenance\",\"read_only\":true,\"bytes\":%u,\"output\":",
         SP_NVRAM_SIZE);
      if (path)
        json_string (path);
      else
        printf ("null");
      puts ("}");
      return ferror (stdout) ? 1 : 0;
    }
  struct sigaction old_int, old_term;
  if (install_handlers (&old_int, &old_term))
    return 1;
  FILE *file = NULL;
  int created = 0, status = SP_OK;
  struct sanei_sp1130n_connection *connection = NULL;
  uint8_t token[40], snapshot[SP_NVRAM_SIZE];
  if (cancelled ())
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = reserve_output (path, &file, &created);
  if (status != SP_OK)
    goto done;
  if (cancelled ())
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = sanei_sp1130n_connection_open (argv[2], &connection);
  if (status != SP_OK)
    goto done;
  if (cancelled ())
    {
      status = SP_CANCELLED;
      goto done;
    }
  status = sanei_sp1130n_token (token);
  if (status == SP_OK && cancelled ())
    status = SP_CANCELLED;
  if (status == SP_OK)
    {
      struct sanei_sp1130n_io io =
        sanei_sp1130n_connection_io (connection, &interrupted);
      status = sanei_sp1130n_read_nvram (&io, token, snapshot);
    }
  if (status == SP_OK && cancelled ())
    status = SP_CANCELLED;
  if (status == SP_OK
      && fwrite (snapshot, 1, sizeof snapshot, file) != sizeof snapshot)
    status = SP_IO;
done:
  sanei_sp1130n_connection_close (connection);
  if (file && fclose (file))
    {
      fprintf (stderr, "Cannot finish output '%s': %s\n", path,
               strerror (errno));
      if (status == SP_OK)
        status = SP_IO;
    }
  if (status == SP_OK && cancelled ())
    status = SP_CANCELLED;
  sigaction (SIGTERM, &old_term, NULL);
  sigaction (SIGINT, &old_int, NULL);
  if (status == SP_OK)
    {
      fprintf (stderr,
               "Read-only maintenance snapshot: %u opaque bytes in %s\n",
               SP_NVRAM_SIZE, path);
    }
  else
    {
      if (created)
        fprintf (stderr, "Created output retained (possibly incomplete): %s\n",
                 path);
      fprintf (stderr, "%s\n", sanei_sp1130n_strstatus (status));
    }
  return status == SP_OK ? 0 : 1;
}

static int
list (void)
{
  struct sanei_sp1130n_device *devices = NULL;
  size_t count = 0;
  int status = sanei_sp1130n_usb_list (&devices, &count);
  if (status == SP_OK)
    {
      for (size_t i = 0; i < count; ++i)
        puts (devices[i].name);
      if (!count)
        fprintf (stderr, "No Ricoh SP-1130N USB device found.\n");
    }
  free (devices);
  if (status != SP_OK)
    fprintf (stderr, "%s\n", sanei_sp1130n_strstatus (status));
  return status == SP_OK ? 0 : 1;
}

static int
device_status (const char *device)
{
  struct sanei_sp1130n_connection *connection = NULL;
  int status = sanei_sp1130n_connection_open (device, &connection);
  if (status != SP_OK)
    {
      fprintf (stderr, "%s\n", sanei_sp1130n_strstatus (status));
      return 1;
    }
  struct sanei_sp1130n_io io = sanei_sp1130n_connection_io (connection, NULL);
  uint8_t reply[108] = { 0 };
  status = sanei_sp1130n_probe (&io, reply);
  if (status == SP_OK)
    {
      printf ("Firmware error: 0x%08x; lock: %u\n", sanei_sp1130n_le32 (reply),
              sanei_sp1130n_le32 (reply + 32));
      for (size_t i = 0; i < sizeof reply; ++i)
        printf ("%02x%s", reply[i], i % 16 == 15 ? "\n" : " ");
      puts ("");
      status = sanei_sp1130n_device_error (sanei_sp1130n_le32 (reply));
    }
  sanei_sp1130n_connection_close (connection);
  if (status != SP_OK)
    fprintf (stderr, "%s\n", sanei_sp1130n_strstatus (status));
  return status == SP_OK ? 0 : 1;
}

int
main (int argc, char **argv)
{
  int scanning = argc >= 2 && (!strcmp (argv[1], "scan")
                               || !strcmp (argv[1], "scan-duplex"));
  int known = scanning || (argc >= 2
                           && (!strcmp (argv[1], "list")
                               || !strcmp (argv[1], "status")
                               || !strcmp (argv[1], "maintenance")));
  if ((argc == 2 && !strcmp (argv[1], "--help"))
      || (argc == 3 && known && !strcmp (argv[2], "--help")))
    {
      usage (stdout, argv[0]);
      return 0;
    }
  if (argc == 2 && !strcmp (argv[1], "list"))
    return list ();
  if (argc == 3 && !strcmp (argv[1], "status") && argv[2][0]
      && argv[2][0] != '-')
    return device_status (argv[2]);
  if (argc >= 2 && !strcmp (argv[1], "maintenance"))
    return maintenance (argc, argv);
  if (scanning)
    {
      struct scan_options options;
      if (!parse_scan (argc, argv, &options))
        {
          usage (stderr, argv[0]);
          return 2;
        }
      if (options.help)
        {
          usage (stdout, argv[0]);
          return 0;
        }
      return options.dry_run ? dry_run (&options) : scan (&options);
    }
  usage (stderr, argv[0]);
  return 2;
}
