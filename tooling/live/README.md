# Physical campaign tooling boundary

The maintained campaign CLI refuses every physical action before loading a
session, checking topology, or opening a transport. Its physical identity
provider is unavailable in this source tree. Hardware campaigns require
separately reviewed private identity tooling and action-specific authorization.

The session parser, build and package helpers, ownership checks, peer-pin and
recovery contracts remain for offline use. A synthetic session identity does
not prove that a physical target is eligible. Physical packaging still depends
on separately provisioned device readiness and root-SSH prerequisites. Those
inputs need independent provenance review.
The private root-SSH ownership-marker bytes in the offline reboot check are
retained for migration compatibility. See the
[physical private-lab workflow](../../docs/operator/physical-private-lab.md)
and [Aula upgrade guide](../../docs/operator/upgrade-to-aula.md).
