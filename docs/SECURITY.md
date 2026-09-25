# Security and disclosure boundary

## Project status

This worktree is a research and prototype workspace. It does not publish a
supported release line, security-support window, or public vulnerability
reporting channel. Do not send credentials, device identifiers, network
topology, captures, or exploit material through a public issue.

Coordinate sensitive reports through a private channel already established
with the repository maintainer or the affected vendor. Because no such channel
is defined in this repository, its address cannot be documented here.

## Known inherited firmware risk

Testing on one owned LS-200 running firmware `2.11.26.80` confirmed that an
authenticated vendor management endpoint can pass attacker-controlled text to
a root shell. The tested scope does not establish which other firmware versions
or AREC products are affected.

Until a vendor fix is obtained and verified:

- isolate the vendor management plane from untrusted networks;
- do not expose it to the public Internet;
- restrict management access to authorized operators;
- avoid the affected RTMP connectivity-test function; and
- monitor for unexpected management requests and shell children of the vendor
  web service.

The maintained nginx, FastCGI gateway, and `ls200-sipd` controls do not repair
or replace the vulnerable vendor management application.

## Maintained product boundaries

The product threat model is maintained in
[`product/sipd/docs/governance/threat-model.md`](../product/sipd/docs/governance/threat-model.md).
Important controls include strict configuration schemas, secret-file
indirection, no network control listener, Unix peer authentication, typed LSZ1
operations, bounded network/media parsers, exact Origin and CSRF validation,
role authorization, idempotency, and manifest-verified deployment.

The inert SIP example uses `plain_compat` only for an isolated private lab. The
daemon's compiled default is `prefer_srtp`, while public Zoom profiles require
TLS-shaped configuration and protected media. A configuration file or persisted
runtime setting can override compiled defaults, so operators must inspect the
effective authenticated status rather than infer protection from defaults.
Settings report TLS policy, not a completed certificate check. Client-supplied
and legacy persisted `verified` values never establish certificate verification;
the console reports that observation as unavailable.

Direct CRC calls over TLS with required SRTP trust the authenticated service to
supply media hosts distinct from its SIP host. This policy deliberately accepts
service-reported unicast IPv4 addresses and nonzero RTP/RTCP ports without a
provider-specific CIDR list. Local/private, link-local and multicast destinations
remain excluded by network-scope checks. An explicit operator media authorizer
takes precedence. This broadens the media-routing trust boundary; it does not
relax SIP certificate validation or media encryption.

A regional Direct CRC route can be supplied through static `sip.crc_address`.
It accepts one public IPv4 literal, while preserving `zoomcrc.com` for SIP and
TLS server identity. The route does not replace CA verification, resolved-peer
authorization, or the requirement for SRTP.

Payload installation requires an independently supplied manifest SHA-256. A
trusted installer-side verifier checks the manifest and each staged file before
activation; a verifier contained in the candidate payload is not a trust anchor.
A digest calculated from an untrusted candidate at install time is insufficient.

## Evidence handling

Follow the [sanitization policy](../product/sipd/docs/governance/sanitization-policy.md).
Complete firmware, extracted filesystems, raw captures, authenticated evidence,
credentials, certificates, keys, device identifiers, private addresses, and
security reproduction material belong only below ignored `evidence/private/`
or an equivalent approved private disclosure channel.

Public reports may contain bounded, sanitized technical conclusions. They must
not contain working exploit payloads, account names, device serials, private
topology, host-key fingerprints, raw protocol bodies, or customer media.

## Physical operations

Physical-device actions require an owned unit, recovery authority, a private
session file, independently reviewed hashes, and the repository's explicit
confirmation gates. Root SSH is a separately managed privileged substrate; the
project's install, rollback, removal, or owned-state purge does not remove or
decommission it. Do not claim restoration of the original security posture
while that substrate remains.
