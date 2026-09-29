# Device-control companion

The C17 companion provides a protected Unix socket, a strict projected status
schema, a durable job journal, and revisioned credential state. It does not
contact firmware. Its default status reports `recording` and `streaming` as
`unknown`; credential connection state is `unknown`. The former firmware
HTTP/login and raw recorder mapping implementations are preserved only in the
private source snapshot under `evidence/private/vendor-compat/`.

From the repository root, run `make -C product/console device-control test`.
The executable is `.work/build/console/aula-device-control`. Product builds
consume explicit reviewed dependencies and write outputs below `.work/`.

## Local contract

The gateway connects to `/run/aula-device/control.sock`, verifies a root peer,
and sends one bounded JSON request per Unix connection. The companion accepts
only the configured gateway UID. The launcher checks socket ownership, type,
permissions, inode stability, and a lifetime lock before accepting or
recovering a stale socket. Frames contain a four-byte big-endian length and
at most 4096 bytes of UTF-8 JSON. Duplicate and unknown fields are rejected.

Revision 1 accepts `{"revision":1,"operation":"status"}` and returns exactly
`revision`, `recording`, and `streaming`. Each activity is `active`, `inactive`,
`paused`, or `unknown`. Revision 2 accepts correlated `status`, `jobs.list`,
`credentials.status`, and `credentials.replace` operations. It replies with
`revision`, `operation`, `correlation`, `outcome`, and `data`. Correlations are
bounded ASCII identifiers. The gateway validates responses before browser
exposure. The companion validates the three-field status shape before sending
it, including any future provider's response. No raw device response schema is
accepted at this boundary.

The default status provider is unavailable, so no recording or stream activity
is inferred from hardware. The job and credential APIs retain their persistent
schemas and idempotency receipts. `jobs.list` exposes only bounded safe fields;
there is no job worker or browser mutation workflow. Credentials remain
write-only and do not alter console accounts or SIP credentials. Existing
credential files remain under the protected deployment state path; the current
schema and file names are retained for compatibility until a separate migration.

The installer provisions `device/` below persistent deployment state as a
root-owned mode-0700 directory. The companion validates ancestors, opens an
exclusive lifetime lock, and rejects unsafe, incompatible, or concurrently
owned journal and credential files. Writes use owner-only temporary files,
file synchronization, atomic rename, and directory synchronization. A failed
write disables further dispatch until restart. Accepted jobs and receipts are
retained through replacement and rollback.

This separation intentionally removes the old firmware HTTP/login client and
raw recorder projection; operators must not treat the safe `unknown` status as
an observation of the TI8168 media board. The SIP daemon LSZ1 interface is
independent of the companion.
