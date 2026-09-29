# TI8168 media board and comparison platforms

This workspace researches a **TI8168 media board**. Its manufacturer, exact PCB
layout, and commercial relationships are not established in the public source.
Documented DM8168 platforms below provide architectural comparisons, without
establishing hardware equivalence or deployment support.

Observations from privately held devices and firmware remain in the research
corpus with their own evidence and confidence limits. They inform bounded
models without establishing that the TI8168 media board exactly reproduces a
physical product.

## Documented comparison platforms

The following manufacturer-documented examples broaden the architectural
comparison. They are reference designs and products from the DM8168 ecosystem,
not proposed identities for the TI8168 media board. Here, TI8168 is a
platform-level description; retain each manufacturer's exact processor and
board names when comparing specifications.

| Platform | Documented hardware form | Possible comparison for the TI8168 media board |
| --- | --- | --- |
| **TI DM8168 EVM**, covered by TI's DM816x/C6A816x/AM389x EVM documentation | Evaluation baseboard with an expansion I/O daughtercard. [TI quick-start guide](https://software-dl.ti.com/dsps/dsps_public_sw/ezsdk/5_05_01_04/exports/DM816x_C6A816x_AM389x_EVM_Quick_start_guide.pdf). | A laboratory design with processor resources and peripheral connections exposed for evaluation. |
| **Z3 Technology Z3-DM8168-RPS** | Starter kit combining a Z3-DM816x-MOD-2x system-on-module card with a Z3-DM8168-APP-0x application/I/O board. [Z3 datasheet](https://z3technology.com/wordpress/wp-content/uploads/DOC-MKT-0004-04_Z3-DM8168-RPS_Data_Sheet.pdf). | A media-oriented design that separates processor resources from application-specific video and audio connections. |
| **iWave DM8168 Qseven SOM** | A DM8168 system-on-module using the Qseven form factor, rather than a complete standalone appliance board. [iWave product page](https://www.iwavesystems.com/product/dm8168-qseven-som/). | A modular design in which a carrier board could determine much of the external connectivity. |

The last column is an architectural interpretation for this project, not a
manufacturer compatibility claim. These references do not establish current
stock, ongoing support, or suitability for a new purchase. They also do not
establish a manufacturing relationship with any privately researched device.

TI's
[EZSDK 5.05 download page](https://software-dl.ti.com/dsps/dsps_public_sw/ezsdk/5_05_02_00/index_FDS.html)
provides the EVM quick-start guide and associated platform software. The
[DM8168 product page](https://www.ti.com/product/TMS320DM8168) provides processor
documentation.

## Using the comparisons

The comparison platforms include integrated boards, evaluation baseboards,
and processor modules on carriers. Models must state the interfaces and
assumptions they use without inferring this board's PCB from its processor
family.

For each comparison, keep processor variant and revision, memory layout,
pinmux, capture interfaces, audio routing, flash configuration, and boot
behavior separate. Shared processor ancestry alone does not establish firmware
compatibility. None of these examples is a supported deployment target for
this project. Maintained filenames use `aula` names, and the synthetic QEMU
machine is `ti8168-mediaboard`.

Platform similarity does not establish shared software ownership or permission
to redistribute SDKs, board-support code, or firmware. Original firmware,
decompiled implementations, vendor web assets, and vendor documents remain
private. Independently written models and integrations require provenance
review before [public source export](../PUBLIC_RELEASE.md).

The references above are external links and short factual summaries; vendor
manuals, schematics, SDKs, and source packages are not copied into the public
source tree.
