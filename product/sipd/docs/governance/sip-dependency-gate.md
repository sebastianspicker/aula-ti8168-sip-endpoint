# SIP dependency gate

Status: exact source and host TLS closure recorded; distribution remains
blocked pending license approval, ARM closure, and unresolved advisory-feature
exclusions. This is a governance record, not a dependency approval.

## Current upstream evidence, refreshed 2026-08-27

- [PJSIP releases](https://github.com/pjsip/pjproject/releases): 2.17 was the
  latest observed release at retrieval.
- [PJSIP upstream repository](https://github.com/pjsip/pjproject): maintained
  source headers state GPL-2.0-or-later; commercial licensing is also
  advertised upstream.
- [PJSIP upstream advisories](https://github.com/pjsip/pjproject/security/advisories):
  multiple 2026 SIP/SDP issues list 2.17 or lower as affected with no patched
  release, including
  [GHSA-rfwg-w9gq-9mw2](https://github.com/pjsip/pjproject/security/advisories/GHSA-rfwg-w9gq-9mw2),
  [GHSA-277r-3q2j-mxcw](https://github.com/pjsip/pjproject/security/advisories/GHSA-277r-3q2j-mxcw),
  and
  [GHSA-wf9x-v65x-5qh3](https://github.com/pjsip/pjproject/security/advisories/GHSA-wf9x-v65x-5qh3).
  A release tag alone therefore cannot satisfy the security gate.
- The selected revision contains the published fixes `d6a0e7f`,
  `43d3bd77bb6833eab4c493503b8d564a754ddfdd`, `082948b`, `5311aee`,
  `673b978`, and `c82123e`; each was verified with
  `git merge-base --is-ancestor` against the official repository history.
- [GHSA-pvmg-ph43-54r2](https://github.com/pjsip/pjproject/security/advisories/GHSA-pvmg-ph43-54r2),
  [GHSA-cgmx-7mqj-77qp](https://github.com/pjsip/pjproject/security/advisories/GHSA-cgmx-7mqj-77qp),
  and
  [GHSA-pjx6-7vrm-3w6f](https://github.com/pjsip/pjproject/security/advisories/GHSA-pjx6-7vrm-3w6f)
  still list no patched version or published fix. The adapter uses the OS
  resolver rather than pjlib-util's asynchronous resolver, does not parse XML
  event bodies, and never forwards or re-serializes received multipart bodies.
  These exclusions reduce exposure; they do not turn an unpatched upstream
  source into an approved dependency.

## Gate decision

PJSIP is not approved for distribution, fetched, or vendored. A user-selected
post-2.17 source pin, `b7317ffcaf1d20a38eb231cf15b7b65c4734dbb4`, is recorded
for an optional external-build wrapper only. The default build has no PJSIP
lookup, retrieval, or link step. Approval requires all of the following:

1. explicit distribution-license approval;
2. review of the relevant post-2.17 security fix and immutable selected source
   revision, including SHA-256;
3. a dated CVE/advisory snapshot for the selected revision;
4. host and ARM EABI5 build evidence;
5. minimal feature configuration and static/dynamic dependency closure; and
6. a reviewed entry in `third_party/manifest.json` and this gate record.

The owned low-level adapter has been compiled and linked on the host against
the recorded revision. That does not establish trusted-device certificate
deployment, provider-issued proxy credentials, public TLS, Zoom acceptance,
target compatibility, or live SRTP behavior. ARM compatibility and live
interoperability remain separate gates.

The public `sip.h` contract intentionally exposes no PJSIP types, so selection
can remain blocked without preventing portable-core implementation.
