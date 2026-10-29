# SP-1130N Protocol

This document describes the protocol implemented by the experimental backend.
Hardware-unvalidated: these details still require physical scanner testing.

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
Advertised error payloads, including those in direct completion records, are
consumed with a bounded deadline before STOP or unlock acknowledgements are
read. The bounded UUID acquisition handshake completes ownership verification
before honoring cancellation. Other in-flight USB commands and their associated
data phases also finish before cancellation is honored; a completed final side
consumes its direct completion records before cleanup. No next image-block
request is sent once cancellation is observed at a transaction boundary.
The USB image phase, including final records and payload drains, shares a
120-second total deadline. STOP/unlock has an additional timeout budget of
25 seconds; cancellation is therefore deferred rather than instantaneous.
Valid matching JPEG geometry and restart metadata are preserved; row-based
restart repair is applied only when the supplied geometry replaces the JPEG
dimensions.

Normal scans do not perform maintenance/date initialization.
No NVRAM write, calibration, firmware upload, reset or restore is implemented.

## Native LAN

The default IPv4 TCP port is 16888.
Controls use fresh connections retained through associated data phases.
All repeated `28/04` requests for every sheet in one feeder job share a
persistent image connection. Prepare/start are sent only once. Each sheet is
buffered and front/back are returned as separate SANE images. The image socket
and this client's lock remain live between sheets, until the feeder ends or
the frontend cancels/closes. Option changes are refused during a live job.
Call cancel or reopen before starting a new feeder job. Failed jobs latch their
error until cancel/reopen; another start must not silently re-feed after a fault.

The provisional LAN interpretation of byte 11 is 0 for front and 1 for back,
unlike the USB profile. Labeled hardware tests must confirm that interpretation.
The acquisition always requests duplex; front/back-only output discards the
unselected side on the host after validating both images. LAN has no required
unsolicited USB completion record. `0x1005` finishes a side, not the job;
`0x1006` finishes a side with no paper remaining, but both sides of the current
sheet are still required. A clean peer close at the next sheet's first header
after a complete sheet means no documents. TCP resets, timeouts, send errors, partial
headers/payloads and a missing back remain errors, not end-of-batch success.

LAN reads 32 bytes first. Only the specific prefix containing two little-endian
`0x80000` words denotes a complete 32-byte header with a 512 KiB payload;
ordinary image headers require the remaining 32 bytes. Effective bytes at
offset 36 take precedence, falling back to block bytes at offset 32 only when
effective bytes are zero. Geometry can be retained from preceding blocks of
the same side, but conflicting geometry is rejected. Length/memory/deadline
bounds and strict JPEG decoding apply even to discarded sides. Short blocks
without metadata are buffered until a full header identifies the side.
If full headers omit geometry, bounded JPEG SOF geometry is used.
Streams that finish solely through JPEG EOI with no full side-identifying
header are not supported: the backend does not guess front/back identity.

The LAN acquisition profile is deliberately restricted to full A4 at 300 dpi:
2480 by 3508, origin zero, source 4, byte 60 `0x75`, byte 67 `0x04`.
Do not extrapolate feed-related parameters to 600 dpi or custom windows before
hardware testing. USB's experimental 300/600-dpi/window profile is unchanged.
LAN checks device error codes rather than interpreting USB's sensor bitfield.
At normal no-paper completion only unlock is sent; stop/unlock are bounded
abort cleanup, never issued between sheets. Lock/status and abort behavior
still need physical LAN validation.

The backend conservatively rejects every nonzero encryption indication at
extended inquiry byte 30 before locking or feeding. Encryption negotiation is
not implemented. The backend does not change scanner configuration.
This is not eSCL, WSD or the SANE net/saned protocol.

Synthetic fixtures test the implemented contract. Their passing behavior
cannot prove protocol completeness, document safety or firmware compatibility.
