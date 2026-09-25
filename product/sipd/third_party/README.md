# Third-party dependency policy

The project has no distribution-approved third-party source dependency yet. In
particular, the `sip.h` interface is a stable local boundary, not an endorsement
of a specific SIP library.

Before a dependency is acquired or linked, update `manifest.json` and the
[dependency gate](../docs/governance/sip-dependency-gate.md) with an immutable
source hash, license decision, CVE snapshot, minimal feature set, host and ARM
results, and complete dynamic and static dependency closure. Builds must
consume already-verified inputs and must not download source, packages, or
toolchains implicitly.

The extracted LS-200 rootfs remains immutable evidence. Reconstructed sysroots,
pkg-config metadata, build output, source archives, and reports belong outside
that evidence tree.

PJSIP is pinned for an optional, externally acquired adapter build at
`b7317ffcaf1d20a38eb231cf15b7b65c4734dbb4` from the official
[`pjproject`](https://github.com/pjsip/pjproject) repository. Its source headers
state GPL-2.0-or-later; commercial licensing is also available. The distribution
decision remains a gate. The official commit archive was retrieved on
2026-08-26 and verified as SHA-256
`2d70c0b0cb49c7ca071a46179e9a0a9af28371a2585ac085da9e59e2dab3589b`.
The default CMake
configuration does not find, download, vendor, or link PJSIP. An authorized
reviewer must provide both `LS200_SIPD_PJSIP_INCLUDE_DIR` and
`LS200_SIPD_PJSIP_LIBRARIES` explicitly when enabling it, plus exact
`LS200_SIPD_PJSIP_UA_LIBRARY` and minimal `LS200_SIPD_PJMEDIA_LIBRARY`
archives for invite/session-timer support.
The 2026-08-27 host closure uses only low-level PJSIP and passes 13/13 CTests,
including UDP, authenticated TLS hostname acceptance/rejection, and SHA-256
digest proxy registration. That result does not approve distribution, real
provider credentials, SRTP, ARM, or Zoom use.

The native AAC path is likewise opt-in. Enabling either FAAD2 or SpeexDSP
requires enabling both and supplying exact include and library paths. The
locked FAAD2 2.11.2 and SpeexDSP 1.2.1 archives match the root dependency lock
and pass a 10/10 host suite with a persistent AAC-LC decode/downmix/resampling
oracle. CMake performs no package discovery or download. ARM libraries must be
separately cross-built and installed into the reviewed development sysroot.

SIPp 3.7.7 is the currently observed private-lab candidate. Its upstream tag,
release commit, license, and retrieval date are recorded in `manifest.json`,
but it is deliberately blocked until the exact release archive is fetched in
an authorized dependency-acquisition step and its SHA-256 and license text are
reviewed. The private-lab runner fails closed while that hash is absent.
