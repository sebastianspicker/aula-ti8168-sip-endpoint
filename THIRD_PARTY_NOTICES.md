# Third-party notices and patch licensing

This repository does not vendor an upstream source tree, prebuilt third-party
library, firmware image or dependency package. Dependency names, versions,
hashes and license identifiers in manifests are build metadata, not copies of
those dependencies.

The repository does include three unified-diff files authored to adapt
separately obtained upstream source. A unified diff necessarily quotes limited
upstream context around each maintained change. The context remains subject to
the upstream project's rights and terms; the original changes are published
under compatible terms so that each patch can be applied to its named source.

| Patch | Upstream source | Applicable terms |
| --- | --- | --- |
| [`lab/qemu/patches/ti8168-mediaboard-model.patch`](lab/qemu/patches/ti8168-mediaboard-model.patch) | [QEMU](https://www.qemu.org/), pinned and verified by the QEMU build metadata | New TI8168 model units: `GPL-2.0-or-later`, as stated by their SPDX notices. Changes and context for QEMU's `hw/char/serial-mm.c` and `include/hw/char/serial-mm.h` retain the MIT-style Fabrice Bellard/Citrix terms and notices reproduced in `LICENSES/MIT-QEMU-serial-mm.txt`. Other upstream context retains its QEMU per-file terms. |
| [`dependencies/patches/pjproject-signaling-only.patch`](dependencies/patches/pjproject-signaling-only.patch) | [PJSIP/pjproject](https://github.com/pjsip/pjproject) at the commit recorded in `dependencies/dependency-lock.json` | `GPL-2.0-or-later`; applying the patch does not replace PJSIP's dual-license terms or its stated open-source third-party exception |
| [`dependencies/patches/nginx-aula-cross.patch`](dependencies/patches/nginx-aula-cross.patch) | [nginx](https://nginx.org/) at the release recorded in `dependencies/dependency-lock.json` | Original changes: Copyright (c) 2026 Sebastian J. Spicker, `BSD-2-Clause`; upstream context retains the Igor Sysoev and Nginx, Inc. copyright and disclaimer terms reproduced in `LICENSES/BSD-2-Clause.txt` |

The applicable GPL-2.0-or-later text, the QEMU `serial-mm` MIT-style terms, and
the BSD-2-Clause text with the nginx and maintainer notices are included under
[`LICENSES/`](LICENSES/). QEMU's license documentation is available at
<https://www.qemu.org/docs/master/about/license.html>, PJSIP's at
<https://docs.pjsip.org/en/latest/overview/license_pjsip.html>, and nginx's at
<https://nginx.org/LICENSE>.

## External build dependencies

The reviewed dependency inventory is
[`dependencies/dependency-lock.json`](dependencies/dependency-lock.json).
These packages are not vendored, and their presence in the inventory is not an
approval to redistribute them. If a future source or binary release contains
any dependency, that artifact must carry the dependency's exact applicable
license text, copyright notices, source offer or corresponding-source material
where required, and any other attribution required by that dependency.

In particular, PJSIP and FAAD2 are recorded as GPL-2.0-or-later paths; PJSIP
also offers alternative commercial licensing. FastCGI 2.4.7 uses the Open
Market license, which requires its notice to be included verbatim in a
distribution containing its source or object code. OpenSSL 3 uses Apache-2.0.
The BSD- and MIT-licensed dependencies retain their respective notice
requirements. The JavaScript lockfile records package resolution metadata but
does not vendor the resolved packages; a built web distribution still needs a
generated license and notice review.
