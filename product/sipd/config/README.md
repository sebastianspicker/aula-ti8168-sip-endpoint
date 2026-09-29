# Configuration contract

`aula-sipd.example.conf` is a safe template, not a runnable deployment file.
It intentionally has an empty SIP URI and credentials, uses the fixture
backend, disables local control, and disables public-network access.

The configuration parser is strict:

- sections and keys are an allowlist; unknown, repeated, and malformed entries
  are errors;
- booleans are lowercase `true` or `false`; numeric values are decimal and must
  fit their documented type; empty is permitted only for explicitly optional
  secrets and source fields;
- paths must be absolute, non-empty where required, and must not contain `..`,
  control characters, or symlink escapes after canonical validation;
- RTP minimum and maximum ports must form an even-port range with room for
  paired RTCP ports;
- media `security` is exactly `prefer_srtp`, `required`, or `plain_compat`.
  The compiled default is `prefer_srtp`; the inert example deliberately
  selects `plain_compat` for an isolated private-lab fixture. SRTP policies fail
  closed when the daemon was built without its explicit SRTP dependency;
- limits may only reduce or equal the compiled hard maxima in
  `include/aula_sipd/common.h`;
- TLS cannot be enabled without an approved certificate/name-validation plan;
- production configuration and secret files must be owned by the installation
  account and must not be group- or world-writable. Secret files additionally
  require mode `0600` or stricter.

Configuration loading creates an immutable per-call snapshot. Reloaded settings
apply only to a subsequent call. Diagnostics use the redacted formatter and
never print full SIP URIs, secret paths, credential values, or source details.
