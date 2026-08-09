# Upstream sources used during this work

`reference/` itself is not published — it is several GiB of third-party source
carrying its own licences.  This is the inventory of what was fetched, pinned to
exact commits, so the investigation can be reproduced.


Downloaded on 2026-08-04 for read-only investigation of a PSM2/OFI
user-space bandwidth path.  The libfabric revision is deliberately matched to
the installed Ubuntu package version, 1.17.0-3build2.  No build output is
retained here.  The one exception is the checksummed, full Ubuntu OEM kernel
source package documented below, retained for the exact T7610 ABI.

| Local files | Upstream source | Pinned commit | License |
| --- | --- | --- | --- |
| `psm2/README`, `psm2/COPYING`, `psm2/psm_hal_gen1/hfi1_deprecated_gen1.h` | https://github.com/cornelisnetworks/opa-psm2 | `be99e35465b0de6a37d9c30408256f1dc5b12ea6` | BSD 3-Clause OR GPL-2.0-only; `psm2/COPYING` |
| `opa-psm2-source/` (depth-one, no-tag source clone) | https://github.com/cornelisnetworks/opa-psm2 | `be99e35465b0de6a37d9c30408256f1dc5b12ea6` (upstream `HEAD` resolved on 2026-08-04) | BSD 3-Clause OR GPL-2.0-only; `opa-psm2-source/COPYING` |
| `opa-hfi1-10.11.0.1/README`, `opa-hfi1-10.11.0.1/LICENSE`, `opa-hfi1-10.11.0.1/files/ifs-kernel-updates.spec.{rhel,sles}` | https://github.com/cornelisnetworks/opa-hfi1 | `bfb3da47c6f90d89015c7c7ce01c88a4e7581c7c` (branch `opa-10_11_0_1`; published OPXS 10.11.0.1.2) | BSD 3-Clause OR GPL-2.0-only; `opa-hfi1-10.11.0.1/LICENSE` |
| `opa-hfi1-10.11.0.1-source/` (depth-one, filtered sparse clone; retained `drivers/infiniband/hw/hfi1/`, `include/uapi/rdma/hfi/`, `README`, and `LICENSE`) | https://github.com/cornelisnetworks/opa-hfi1 | `bfb3da47c6f90d89015c7c7ce01c88a4e7581c7c` (branch `opa-10_11_0_1`; published OPXS 10.11.0.1.2) | BSD 3-Clause OR GPL-2.0-only; `opa-hfi1-10.11.0.1-source/LICENSE` |
| `linux-v6.17/include/uapi/rdma/hfi/hfi1_user.h` | https://github.com/torvalds/linux | `e5f0a698b34ed76002dc5cff3804a61c80233a7a` (tag `v6.17`) | GPL-2.0 WITH Linux-syscall-note OR BSD-3-Clause; SPDX header in file |
| `linux-hfi1-v6.17/drivers/infiniband/hw/hfi1/`, `linux-hfi1-v6.17/include/uapi/rdma/hfi/`, and directly required RDMA UAPI headers | https://github.com/torvalds/linux | `e5f0a698b34ed76002dc5cff3804a61c80233a7a` (tag `v6.17`; shallow filtered sparse clone) | HFI1 driver files are GPL-2.0 OR BSD-3-Clause; HFI1 UAPI files are GPL-2.0 WITH Linux-syscall-note OR BSD-3-Clause; SPDX headers in retained files |
| `linux-oem-6.17-6.17.0-1030.30/archive/` and `source/` (official Ubuntu source package, extracted) | https://archive.ubuntu.com/ubuntu/pool/main/l/linux-oem-6.17/ | Ubuntu source package `linux-oem-6.17` `6.17.0-1030.30`; immutable archive SHA-256 inputs `a5623ec5…f4394e3` (orig) and `dd24b563…c457ad` (Ubuntu diff); VCS declaration is Launchpad `noble`/`oem-6.17` but no Git object is encoded in the DSC | GPL-2.0 WITH Linux-syscall-note; extracted `source/COPYING` |
| `linux-hfi1-v6.17/.git` tag `hfi1-dma-mmap-1ec82317a1da` (comparison commit and parent retained, no worktree checkout) | https://github.com/torvalds/linux/commit/1ec82317a1daac78c04b0c15af89018ccf9fa2b7 | `1ec82317a1daac78c04b0c15af89018ccf9fa2b7` (`IB/hfi1: Use dma_mmap_coherent for matching buffers`) | GPL-2.0 OR BSD-3-Clause; SPDX header in the retained HFI1 driver file |
| `libfabric-main/README.md`, `libfabric-main/man/fi_opx.7.md`, `libfabric-main/COPYING` | https://github.com/ofiwg/libfabric | `65c70e234713970be1b9a1d2b1e7448bdec53255` | BSD 2-Clause OR GPL-2.0-only; `libfabric-main/COPYING` |
| `libfabric-v1.17.0/README`, `libfabric-v1.17.0/COPYING` | https://github.com/ofiwg/libfabric | `4a7d3ad0a4dfbb25bfc37cf91a038edd33f6dbc2` (tag `v1.17.0`) | BSD 2-Clause OR GPL-2.0-only; `libfabric-v1.17.0/COPYING` |
| `libfabric-v1.17.0/fabtests/man/man1/fi_rdm_tagged_bw.1`, `libfabric-v1.17.0/fabtests/benchmarks/rdm_tagged_bw.c` | https://github.com/ofiwg/libfabric/tree/4a7d3ad0a4dfbb25bfc37cf91a038edd33f6dbc2/fabtests | `4a7d3ad0a4dfbb25bfc37cf91a038edd33f6dbc2` (tag `v1.17.0`) | Project license above; the benchmark source also carries a BSD notice. |
| `libfabric-v1.17.0/man/fi_psm2.7.md` | https://github.com/ofiwg/libfabric/blob/4a7d3ad0a4dfbb25bfc37cf91a038edd33f6dbc2/man/fi_psm2.7.md | `4a7d3ad0a4dfbb25bfc37cf91a038edd33f6dbc2` (tag `v1.17.0`) | Project license above; `libfabric-v1.17.0/COPYING`. |

The GitHub blob URLs used for the exact files are encoded by the repository,
commit, and relative paths above.  The `.1` file is a `man7/fabtests.7`
alias; its implemented test semantics are preserved in
`rdm_tagged_bw.c`.

`psm2-hfi1-userspace-audit.md` records the endpoint-open call path and the
token-to-`mmap64()` requests for the pinned PSM2 source.  The accompanying
diagnostic patch is retained under `../patches/`; it is intentionally not
applied to the reference clone.
