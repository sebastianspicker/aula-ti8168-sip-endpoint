# Upgrade to Aula

The project is `aula-ti8168-sip-endpoint`. Code, executables and service
identities use the `aula` prefix. Public C consumers must rebuild against
`include/aula_sipd/`; the proprietary control-bridge API is no longer part of
the maintained product. LSZ1 framing and the product's account, job,
credential and runtime-settings JSON schemas are unchanged.

The synthetic emulator has a separate new state schema. Its old vendor
snapshots belong to the private compatibility snapshot and are not imported.
The QEMU machine is `ti8168-mediaboard`, with synthetic defaults; it does not
support the former recovered-image boot workflow.

## Existing installations

This is an explicit upgrade with a stopped service and reviewed state
restoration. There is no automatic in-place adoption of an earlier ownership
journal, account overlay, release tree or autostart configuration. Keep the
previous trusted installer/remover and its verified release available for
rollback. Do not run a renamed remover against an old installation.

The new installers refuse the following legacy paths, including dangling
symlinks, before creating new application state:

| Legacy path | New application path |
| --- | --- |
| `/opt/ls200-zoom` | `/opt/aula-ti8168-sip-endpoint` |
| `/var/lib/cbox/ls200-zoom` | `/var/lib/cbox/aula-ti8168-sip-endpoint` |
| `/run/ls200-zoom-state` | `/run/aula-state` |
| `/var/run/ls200-zoom-operation.lock` | `/var/run/aula-ti8168-sip-endpoint-operation.lock` |
| `ls200-sipd.conf` | `aula-sipd.conf` |
| `ls200-web`, `ls200-gateway`, `ls200-sip` | `aula-web`, `aula-gateway`, `aula-sip` |
| `LS200_*` environment variables | `AULA_*` environment variables |

An operator with separate authority for the target should follow these steps:

1. Stop the old services and disable their autostart using the old trusted
   tooling. Resolve any pending transaction using that version's recovery
   procedure. Hold its operation lock while inspecting or changing its state.
2. Back up configuration, runtime settings, account/directory records, job and
   credential stores, TLS material, service identities, ownership journals and
   the verified old release. Preserve file modes and numeric ownership. Check
   the backup before proceeding; keep it private and outside both active
   application trees.
3. Use the old remover and ownership journal to retire only its managed
   hooks, mounts and selectors. Preserve the backup. Retire residual old
   application paths and locks through the reviewed recovery procedure; do
   not rename journals or blindly replace strings in their contents.
4. Install the new verified package without starting it. Let it establish
   its own service identities, ownership journal and configuration templates.
   This source tree requires separately prepared target prerequisites; private
   provisioning tools are not shipped with the maintained source.
5. Restore application data into the new state tree. Account records, jobs,
   credential records and runtime settings retain their schemas and bytes.
   Apply the new service ownership and required modes: gateway state belongs
   to the gateway identity, SIP settings to the SIP identity, device state to
   root, and TLS state to the identities specified by the new installer.
   Stored device credentials remain unused while the hardware provider is
   unavailable.
6. Transfer operator settings into the new configuration templates. Review
   path-valued keys such as `pid_file`, `settings_file`, `unix_socket_path`,
   `control_socket_path` and `account_store_path`. Set `gateway_uid` and
   `expected_sipd_uid` to the newly established identities. Do not copy old
   UID/GID assumptions, journals, release symlinks, bootstrap transactions or
   autostart files into the new installation. Do not replace substrings in
   secrets, SIP identities, certificates or arbitrary stored data.
7. Validate the new configuration and ownership before separately authorizing
   service start. Keep the old backup until functional acceptance and rollback
   review are complete. Restore an old installation only with its old tooling
   and matching journal; never combine journals across names.

The host migration tests exercise refusal for each legacy path as a file,
directory and dangling symlink, twice, while verifying unchanged legacy
metadata and no new application directory. Existing product persistence tests
exercise the unchanged JSON schemas. These checks do not establish that an
operator's physical upgrade or restoration has succeeded.

## Deliberate compatibility spellings

The old names remain only in this migration document, the legacy-path guards
in the two installers and their migration test, and the private root-SSH
ownership-marker contract in the physical installer/remover, their tests, and
the offline reboot ownership checks in `tooling/live/campaign_transport.py`.
That contract retains `.ls200-root-shell-managed`,
`managed-by=open-ls200-root-shell.sh` and `ls200-root-ssh-…` cron markers so
existing privately provisioned overlays can still be identified exactly.
The checks do not create or modify provisioning markers. Their private
provisioner is outside the maintained source boundary.

Public-source approval records require review again after a path or byte
change. The source allowlist remains empty; the rename does not authorize
publication or copying the repository's Git history.
