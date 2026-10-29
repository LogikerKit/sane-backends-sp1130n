# Experimental SP-1130N Backend

This is an initial Linux-only SANE port, not a hardware-validated release.
Duplex remains the default.

## Build

From a SANE source checkout, with its documented Autotools prerequisites:

```sh
./autogen.sh
./configure BACKENDS=sp1130n
make
make check
```

Dependencies for this backend: C11, libusb-1.0, libjpeg and pthreads.
Python 3 runs the optional synthetic TCP integration tests. No vendor package
is a build or runtime dependency. An automatic/default configure excludes this
untested backend. Explicit selection without its dependencies fails.
The default `dll.conf` entry remains commented out.

No system installation, scanner configuration or physical scan was performed.
The test suite uses authored replies and synthetic JPEGs on localhost. A
passing fixture cannot establish compatibility with real firmware.

## Checked Contribution

Rebased and checked on October 29, 2026 against current SANE master
`1052d69a6f5e9aab2032b1de27ab1f364c9b6553`.
The normal build and full `make check` passed, including upstream's new
`net_test`. All seven SP-1130N groups, including twenty localhost LAN cases,
and `net_test` also passed in the UBSan preload build. The upstream
scanimage/saned shell scripts skipped because the SANE test backend was not
built. No physical scanner validation or full upstream UBSan rerun was done.
The October 25 results below describe the earlier baseline.

Reviewed on October 25, 2026 against SANE master commit
`982deaefc2479c117d2b8dba2200765134385490`.
The normal Linux build and `make check` passed; the upstream scanimage/saned
shell integration scripts skipped because the SANE test backend was not built.
All seven SP-1130N test entries passed, including twenty localhost LAN cases.
The new backend exports only its `sane_sp1130n_*` and `sanei_*` symbols.

The latest correction covers signal-handler cancellation, device-list lifetime,
USB UUID cancellation and error-payload cleanup, TCP reset reporting, and
preservation of valid JPEG restart metadata. Authored regressions reproduce
those boundaries. The updated normal suite and all seven backend entries in
the UBSan preload build passed. A separate ASan device-list lifetime probe
passed with leak detection disabled; no leak-check result is claimed.

Further regressions cover payload-bearing USB completion errors and cancellation
at USB command/data boundaries. Pending USB replies, declared image payloads
and final completion records are consumed before STOP/unlock reuses the reply
stream. Cancellation can wait for those bounded transactions and cleanup;
the USB image phase has a 120-second timeout budget followed by up to 25
seconds for STOP/unlock.
The noninstalled test utility also validates device-name syntax and supported
transport profiles during a dry run, without checking device presence or
contacting a scanner.

A GNU C11/UBSan build with `PRELOADABLE_BACKENDS=sp1130n` also built, and
the same SP-1130N tests passed without sanitizer findings. The full upstream
UBSan check did not pass: unchanged `sanei/sanei_codec_bin.c:117` shifts
255 left by 24 as a signed int in `test_wire`. That pre-existing finding was
not changed in this contribution. Preloading required removing a redundant
`sanei_usb.lo` link already supplied by SANE's `libdll_preload.la`.

Default build selection excluded this backend, and explicit selection with
`--without-usb` failed.
These results do not replace physical scanner or maintainer validation.

## Use

Enable only this backend in an isolated SANE configuration. Set
`SP1130N_EXPERIMENTAL=1` for scanning. Select USB from `scanimage -L` or explicitly
select `sp1130n:tcp:IPv4`. LAN defaults to native TCP port 16888; discovery is
not implemented. A nonzero encryption indication is refused, not downgraded.
Plaintext has no confidentiality or scanner authentication.

```sh
SP1130N_EXPERIMENTAL=1 scanimage -d sp1130n:tcp:192.168.1.50 \
  --source "ADF Duplex" --batch=side-%03d.pnm --batch-count=2 --format=pnm
```

The IP is a placeholder. Use one replaceable, non-sensitive sheet marked
FRONT/BACK for the first real test, not a stack. Front and back are separate
SANE images; the second `start` serves the buffered back, not a second feed.
Cancellation between pages discards that buffered image and ends the feeder
job. The cancellation request itself only sets lock-free atomic state; bounded
cleanup runs in the next normal backend call or on close, not in a signal
handler. LAN retains one image socket across all sheets, not one socket per sheet.
After end-of-feeder or a failed LAN job, cancel or reopen before starting
another feeder job.

USB options include front/back-only sources, 300/600 dpi and integer-mm scan
windows. LAN currently accepts only full A4 at 300 dpi, with front/back-only
selection performed on the host after duplex acquisition. Unverified LAN feed
profiles are refused before scanner I/O. Both transports offer
color/gray/lineart, brightness/contrast/threshold, optional back
rotation, host-side cropping, conservative deskew and blank filtering.
Host-side processing does not imply corresponding firmware features.
Blank filtering changes page numbering and may drop faint marks.

The LAN interoperability review used the public
[`namp/sp-1130n-macos`](https://github.com/namp/sp-1130n-macos) project at commit
`626237c7bd26f53bc111adf191b63a73d249ebfa`. Its reported hardware results are
not validation of this SANE backend. The integration remains native C/SANE,
not a port of the macOS eSCL service; no vendor binary is included.
Regression fixtures cover reversed LAN side identifiers, short headers,
multi-sheet connection reuse, normal feeder end, missing sides, malformed
lengths, truncation, timeout and cancellation. Physical lock/status, feed
profile and abort cleanup remain unverified.
LAN front/back byte interpretation is provisional and must be checked with
labeled originals; neither front/back-only output nor back rotation has been
physically verified.
Short-header-only/EOI-only streams without a full side-identifying header
remain unsupported; real-device testing must determine whether they occur.

The read-only 288-byte NVRAM primitive remains internal to the backend and is
tested using a noninstalled test CLI. No SANE maintenance or calibration
option is exposed; no write/reset/restore/first-read-date command is added.
The USB trace helper and all actual captures are excluded from this port.

## Before Submission

Hardware-unvalidated: no physical scanner testing has been completed.
Do not open a pull request or merge request until hardware testing is complete.
In particular, duplex operation must be validated on both USB and LAN.

Check the contribution before sharing:

```sh
make check
git diff --cached --check
```

Review the code and license notices. Do not add private material, generated
code, logs, credentials or binaries.
Do not sign a contributor certification without reviewing what it certifies.

`MERGE-REQUEST.md` is a draft for maintainer discussion, not an already
submitted request. Physical duplex, second-sheet, 600-dpi, orientation, error,
cancellation and frontend tests remain required. Maintainers should decide
backend placement and licensing conventions before release inclusion.
