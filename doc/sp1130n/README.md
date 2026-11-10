# SP-1130N Backend

Linux-only SANE backend for the Ricoh/Fujitsu SP-1130N. Duplex is the default.
Basic support has limited physical test results on the Fujitsu-ID model.
The current shared `sanei_usb` transport passed a two-sheet Color/300-dpi
duplex batch with four complete images and user-confirmed full ejection.
Earlier USB results below used direct libusb and do not validate other
modes or recovery with the shared helper. Native Ethernet has separate
physical results. The Ricoh USB ID is still untested. This contribution is
prepared for upstream review, not already accepted or fully validated in
every mode; remaining limitations are recorded below.

## Build And Use

From a SANE checkout with its documented Autotools prerequisites:

```sh
./autogen.sh
./configure BACKENDS="sp1130n test net"
make
make check
```

The driver requires C11, libusb-1.0, libjpeg and pthreads. Python 3 runs the
optional synthetic TCP tests. No vendor package is a build or runtime
dependency. Automatic backend selection includes this driver on Linux when
its dependencies are available; its default `dll.conf` entry is enabled.
No scanner-specific environment opt-in is required. Existing installations
may still need `sp1130n` added to their SANE configuration.

USB accepts Ricoh `05ca:031e` and Fujitsu `04c5:1627`. Use the
`sp1130n:usb:BUS:ADDRESS` name returned by `scanimage -L`; addresses can change
after reconnecting. Physical tests used the Fujitsu-ID device. Compatibility
with the non-N SP-1130 or other models is not implied.

LAN uses explicit numeric unicast IPv4 and TCP port 16888, optionally
overridden in `sp1130n:tcp:IPv4:PORT`. Add `tcp:IPv4:PORT` to
`/etc/sane.d/sp1130n.conf` to list it in ordinary SANE frontends, without
using a special launcher. Configured devices are listed without opening
sockets or checking availability; local-only requests omit LAN entries.
It is not eSCL, WSD or SANE's net protocol. There is no broadcast discovery
or DNS lookup. Transfer is plaintext without
scanner authentication; reported encryption is refused, not downgraded.

Example with a placeholder address and one replaceable, non-sensitive sheet:

```sh
scanimage -d sp1130n:tcp:192.168.1.50 \
  --source "ADF Duplex" --batch=side-%03d.pnm --batch-count=2 --format=pnm
```

Load originals face-down, head-first as specified in the operator guide.
Each acquired sheet yields front then back as separate SANE images. Do not
cancel between those images if the back is required. Sheet order remains
physical acquisition order; the driver does not reverse an entire stack.

## Options And Lifecycle

USB supports 300/600 dpi and integer-mm acquisition windows.
Only those two resolutions are exposed by SANE. The noninstalled diagnostic
CLI accepts USB and full-A4 LAN integer native-resolution trials from 50
to 600 dpi; encoding,
geometry and rejection tests do not establish hardware support for that range.
Do not infer valid feeding, image geometry or ejection from a successful
offline dry run. No host-side resolution resampling is performed.
The CLI's optional `--trace-protocol` logs acquisition phases, status and
selected image-header fields, not image payloads, device identities or lock
tokens. It does not change scan commands or enable automatic retries.
The CLI rejects new-resolution images with a width inconsistent with the
requested dpi or a height beyond the requested paper, before host processing.
It does not relabel or downsample a 300-dpi reply as a lower-resolution success.
1200-dpi output is deliberately unsupported and rejected.
Coordinates are relative to the centered sheet. Page-size changes constrain
the window to the new edges and request option/parameter reloads. The nominal
216-mm setting is capped at the existing feeder pixel width: 2550 at 300 dpi
and 5100 at 600 dpi. This is a bounds correction, not geometry calibration.
SANE LAN still exposes only 300 dpi while new values are being qualified.
LAN accepts only full A4; custom feed windows are rejected before scanner I/O.
Sources are `ADF Duplex`, `ADF Front` and `ADF Back`. LAN always
acquires and validates both sides before selecting output on the host.

Both transports offer Color, Gray and Lineart, brightness/contrast/threshold,
optional back rotation and host-side autocrop, deskew and blank filtering.
Rotation, paper-edge detection/cropping, paper-edge deskew and blank
classification reuse SANE's shared `sanei_magic` helpers, rather than separate
scanner-specific algorithms. The small RGB-to-Gray/Lineart and
brightness/contrast conversions remain local because those operations have no
equivalent `sanei_magic` API. Gray and Lineart remain host-generated modes.

Autocrop, deskew and blank filtering are disabled by default. Autocrop now
detects media edges, not a content bounding box. `crop-margin` retains pixels
around the detected edges. Deskew uses media edges and applies corrections
only within five degrees; missing edges leave geometry unchanged. The former
`content-threshold` option is removed, not silently ignored. `blank-per-mille`
now limits average RGB darkness (0 = white, 1000 = black), not the fraction
of pixels below a luma cutoff. Classification follows any requested
deskew/crop, as in the Fujitsu backend. Whole-image `sanei_magic_isBlank` is
used so small scan windows and edge annotations remaining after cropping
are not omitted from classification.
Filtering can still drop faint marks and changes output numbering; at most
32 blank sheets are acquired per start.

These helpers are explicitly called inside the backend, not an automatic
frontend processing layer or corresponding firmware capability. Shared
helpers operate on a private buffer so allocation failure or cancellation
preserves the input image. Cancellation is checked between helper calls;
the shared image-processing functions themselves are not interruptible.
Native orientation, acquisition, transfer, feeding and ejection commands
are unchanged by this migration. Previous crop/deskew/filter hardware checks
do not qualify the changed algorithms; new image-quality checks are needed.

USB `upright-loading=yes` optionally accommodates front-face-up, readable
upright loading by swapping logical sides, rotating both images 180 degrees
and mirroring the acquisition window. It defaults off and is inactive on
LAN. `back-rotate` adds rotation to the logical back only.

LAN retains one image connection and one prepare/start across a feeder job.
USB follows completion records, which are not themselves evidence of an
empty feeder. Positive paper status determines exhaustion. Failed jobs latch
errors until cancellation or reopening; no automatic acquisition retry is
performed. Cancel sets lock-free atomic state; bounded cleanup occurs in a
normal backend call, on close, or in a per-handle worker which checks the flag
every 100 ms while the frontend is idle. Cleanup never runs in a signal handler.
Close waits for the worker before releasing the handle. USB finishes
accepted command/data transactions before STOP/unlock. See `PROTOCOL.md`
for framing, deadlines and cleanup details.
The USB transport uses SANE's shared `sanei_usb` helper for discovery,
opening, endpoint selection, transfers and closing. Existing Linux
`sp1130n:usb:BUS:ADDRESS` names remain accepted. Helper calls are serialized
within this adapter, with the requested timeout set before each transfer.
Other backends do not participate in that adapter mutex.

The helper reports bulk failures without a distinct timeout status or reliable
partial-byte count, and its current implementation clears the endpoint halt
on errors. The adapter therefore does not enable inquiry or image-tail recovery.
A failed bulk transfer blocks subsequent transfers, including STOP/unlock
commands which could misinterpret a late payload as their acknowledgement.
No command or acquisition is automatically replayed, and no scanner reset is
added. Closing/reopening clears host state, not necessarily pending replies or
the scanner lock; a fault may require physical clearance or a power cycle.
Normal scans retain the existing duplex, feeder and first-sheet policies.

The protocol's optional tail-recovery paths retain synthetic test coverage,
but are not enabled by the shared USB adapter. The dated fault-injection
checks below were performed with the earlier direct-libusb transport, not
the current shared-helper adapter.

## Document Scanner

GNOME Document Scanner 46's "Single Page" ends after one SANE image, even with
"Both Sides" selected. It cancels the job and discards the buffered back.
It sets the backend's `batch-scan` option to false. This compatibility mode
**consumes every loaded sheet**, retaining only the selected side(s) of the
first physical sheet. The first start completes the feeder job before any
image is exposed, so stopping after the first image does not interrupt that
normal feed sequence. Later images are discarded in memory, not saved.
This is not a physical single-sheet mode; do not load sheets that must remain
in the tray. Normal `batch-scan=true` (the default) returns all sheets.

Ethernet batch acquisition uses a dedicated worker. It receives JPEG images
while the frontend decodes, processes and compresses completed sheets. The
in-memory FIFO retains at most 32 additional sheets (including acquisition
in progress) and 128 MiB of queued compressed data. An in-progress sheet may
add up to 128 MiB; decoded pixels for the current sheet are separate.
Front/back ordering is preserved, and completed sheets are delivered before
a later acquisition error. Cancel discards the queue and ends acquisition.
No images are spooled to disk. A persistently slower frontend can still fill
the queue and pause feeding. USB and `batch-scan=false` remain synchronous.

On November 8, 2026, the local parallel-acquisition build completed a physical
Ethernet Color/300-dpi duplex PNG test: 21 sheets, 42 complete images, exit 0
and positive empty-feeder status. Acquisition and unlock completed at about
42 seconds; buffered PNG output completed at 59.65 seconds. The user reported
smooth continuous feeding. All output was discarded without image storage or
content inspection; this does not validate image quality or disk throughput.
This run preceded a final cancellation-race guard. The final build passed
full normal checks, scanner UBSan checks, backend memory/leak checks and 25
repeated concurrency runs. On the same date, that final build completed an
Ethernet Gray/300-dpi duplex PNG test: 21 sheets, 42 complete images in
43.38 seconds, exit 0, positive empty-feeder status and successful unlock.
PNG output took at most 0.479 seconds per side. Again all image bytes were
discarded without storage or inspection. Physical ejection was not
independently observed, and this is not a GNOME Text-preset or image-quality
test.

Use "All Pages From Feeder" with "Both Sides" to obtain duplex images, including
when only one sheet is loaded. Cancelling must not require a subsequent
frontend call to send the existing bounded STOP/unlock sequence. Synthetic
USB event-2/event-4 and live localhost LAN regressions cover idle cancellation;
these are not evidence of physical ejection.

For command-line single-image compatibility testing, select `--batch-scan=no`;
this backend option is distinct from scanimage's `--batch` output-file option.
A client requesting another image can still read the retained first back side
without scanner I/O. Later-sheet errors, failed cleanup or cancellation cause
the first start to fail without exposing even the first image. Blank filtering
applies only to the retained first sheet, not a later replacement sheet.
The drain has a 100-total-sheet cap and a 120-second deadline checked between
acquisitions. Accepted transactions keep their existing bounded waits.
Hitting a limit reports an error, not completed feeding; physical removal may
be necessary. Two limited USB checks of this path are recorded below;
successful fixtures alone do not resolve the prior native single-sheet failure.

The app's "Image" preset selects Color. Its default "Text" preset requests Gray
and reduces the returned eight-bit grayscale to two bits per pixel (four
shades). These labels and conversion belong to the frontend; the backend also
offers Gray without that conversion and one-bit Lineart to clients exposing
the SANE scan-mode option. For example, use `scanimage --mode Gray`.
Brightness and contrast are applied as requested by the frontend; zero is
neutral. A saved negative brightness affects both presets.

## Validation Summary

November 3, 2026 shared-image-helper migration checks passed full normal
`make check` with 32 passing entries and all eight scanner-specific UBSan
entries, including 42 localhost LAN cases. Image conversion/rotation and
shared postprocessing allocation tests also passed ASan/UBSan/leak checks.
Regressions cover helper parity, crop boundaries, blankness after cropping,
small windows, odd-width Lineart rotation and input preservation on failure.
A shared `sanei_magic` allocation-failure cleanup defect was fixed; the
regression reproduces a NULL-pointer load with the original helper and
passes with the fix. Existing SANE copyright and license notices are retained.
No new physical image-quality or GUI checks were performed for this
migration, and previous custom-processing checks do not validate it.

November 3, 2026 diagnostic resolution trials returned two complete RGB8
images per sheet at 200 dpi over USB (1653x2329) and LAN (1653x2321), and
400 dpi over USB and LAN (3307x4656). No host-side resolution resampling
was performed. Independent status inquiries reported error 0 and lock 0.
LAN400 initially stopped mid-feed before its first image prefix arrived;
physical clearance was required. Extending only the initial prefix wait
from five to thirty seconds then passed a hardware retest, with user-confirmed
paper passage. An authored six-second delayed-reply regression fails on the
old wait and passes on the fix. Full normal `make check` and all eight scanner
UBSan entries passed, including 41 localhost cases. These are limited
diagnostic trials, not qualification of every integer dpi, all image modes,
repeated feeding or internal sensor resolution. Normal SANE resolution
options remain USB 300/600 and LAN 300 pending broader qualification.

November 3, 2026 shared-USB integration checks passed full normal
`make check` with 32 entries, no failures or skips, and all eight
scanner-specific UBSan entries, including 36 localhost LAN cases.
The new adapter regression also passed ASan/UBSan/leak checks. It uses
authored shared-helper stubs, not scanner captures. Full upstream UBSan
testing still fails on the signed shift in unchanged
`sanei/sanei_codec_bin.c:117`; no full-suite sanitizer pass is claimed.
No shared SANE source or API was changed.

A physical Color/300-dpi ADF Duplex batch through the local shared-USB build
returned four complete RGB8 images from two loaded sheets and exited cleanly
after positive feeder exhaustion. Both completions were event 4, with
successful STOP/unlock. An independent subsequent status inquiry reported
firmware error 0 and lock 0. The user confirmed both sheets fully ejected
without assistance. Image payload sizes matched their headers, and a private
thumbnail inspection found no obvious corruption; printed side labels and
ordering were not independently established in this run.
This limited test does not validate 600 dpi, Gray/Lineart, first-sheet
compatibility, event 2, or fault recovery with the current shared-USB adapter.
No new LAN hardware run was performed for this transport-only change.

November 3, 2026 upstream-preparation checks rebuilt the backend after
removing the experimental build/scan opt-in. Automatic Linux backend selection
included `sp1130n` with its dependencies available. Fresh normal and UBSan
builds passed; full normal `make check`, all seven scanner-specific UBSan
entries (including 36 localhost LAN cases), and the backend
AddressSanitizer/UBSan/leak regression passed with no scanner opt-in set.
No new physical scan was performed for this labeling/default-selection change.

November 3, 2026 first-sheet compatibility checks passed full normal
`make check`, all seven scanner-specific UBSan entries (including 36 localhost
LAN cases), and the backend AddressSanitizer/UBSan/leak-check regression.
Tests cover retaining the first duplex pair, front/back selection, confirmed
exhaustion before first read, cancellation while draining, later-sheet faults,
cleanup failure propagation, blank-first filtering and the sheet-count bound.
USB replay covers both event `2` continuation and event `4` acquisition
boundaries without changing either sequence.

Two physical USB Gray/300-dpi tests used two loaded sheets, `batch-scan=false`
and exactly one frontend start: one with ADF Duplex and one with ADF Front.
Both consumed the two sheets, confirmed empty paper status and successful
STOP/unlock, and returned only the retained first front image to EOF.
The user confirmed both sheets went through and reloaded after each test.
The respective first-image dimensions were 2480x3506 and 2480x3507.
All pixels were discarded in memory without saving document images.
These are limited direct-backend CLI results, not installed-GUI validation
or a native physical single-sheet feed fix. This new option has not been
physically checked over LAN or at USB 600 dpi.

November 3, 2026 idle-cancellation follow-up passed full normal `make check`
and all seven scanner-specific UBSan entries, including 33 localhost LAN
cases. The backend regression also passed an AddressSanitizer/UBSan build.
The new tests observe cleanup while the frontend makes no further call:
USB event `2` sends STOP/unlock, event `4` does not duplicate cleanup, and a
live localhost LAN session closes before restart. After installing this
update locally, the user reported that USB single-image GUI scanning still
left the sheet incompletely ejected. The idle-cleanup fix therefore does not
resolve that physical ejection problem. Its cause remains unconfirmed.

November 2, 2026 checks against official SANE master
`5392ca0da55edd6ac9f941ce615fd62243c8a296` passed bootstrap, configure, build
and full normal `make check` without skips. The scanner-specific suite has
seven test entries, including 33 localhost LAN cases; normal and backend
UBSan checks passed. Full upstream UBSan testing previously failed on a
signed shift in unchanged `sanei/sanei_codec_bin.c:117`; no full-suite
sanitizer pass is claimed.
Static builds explicitly skip the ctypes-only LAN cases; CLI network fixtures
and the C regressions still run.

November 3, 2026 follow-up checks rebuilt the updated USB event-2 continuation
and empty-reply pacing changes. Full normal `make check` and all seven
scanner-specific UBSan test entries passed without skips, including all 33
localhost LAN cases. A sleep-wrapper regression injects cancellation after
the first backoff slice and requires immediate cleanup without another slice
or image-block request. Private mutation checks rejected restarting after
event `2`, removing pacing and ignoring cancellation during the pause.
These are software results; the November 2 physical results below predate
these changes. No independent AddressSanitizer run is claimed here.
A subsequent November 3 physical Ethernet Color/300-dpi duplex batch on the
updated build returned four complete images (two duplex pairs) in 6.63 seconds.
Guarded reads, RGB8 stride/declared byte counts, EOF, independently confirmed
feeder exhaustion and successful unlock passed. Client/server exited cleanly,
with no saved images; independent pre/post firmware error and lock were zero.
The user confirmed full paper passage and reload. This limited LAN result
does not physically exercise USB event-2 continuation or repeated-empty pacing,
and no new printed-label/orientation check was performed.

Synthetic tests use authored protocol replies and generated JPEGs, including
partial transfers, distinct sheet/side values, allocation failures, malformed
data, cancellation, error latching and confirmed-empty checks. They are not
recorded vendor traffic. Actual scanimage/saned frontend checks were also run;
no GUI validation is claimed.
The protocol test also exercises the real USB acquisition core and SANE handle
over a synthetic adapter: buffered backs, event-2 cleanup, cancellation,
exhaustion, loaded restart, inquiry/image timeout tail accounting, failed
recovery, invalid framing and fault latching.
This does not exercise libusb or prove general timeout resynchronization on
the physical scanner.

Limited physical results on the Fujitsu-ID SP-1130N:

- USB and Ethernet native Color/300-dpi duplex returned labeled
  FRONT1/BACK1/FRONT2/BACK2 upright with complete images and exhaustion.
- Both transports completed Color/Gray/Lineart stacks through localhost-only
  saned. Exact output stride/depth/byte counts and EOF were checked.
- Both transports passed loaded restart on the same handle after exhaustion,
  cancel and an explicit reload. Assistance-free ejection was confirmed for
  those particular tests.
- USB native front-only/back-only labels passed. Optional back rotation kept
  fronts upright and rotated only backs; ejection confirmation for that latest
  rotation run remains pending.
- USB Color/600-dpi runs succeeded, including a 23.52-second two-sheet batch
  with confirmed ejection. Earlier intermittent startup and partial-ejection
  failures remain unexplained; later successes do not erase them.
- Empty starts and between-image cancellation/empty restart passed on both
  transports. Ethernet idle empty-cover rejection and fresh-handle recovery
  passed without feeding; moving-paper cancellation was not tested.
- The November 2 build also passed a reloaded Ethernet Color/300 duplex run and
  confirmed-empty cleanup after a completion record followed by socket closure.
  A preceding first-image firmware error `0x4004` remains unexplained.
  The newer direct-close-before-record branch also passed one private EOF
  injection after complete duplex images, with a real empty inquiry and unlock.
  Ejection was confirmed. This was not a spontaneous scanner closure or a
  SANE frontend test; loaded/unknown/error outcomes remain fixture-tested only.
- Ethernet front-only/back-only each returned one complete image and
  confirmed-empty cleanup, with ejection confirmed. Both checks then failed
  the immediate fresh-handle empty start with `DEVICE_BUSY`; later independent
  status was error 0/lock 0. The cause remains unresolved. These runs did not
  inspect printed side labels or orientation.
  Separate front-only, back-only and duplex/back-rotation diagnostic runs
  subsequently passed image integrity, exhaustion and immediate empty reopen,
  with ejection confirmed. Only phase-status logging was added, not a recovery
  fix; these successes do not resolve the earlier busy failures or validate
  printed labels/rotation.
- One Ethernet cancellation 0.5 seconds after START acknowledgement returned
  `CANCELLED` with zero image output and successful STOP/unlock. The user
  confirmed ejection. This preceded the first image header; it does not prove
  cancellation during physical paper movement or image transfer.
- A subsequent Ethernet cancellation after the first image header returned
  `CANCELLED` with zero sides, but STOP and unlock both failed with I/O/protocol
  errors. Ejection was confirmed and a later independent status was error
  0/lock 0. A metadata-only diagnostic repeat showed STOP's TCP reply failing
  with a peer reset after the partially unread image socket was closed.
  The backend now consumes only the validated current payload remainder within
  one second before cleanup. Two first-payload cancellation retests, including
  one without syscall tracing, passed with zero output, successful STOP/unlock,
  confirmed ejection and independent error 0/lock 0. A separately reloaded
  fresh-handle duplex scan also passed complete images and immediate empty
  reopen without a reset. Partial-header cancellation and other transfer
  phases remain unvalidated on hardware; the earlier failures are retained.
- One injected timeout with zero transferred bytes during the first USB
  Color/300-dpi image payload recovered by consuming the known 524,288 bytes.
  STOP/unlock and independent idle status passed; a reloaded fresh-handle
  duplex scan then completed without requiring a reset or power cycle.
  The user reported paper passage for both runs. No partial image or automatic
  acquisition retry occurred. This was not a spontaneous USB failure;
  physical partial-transfer faults and same-handle loaded fault recovery
  remain unvalidated.
- One injected zero-byte timeout before the first USB image header recovered
  the 64-byte header and its validated 524,288-byte payload, with successful
  STOP/unlock, zero output and the original error latch. The sheet did not
  fully eject; the user manually removed and reloaded it. Independent status
  after removal was error 0/lock 0. This is a protocol recovery result, not
  assistance-free ejection. A separate normal fresh-handle duplex scan then
  passed complete images, empty reopen and user-confirmed final ejection.
- One injected zero-byte timeout in the second front-side USB payload also
  recovered its known 524,288 bytes with successful STOP/unlock, zero output
  and the original error latch. Final ejection again did not complete, requiring
  manual removal. Independent status after removal was error 0/lock 0.
  These early-fault ejection failures remain unresolved.
- One injected zero-byte timeout in the first back-side USB payload, after
  the front completed, recovered exactly 524,288 pending bytes before
  STOP/unlock. The user confirmed full final ejection. A separately reloaded
  fresh-handle duplex scan passed complete images, empty reopen and confirmed
  final ejection. Both independent status checks were error 0/lock 0.
- One injected zero-byte timeout before the final USB completion record
  recovered the 64-byte event-4 record before STOP/unlock. Another injection
  in the final back payload recovered its 410,359 bytes and the completion
  record before cleanup. Both failed jobs stayed error-latched with zero
  output and no automatic retry/reset. Each had user-confirmed final ejection,
  independent error 0/lock 0 and a separate successful fresh-handle normal
  duplex scan with confirmed final ejection and empty reopen.
- Final multipage Color/300-dpi duplex batches through localhost-only saned
  returned ten complete images (five duplex pairs) over each transport.
  USB finished in 19.76 seconds and Ethernet in 12.29 seconds. Guarded reads,
  RGB8 stride/declared byte counts, EOF, positive feeder exhaustion and clean
  client/server exits passed, with no saved images. Independent pre/post
  error and lock were zero. The user reported paper passage after both runs.
  No independent starting sheet count or printed-label inspection was made
  for these final batches.

Raw images, personal document content, captures and hardware logs are not
distributed. Label inspection used only green-label masks.

## Future Work And Review

By request, the current hardware campaign ended after the final USB and
Ethernet multipage duplex checks on November 2, 2026, apart from the targeted
November 3 follow-ups recorded above. The additional checks below are deferred
to future work, not recorded as passes. Successful normal duplex scans do not
resolve the earlier fault/ejection failures or validate every exposed option.
Human code/license review and maintainer decisions on integration and
acceptable scope remain
necessary for upstream acceptance. A development review request may discuss
this limited tested scope:

- USB600 startup/ejection reliability and relevant fault recovery.
- Additional shared-USB modes, first-sheet compatibility and fault behavior;
  earlier direct-libusb results do not establish these on the current adapter.
- Early USB image-fault ejection, physical partial-transfer faults and loaded
  recovery on the original faulted handle.
- LAN front/back-only and optional back-rotation label checks.
- Calibrated window/offset geometry and actual crop/deskew/blank-filter effects.
- Moving-paper cancellation, USB cover handling and broader fault recovery.
- Relevant GUI frontend behavior.

USB event-2 continuation and the alternate LAN START-image-prefix path have
fixture coverage only. Short-header-only streams without a full
side-identifying header are unsupported; side identity is never guessed.
LAN discovery/encryption, configuration changes, calibration, maintenance
resets, NVRAM writes and firmware upload are absent. An internal read-only
NVRAM primitive is exercised only by a noninstalled test utility.

Source and synthetic fixtures use GPLv2-or-later with the SANE linking
exception; see `backend/sp1130n-license.txt`. The original MIT notice for
previously published code is retained, and existing SANE notices remain
intact. Protocol investigation included inspection of decompiled
vendor code using REA and informed development, including the USB event-2
continuation correction. AI-assisted implementation and review included
Claude. No formal clean-room separation or legal clearance is claimed. Vendor packages,
decompiler/assembly listings, analysis databases, private research and
credentials are excluded from the contribution. The public interoperability
reference was `namp/sp-1130n-macos` at
`626237c7bd26f53bc111adf191b63a73d249ebfa`; its hardware results do not
validate this backend.

Official upstream review is a GitLab merge request to
`sane-project/backends:master`; a GitHub comparison
or PR into this repository is not an upstream submission. Review licensing
and any contributor certification before submitting. No sign-off is invented.
