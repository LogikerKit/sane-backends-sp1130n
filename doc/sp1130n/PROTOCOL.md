# SP-1130N Protocol

This document describes the protocol implemented by the SP-1130N backend.
Hardware validation is incomplete; only the profiles recorded below have
limited physical results.

## Framing

The request header is 16 little-endian bytes. Offsets:

| Offset | Meaning |
| ---: | --- |
| 0 | 32-bit magic `0x230` |
| 4 | Operation byte |
| 5 | Header flag, zero in the supported mode |
| 6 | Sub-operation byte |
| 7 | Header flag, zero in the supported mode |
| 8 | 16-bit selector |
| 10, 11 | Header flags, zero in the supported mode |
| 12 | 32-bit value, often transfer length |

Ordinary controls return 32 bytes. Extended inquiry returns 108 bytes.
USB image-block replies have 64 status bytes followed by declared JPEG bytes.
The code explicitly decodes integers and handles partial transfers.

## Command Sequence

| Operation/sub-operation | Use |
| --- | --- |
| `12/7a`, selector 108 | Extended inquiry; lock indication at reply offset 32 |
| `2a/78`, selector 1, length 40 | Write temporary client UUID lock |
| `28/78`, selector 1, length 40 | Read lock echo; require an exact match |
| `12/02`, selector 32 | Paper/cover status |
| `2a/09`, selector 4, length 260 | Write acquisition parameters |
| `14/05` | Prepare |
| `14/04` | Start acquisition |
| `28/04` | Request an image block |
| `14/06` | Stop |
| `2a/78`, selector 2, length 40 | Release this client's lock |
| `28/10`, selector 0, length 288 | Read-only opaque NVRAM snapshot |

The USB 260-byte parameter record selects source 4 for duplex, 5 for back, 6 for
front. The backend encodes RGB depth, geometry and 300/600-dpi fields.
USB image status byte 11 is 1 for front and 0 for back. Offsets 16 and 20 report
height/width; offset 36 declares compressed length; byte 48 describes the JPEG
form. Status values `0x1005`/`0x1006` finish a side. Direct completion records
follow finished sides without another block request.
Completion byte 6 values `2` and `4` do not themselves mean no paper.
Event `2` keeps the current acquisition running: the next sheet is read
with further `28/04` block requests, without resending parameters, prepare
or START. Event `4` ends this acquisition with bounded STOP/unlock. Any
failure, decode error or cancellation after event `2` sends STOP before
unlock. Event `2` has never been observed on the tested Fujitsu device
(every recorded USB completion was event `4`); its handling has replay
coverage only.
Another SANE sheet start may perform normal status/acquisition when paper
remains. Only explicit empty paper status latches no documents.
The event-4 hardware observation had both image-side statuses `0x1005`
and occurred after only the first of two loaded sheets.
Corrected two-sheet 300/600-dpi batches returned four complete images and
explicit no-paper status after event `4` on each sheet; the user confirmed
clean passage at 300 dpi and reported success on the first 600-dpi run.
A repeated 600-dpi batch returned complete images and no documents but left
the last sheet partly stuck according to the user. Completion, STOP/unlock
acknowledgements and empty-input status do not establish physical ejection.
The ejection cause and reliability remain unresolved; event-2 continuation
has replay coverage only. No unverified delay or motor command was added.
A subsequent user-confirmed face-down/head-first USB Color/300-dpi labeled
two-sheet test returned FRONT 1, BACK 1, FRONT 2, BACK 2, all upright.
Completion event4 and successful STOP/unlock followed each pair, then an
independent empty query ended the batch. Pre/post error/lock were zero and
the user reported ejection/reload. This validates native side interpretation
for that profile, not 600-dpi ejection reliability or event2 continuation.
A later native USB Color/300-dpi test completed two two-sheet batches on
one SANE handle. Positive empty status and cancellation preceded an explicit
user reload gate; batch two preserved mode without reopening. Both batches
returned four complete sides with exact byte counts/EOF, event4 and
successful STOP/unlock per sheet, followed by positive empty status.
Post-job error/lock were zero and the user subsequently confirmed full
assistance-free ejection in both batches. This validates limited loaded
restart after exhaustion/cancel, not earlier600-dpi physical failures.
A later native USB front/back-only pair of two-sheet Color/300-dpi tests
returned upright FRONT1/FRONT2 and BACK1/BACK2 respectively, with positive
exhaustion, successful cleanup, clear status and user-confirmed ejection.
A separate duplex test with back rotation enabled preserved
FRONT1/BACK1/FRONT2/BACK2 order, kept fronts upright and rotated only backs
180 degrees. All four images were intact, followed by positive exhaustion
and clear status; physical ejection for this rotation run needs confirmation.
These limited label checks do not validate LAN source/rotation profiles.
Advertised error payloads, including those in direct completion records, are
consumed with a bounded deadline before STOP or unlock acknowledgements are
read. The bounded UUID acquisition handshake completes ownership verification
before honoring cancellation. Other in-flight USB commands and their associated
data phases also finish before cancellation is honored; a completed final side
consumes its direct completion records before cleanup. No next image-block
request is sent once cancellation is observed at a transaction boundary.
The USB image phase, including final records and payload drains, shares a
120-second total deadline. The first image-header wait allows thirty seconds
for initial readiness; later headers allow five seconds. The initial wait
was extended after a successful 600-dpi header took approximately 4.4 seconds
and other attempts failed near the old five-second limit. Synthetic regressions
and one physical two-sheet 600-dpi trial passed with the extended allowance.
That trial's initial header waits were approximately 3.66 and 3.06 seconds,
both within the old limit, so this remains a bounded mitigation rather than
a confirmed fix for the intermittent failure.
STOP/unlock has an additional timeout budget of
25 seconds; cancellation is therefore deferred rather than instantaneous.
Valid matching JPEG geometry and restart metadata are preserved; row-based
restart repair is applied only when the supplied geometry replaces the JPEG
dimensions.

The SANE interface applies a separate USB loading policy after acquisition.
With optional upright-loading enabled on USB, logical front/back selection
is reversed, duplex pages are exchanged and both images are rotated
180 degrees. The logical acquisition window is mirrored within the configured
paper dimensions. Back-rotate adds rotation to the logical back only.
Upright-loading defaults off to match documented face-down loading and retain
native handling. These host-side operations
do not reinterpret the wire side identifiers or change physical sheet order.
The noninstalled protocol test CLI retains native handling; the loading
policy is a SANE option, not a wire-protocol mode. LAN is unchanged.

Normal scans do not perform maintenance/date initialization.
No NVRAM write, calibration, firmware upload, reset or restore is implemented.
The USB adapter uses SANE's shared `sanei_usb` functions. Its bulk
API hides timeout-specific status and discards partial counts on error, and
the current helper clears the endpoint halt on failure. Automatic inquiry
and image-tail recovery are consequently disabled in this adapter; failures
block further transfers until close, without proving scanner synchronization.
The optional protocol recovery mechanism and physical results described
next apply to the previous direct-libusb adapter, not the current shared-helper
adapter. A November 3, 2026 two-sheet Color/300-dpi duplex batch passed with
four complete images, event-4 STOP/unlock, positive feeder exhaustion, idle
status and user-confirmed full ejection. Other shared-USB modes and fault
recovery remain unvalidated on hardware.

USB transport failures can leave late or partial replies on the shared input
endpoint. A timeout while receiving the fixed 108-byte `12/7a` inquiry reply
gets a bounded attempt to consume only its known remaining bytes, within one
additional second. Bytes transferred on the failed call are counted, including
a full reply reported with a timeout. No request is replayed; the original I/O
error remains latched and a new job requires explicit cancellation.
For image-input timeouts, the accepted block's fixed header and declared
payload tail are consumed with a separate five-second total recovery budget.
Partial counts from the failed transfer are preserved; a full transferred
buffer reported with a timeout does not cause a duplicate read. When the
last side completes, its unsolicited completion records must also be accounted
for before cleanup. Recovery never requests another image block or START.
The USB connection stays blocked throughout tail consumption; only a complete
valid transaction boundary permits STOP/unlock. The failed job still reports
I/O error, exposes no partial images and requires explicit cancellation before
a new acquisition. Synthetic tests cover successful and failed recovery;
one injected first-payload timeout during a real Color/300-dpi scan passed
exact tail consumption, STOP/unlock and independent idle status. A reloaded
fresh-handle duplex scan then passed without requiring a reset or power cycle.
The user reported paper passage in both runs. This does not validate spontaneous
USB failures, physical partial-transfer faults or loaded recovery on the same handle.
Subsequent first-header and second-front-payload injections also passed exact
transaction accounting and STOP/unlock, but neither fully ejected the sheet;
manual removal was required. A normal fresh-handle duplex scan after clearance
of the first-header test passed with confirmed final ejection.
Subsequent zero-byte injections in the first back payload, final back payload
and final completion read passed transaction accounting, STOP/unlock and the
original error latch, with no partial output or automatic retry/reset.
The first back payload recovery consumed 524,288 bytes. Final back payload
recovery consumed 410,359 bytes plus the 64-byte event-4 completion record;
the separate completion-read fault recovered only that record.
The user confirmed full final ejection for each, and separately reloaded
fresh-handle normal duplex scans passed complete images and empty reopen.
Independent status was error 0/lock 0 after each fault and normal scan.
These are limited injected-fault results, not general mechanical recovery.
The earlier early-fault ejection failures remain unresolved.
Acknowledged STOP/unlock alone does not prove physical ejection.
This recovery is not applied to OUT failures, other control replies, stalls,
disconnects or timeouts while discarding a device-error payload.
A failed recovery or another USB transfer error blocks further
transfers on that connection, including cleanup commands that could otherwise
misinterpret old data as their acknowledgements. Closing a blocked connection
does not establish scanner-side synchronization or release a device lock.
General draining and reset are not implemented. Cancel/reopen clears host
state, not necessarily scanner transport
state. Do not infer reliable fault recovery from a STOP/unlock acknowledgement.

## Frontend First-Sheet Compatibility

`batch-scan` is a host-side output policy, enabled by default. Disabling it
retains the first physical sheet and calls the existing acquisition loop to
consume and discard the remaining sheets until confirmed exhaustion. It
does not change scan parameters or add a motor command. USB event `2`
continues block reads on the same START; event `4` uses the existing
cleanup/new-acquisition sequence. LAN continues its persistent feeder job.
This deliberately consumes all loaded paper, rather than claiming a native
single-sheet feed mode.

The first start exposes no image until exhaustion and cleanup succeed.
The selected first front/back pair remains available through separate SANE
starts; later pairs are freed immediately. Cancellation or any later-sheet
error frees the retained sheet and follows the existing bounded cleanup.
Empty-feeder cleanup failures propagate as errors instead of being hidden
by NO_DOCS. Blank filtering affects only the retained first sheet.
The drain checks a 120-second monotonic deadline between acquisitions and
caps total acquired sheets at 100; each in-flight acquisition retains its
own bounded transaction waits. Exceeding either bound fails the job and
does not guarantee physical ejection. The new policy has synthetic coverage;
two USB Gray/300-dpi two-sheet CLI runs (duplex and front-only acquisition)
returned the first image after positive exhaustion, with user-confirmed
passage of both sheets. LAN hardware and installed-GUI checks of this policy
remain open; native single-sheet feeding has not been fixed.

## Native LAN

The default IPv4 TCP port is 16888.
Controls use fresh connections retained through associated data phases.
START normally returns an ordinary control reply, but may instead supply
the first 32 bytes of an image header. A recognized prefix has a supported
image status, a zero second word, a valid side and a nonzero bounded width.
In that case the START connection becomes the persistent image connection:
the remaining header and declared payload are read there without another
`28/04` request. Ordinary control metadata is not treated as a side identifier.
All repeated `28/04` requests for every sheet in one feeder job share a
persistent image connection. Prepare/start are sent only once. Each sheet is
buffered and front/back are returned as separate SANE images. The image socket
and this client's lock remain live between sheets, until the feeder ends or
the frontend cancels/closes. Option changes are refused during a live job.
Call cancel or reopen before starting a new feeder job. Failed jobs latch their
error until cancel/reopen; another start must not silently re-feed after a fault.

The SANE cancel entry point only sets lock-free atomic state so it remains
signal-safe. A per-handle worker checks this state every 100 ms and performs
the existing bounded cleanup under the backend state mutex when acquisition
is no longer running. Normal calls can also reap the request. This handles
frontends that cancel after one image and remain idle without calling
read/start/close again. Close waits for the worker; cancellation does not
request another image block, restart acquisition, or reset the device.

Cancellation during a payload read preserves the image socket long enough to
discard only the validated current payload's unread remainder. The byte count
includes fragments already received; consumption has a one-second total deadline
and a call limit, and sends no further image request. STOP and this client's
unlock then use their normal fresh control connections. Tail failure remains
diagnostic failure, not proof of cleanup; no generic draining, reset or scan
retry is attempted. SANE cancellation still returns no image data. Two
first-payload hardware retests passed after earlier immediate socket closure
caused failed cleanup; partial headers, physical stalled tails and other phases
remain unvalidated. This does not add automatic LAN timeout recovery.

LAN byte 11 is 1 for front and 0 for back, matching the USB profile. A
November 2, 2026 two-sheet capture with user-confirmed face-down loading
associated those tags with the FRONT/BACK labels; all four labels were upright.
The former reversed interpretation produced BACK 1, FRONT 1, BACK 2, FRONT 2.
The parser now places tag1 in the front slot and tag0 in the back slot,
independent of arrival order, without rotating images or reversing sheets.
A separately authorized face-down/head-first retest returned upright
FRONT 1, BACK 1, FRONT 2, BACK 2, validating corrected native duplex label
order for that run. Simplex and optional back rotation still need their own
labeled hardware checks. The user confirmed both sheets fully exited without
assistance and were reloaded; the later paper-present inquiry is consistent
with that reload, not an empty-feeder result.
The acquisition always requests duplex; front/back-only output discards the
unselected side on the host after validating both images. LAN has no required
unsolicited USB completion record. `0x1005` finishes a side, not the job;
`0x1006` finishes a side with no paper remaining, but both sides of the current
sheet are still required. A clean peer close at the next sheet's first header
after a complete sheet requires independent confirmed-empty paper status
before it means no documents. A zero-payload/zero-geometry
full record at the next sheet boundary may instead precede a close/reset or
failed send with no reply bytes. That record alone is not EOF: the backend
queries paper status on a fresh control connection and requires confirmed
empty status before returning no documents. Opaque boundary metadata need
not be zero. Paper-present/unknown status, firmware faults, generic resets,
timeouts, partial headers/payloads and a missing back remain errors.

LAN reads 32 bytes first. Only the specific prefix containing two little-endian
`0x80000` words denotes a complete 32-byte header with a 512 KiB payload;
ordinary image headers require the remaining 32 bytes. Effective bytes at
offset 36 take precedence, falling back to block bytes at offset 32 only when
effective bytes are zero. Geometry can be retained from preceding blocks of
the same side; width and height may arrive separately, but conflicting
nonzero geometry and excessive combined dimensions are rejected. Length/memory/deadline
bounds and strict JPEG decoding apply even to discarded sides. Short blocks
without metadata are buffered until a full header identifies the side.
If full headers omit either dimension, bounded JPEG SOF geometry is used and
checked against any already supplied dimension.
Streams that finish solely through JPEG EOI with no full side-identifying
header are not supported: the backend does not guess front/back identity.
The first image-prefix wait per sheet permits thirty seconds for startup,
as on USB; later prefixes and the remaining header use five-second waits.
The prefix wait is capped by the sheet's 120-second total deadline.
TCP cancellation remains interruptible during that wait. This does not
retry an acquisition, add a reset or establish timeout recovery.

The LAN acquisition profile remains full A4: origin zero, source 4,
byte 60 `0x75`, byte 67 `0x04`. The diagnostic CLI permits native 50-600-dpi
trials with width floor(210 * dpi * 10 / 254) and height
ceil(297 * dpi * 10 / 254), preserving 2480 by 3508 at 300 dpi.
The new-resolution CLI trials check actual returned raster dimensions before
host processing; mismatched widths and oversized heights are not resampled
or reported as success. Encoding/replay tests do not validate scanner feeding.
SANE still exposes USB 300/600 and LAN 300 until broader hardware qualification.
1200 dpi and custom LAN windows are not supported.
LAN checks device error codes rather than interpreting USB's sensor bitfield.
The observed loaded/empty `12/02` tags at offset 8 are
`0xc5000002`/`0xc5000000`. With no firmware error and zero height/width,
the recognized empty tag returns no documents before PREPARE/START.
The observed second word is `0x04000000`, not required to be zero.
Unknown tags are not treated as empty. A nonzero firmware error takes
precedence over the tag.
A direct orderly image-socket close at the start of a later sheet also
requires this inquiry before reporting exhaustion. Loaded or unknown paper
remains an I/O error; cover and firmware faults are preserved. An authored
regression covers empty/loaded/unknown/cover/jam replies, latching and cleanup.
An idle empty-feeder open/closed-cover comparison returned `0xc5000001`
with the cover open and `0xc5000000` after closing it, with zero error and
geometry in both replies. The observed open-cover tag now returns cover open
before parameter writes or PREPARE/START. Nonzero geometry with that tag is
rejected as a protocol error; firmware errors still take precedence.
Synthetic tests verify rejection without feed commands, error latching,
cancel/new-job recovery and cover reporting at a sheet boundary. A subsequent
physical empty-feeder LAN check returned cover open on first/repeated SANE
starts, zero bytes on inactive reads and successful unlock, with no
acquisition parameters or PREPARE/START sent. Post-check error/lock were
zero. After user-confirmed cover closure, a fresh SANE handle returned
no documents on first/repeated starts, zero-byte inactive reads and successful
unlock, without parameters or PREPARE/START; post-check status was empty with
error/lock zero. This is limited empty-feeder recovery after handle reopening,
not same-handle cover recovery, loaded recovery or validation of other cover tags.
At normal no-paper completion only unlock is sent; stop/unlock are bounded
abort cleanup, never issued between sheets. Normal cleanup and one confirmed
feeder exhaustion have physical results; transport-fault and cancellation
recovery still need validation.

The first physical full-A4 300-dpi duplex LAN trial failed at scan start
with I/O error and no images. Inquiry afterward reported `0x2002` and
lock zero, while a cleanup connection was refused. These observations do
not establish a mechanical jam, a valid feed profile, or successful
STOP/unlock acknowledgements. No protocol parameters were changed on the
basis of this failed trial.

A second authorized trial likewise returned no images and ended with
`0x2002`, lock zero. It read a START reply, closed that connection, then
failed after reading an image header on a new connection without consuming
a payload. Abort STOP was reset and unlock connection refused. The metadata
trace does not establish the contents of the START reply.
The subsequent START-prefix correction is supported by the public macOS
reference and authored regressions, not a successful physical retest.
No further feed was attempted while the read-only status still reported
`0x2002`. Selected debug header fields aid diagnosis without dumping payloads,
device identifiers or lock tokens. Feed parameters were not changed.

After the user cleared a confirmed jam, two two-sheet full-A4 300-dpi
duplex batches succeeded with four intact images each, successful STOP/unlock,
and error/lock zero afterward. The actual START replies were ordinary
control acknowledgements; the image headers supplied nonzero width and
provisional zero height until each side's final `0x1005` record.
The parser's independent-dimension handling is therefore physically exercised,
but the alternate START-prefix path remains fixture-only.
A zero-length/zero-geometry full record between sheets did not end the job;
the next request continued on the same image socket. Those initial runs did
not check labels; the later labeled native duplex result is recorded above.
Geometry calibration remains unverified.

An earlier empty-feeder start returned no image header before the five-second wait,
so the backend reported an I/O error. Cleanup succeeded and status remained
clear. After loaded/empty comparison and boundary/status corrections, a
two-sheet batch requesting six images returned four intact images and then
no documents, with independent empty-status confirmation and successful
unlock. Read-only status afterward reported error/lock zero.
A fresh empty start returned no documents in 0.05 seconds, before feeding.
Those runs did not check labels; the later labeled result is recorded above.
Repeated reliability, calibrated geometry and broader fault/cancellation
recovery remain outstanding.

The backend conservatively rejects every nonzero encryption indication at
extended inquiry byte 30 before locking or feeding. Encryption negotiation is
not implemented. The backend does not change scanner configuration.
This is not eSCL, WSD or the SANE net/saned protocol.

Synthetic fixtures test the implemented contract. Their passing behavior
cannot prove protocol completeness, document safety or firmware compatibility.
