# Draft: Experimental Ricoh SP-1130N Backend

This is a draft for discussion with SANE maintainers. It has not been
submitted and is not a request to label the scanner hardware-supported.
Do not open a pull request or merge request until hardware testing is complete.
USB and LAN duplex are required before submission.

## Summary

Initial Linux-only port of the SP-1130N implementation into SANE's Autotools
layout. USB 05ca:031e and explicit-IPv4 native TCP 16888 are implemented.
Duplex is the default, with front/back-only output and optional host-side
processing. USB retains experimental 300/600 dpi and acquisition windows;
LAN is restricted to a full-A4 300-dpi acquisition profile.
LAN uses one image connection and one prepare/start for the whole feeder job,
with separate USB/LAN header parsing and side identifiers.
The backend is excluded from automatic build selection and remains commented
out in dll.conf. Scanning also requires SP1130N_EXPERIMENTAL=1.
The description is deliberately :untested.

## Licensing

New files retain MIT notices; existing upstream files retain their notices.
Please review backend placement and preferred contribution licensing.
No contributor certification or maintainer approval is claimed.

## Validation and Release Blockers

Synthetic tests cover protocol framing, partial reads, duplex ordering,
front/back selection, geometry, image transforms, cancellation, EOF, and
localhost TCP behavior using the real backend. Physical scanner validation has
not been performed.
Latest regressions also cover signal-handler cancellation without mutex
reentrancy, preservation of enumerated device storage, USB UUID cancellation,
queued USB error payloads (including direct completion records), cancellation
at USB command/data boundaries, reset-versus-orderly-EOF reporting, valid JPEG
restart intervals, and offline test-utility target/profile validation.
The corrected normal and backend UBSan suites passed.

Rebased and checked on October 29, 2026 against current master
`1052d69a6f5e9aab2032b1de27ab1f364c9b6553`.
The normal build and full `make check` passed. All seven SP-1130N groups,
including twenty localhost LAN cases, and upstream's new `net_test` passed
normally and in the UBSan preload build. The upstream scanimage/saned shell
scripts still skipped because the SANE test backend was not built. No physical
scanner test or full upstream UBSan rerun was performed.

Checked on October 25, 2026 against master
`982deaefc2479c117d2b8dba2200765134385490`: normal build and `make check`
passed, with the upstream scanimage/saned shell scripts skipped because the
test backend was not built. All seven new test entries passed, including twenty
LAN cases. The new backend's GNU C11/UBSan tests also passed with preloading
enabled, but the full upstream sanitizer check failed on a signed left shift
in unchanged `sanei/sanei_codec_bin.c:117` (`test_wire`). No full-suite UBSan
pass is claimed. The patch removes the duplicate preloaded `sanei_usb.lo`
link, already supplied by `libdll_preload.la`.

Automatic selection excluded this backend; explicit selection without USB
support failed. Review the final patch immediately before sharing.

Before release inclusion, obtain labeled USB/LAN duplex
and second-sheet scans, 600-dpi/geometry/orientation results, fault and
cancellation tests, and frontend behavior tests. LAN lock/status and abort
cleanup must be checked explicitly; 600 dpi and custom windows must not be
enabled on LAN until their feed profiles are validated.
The current LAN side-byte interpretation remains provisional; labeled
front/back-only and back-rotation results are required before treating it as
validated.

LAN discovery/encryption, address changes, vendor calibration, NVRAM
write/reset/restore and firmware upload are absent.
Please consider this an experimental development port, not a completed driver
or device-compatibility claim. Recheck the final contribution and obtain a
human review of the code, licensing and any contributor certification before
sending this draft.
