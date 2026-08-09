# PSM2/HFI1 mapping-fix history

## 2026-08-04 — investigation opened

- Confirmed that PSM2 library loading and `psm2_init` succeed on T7610, while local `psm2_ep_open` causes a kernel `Corrupted page table` / `Bad pagetable` Oops.
- Reproduced the failure on Ubuntu kernels 6.17 OEM and 6.8 generic, with distribution and current upstream PSM2 builds.
- User authorized source-level work on the GitHub PSM2 and Linux HFI1 implementations, with physical recovery available for T7610 experiments.

## 2026-08-04 — single-run mapping capture

- Fixed PSM2 at `be99e35465b0de6a37d9c30408256f1dc5b12ea6` was instrumented with an opt-in map trace and built in an isolated tree. The upstream clone was not modified.
- A guarded, one-run T7610 capture correlated PSM2 trace, `strace`, HFI1 function trace, kernel journal, and pstore. Context assignment, context info, and user info ioctls all returned zero.
- The first failure is the read touch of `sc_credits_addr`: PSM2 mapped token `0xdabbad00030b02d8` at page-aligned offset `0xdabbad00030b0000`, delta `0x2d8`, length 4096, `PROT_READ`, into virtual address `0x76d3fdd55000`. `mmap` returned that address, then the first touch caused `Corrupted page table` / `Bad pagetable`; the Oops CR2/RBX matched the mapping address.
- This narrows the repair investigation to the HFI1 send-context credits mapping. The capture remains on T7610 under `/var/tmp/opa-psm2-fault-trace/runs/20260804T132345Z-U7Zy5a/`; the one-run fuse is consumed and no reproduction is repeated before a kernel-side hypothesis is ready.

## 2026-08-04 — credits-mapping candidate and PSM2 admission

- The failing token is HFI1 type 3 `PIO_CRED`. It maps a page in the NUMA-local coherent credit-return array. Upstream commit `1ec82317a1da` changed that type from legacy PFN/`io_remap_pfn_range()` mapping to `dma_mmap_coherent()`; the candidate patch restores the former only for type 3.
- The candidate was built as a temporary module against the exact Ubuntu OEM 6.17.0-1030.30 source, headers, configuration, and Module.symvers. Its vermagic and all 401 modversion CRCs match the stock module. It is never installed under `/lib/modules`; a normal reboot restores stock HFI1.
- The patched module is admitted and links normally on T7610 when its declared dependencies are preloaded. It registers the RDMA HCA as `opap129s0`; the kernel module identity is verified by its hash/srcversion rather than that non-stable RDMA name.
- PSM2 initially rejected that name before any HFI ioctl. A process-only 0700 `HFI_SYSFS_PATH` shim exposing `hfi1_0` as a symlink to the active `opap129s0` made patched PSM2 `psm2_init` and `psm2_finalize` succeed without HFI syscall or kernel fault. The shim was deleted and stock HFI1 reboot recovery verified. This is an admission workaround only; endpoint open still requires a new guarded test.

## 2026-08-04 — Linux HFI1 mmap source audit

### Pinned source

- Retained a shallow, filtered sparse clone at
  `reference/linux-hfi1-v6.17/`, fixed at Linux tag `v6.17`, commit
  `e5f0a698b34ed76002dc5cff3804a61c80233a7a`.
- The retained paths are `drivers/infiniband/hw/hfi1/`,
  `include/uapi/rdma/hfi/`, and directly included RDMA UAPI headers.  The
  source/license/commit inventory is in `reference/README.md`.
- This audit is against upstream v6.17 source.  The one T7610 probe must
  still identify the exact OEM module build from `modinfo hfi1` before a
  kernel patch is selected.

### Confirmed control and data path

1. PSM2 reaches `map_hfi_mem()` only after `ASSIGN_CTXT`, `CTXT_INFO`, and
   `USER_INFO`: `psm2_ep_open` → `psmi_ep_open_device` →
   `psmi_context_open` → `hfp_gen1_context_open` →
   `hfi_userinit_internal()` → `map_hfi_mem()`.
2. In `file_ops.c`, `hfi1_file_ioctl()` dispatches those three requests to
   `assign_ctxt()`, `get_ctxt_info()`, and `get_base_info()` respectively
   (lines 185–205).  `get_ctxt_info()` returns the dimensions from which PSM2
   computes each mapping length (lines 1127–1166), while `get_base_info()`
   creates the opaque tokens (lines 1244–1316).
3. A token is not a physical address.  Its format is
   `0xdabbad00:type[27:24]:ctxt[23:16]:subctxt[15:12]:page_delta[11:0]`
   (`file_ops.c` lines 95–143).  The low twelve bits are a post-map pointer
   adjustment; the driver selects the backing object from type, context, and
   subcontext only.
4. PSM2's `HFI_MMAP_ALIGNOFF` passes `token & PAGE_MASK` to `mmap64()` with
   `MAP_SHARED | MAP_LOCKED`, so the kernel sees the encoded page offset and
   reconstructs `token = vma->vm_pgoff << PAGE_SHIFT`.  PSM2 restores the
   saved low delta to the returned virtual address for the credit and event
   pages.  This exactly matches the driver validation and selector reset in
   `hfi1_file_mmap()` (lines 318–352); it does not pass `token >> PAGE_SHIFT`.
5. `hfi1_file_mmap()` verifies the magic, context, subcontext, and shared VMA
   before selecting a mapping.  Except for the eager special case, it rejects
   a page-rounded requested VMA length that differs from its computed length
   (lines 353–586).  PSM2's apparently unrounded byte expressions are rounded
   by the kernel when the VMA is created.

### Token-to-backing mapping

| Type | `hfi1_base_info` token and PSM2 request | v6.17 backing / terminal mapping |
| --- | --- | --- |
| 1 | `pio_bufbase`; `credits * 64`, `PROT_WRITE` | HFI PIO MMIO, write-combined; `io_remap_pfn_range()` |
| 2 | `pio_bufbase_sop`; `credits * 64`, `PROT_WRITE` | SOP half of HFI PIO MMIO, write-combined; `io_remap_pfn_range()` |
| 3 | `sc_credits_addr`; one page, `PROT_READ`, touch | coherent credit-return DMA page; `dma_mmap_coherent()` |
| 4 | `rcvhdr_bufbase`; `rcvhdrq_cnt * rcvhdrq_entsize`, `PROT_READ`, touch | coherent receive-header DMA queue; `dma_mmap_coherent()` |
| 5 | `rcvegr_bufbase`; `egrtids * rcvegr_size`, `PROT_READ`, touch | all non-contiguous eager DMA allocations, mapped sequentially with `dma_mmap_coherent()` |
| 6 | `user_regbase`; one page, `PROT_READ | PROT_WRITE` | per-context user-register MMIO, noncached; `io_remap_pfn_range()` |
| 7 | `events_bufbase`; one page, `PROT_READ` | `vmalloc_user()` event page; HFI installs `vma_fault()` and resolves each page with `vmalloc_to_page()` |
| 8 | `status_bufbase`; one page, `PROT_READ` | `vmalloc_user()` status page converted to a PFN with `vmalloc_to_page()`; `remap_pfn_range()` |
| 9 | `rcvhdrtail_base`; one page, `PROT_READ`, only with `DMA_RTAIL` | coherent DMA receive-tail page; `dma_mmap_coherent()` |
| 10 | `subctxt_uregbase`; one page, `PROT_READ | PROT_WRITE`, touch | shared-context `vmalloc_user()` page; HFI `vma_fault()` |
| 11 | `subctxt_rcvhdrbuf`; aligned header-queue size × subcontext count, read/write, touch | shared-context `vmalloc_user()` header area; HFI `vma_fault()` |
| 12 | `subctxt_rcvegrbuf`; aligned eager size × subcontext count, read/write, touch | shared-context `vmalloc_user()` eager area; HFI `vma_fault()` |
| 13 | `sdma_comp_bufbase`; `sdma_ring_size * sizeof(entry)`, `PROT_READ`, when SDMA is enabled | `vmalloc_user()` SDMA completion ring; HFI `vma_fault()` |

`EVENTS`, the three `SUBCTXT_*` cases, and `SDMA_COMP` are therefore the
lazy-fault/VMA-backed branch; the fault function is deliberately small:
`vmalloc_to_page(vmf->pgoff << PAGE_SHIFT)` returns `VM_FAULT_SIGBUS` on a
missing page (`file_ops.c` lines 591–607).  `STATUS` is distinct: it uses the
PFN of its `vmalloc_user()` allocation directly (lines 489–497 and 714–728).

The source establishes contract compatibility for the normal PSM2 sequence;
it does not establish which map or subsequent PSM touch caused the observed
Bad pagetable.  In particular, `MAP_LOCKED` and PSM2's explicit touches make
the DMA and VMA fault boundaries observable during endpoint open, so changing
length or protection rules before collecting that evidence would be unsafe.

### Minimal, safe kernel-side evidence candidate

- Existing HFI trace events can provide context dimensions
  (`hfi1_ctxts:hfi1_ctxt_info` and `hfi1_ctxts:hfi1_uctxtdata`).  The existing
  `hfi1_dbg:hfi1_PROC` emission from `mmap_cdbg()` records a decoded type, but
  it omits the input token and final return value and includes backing
  addresses.  It is a tracepoint, not dynamic-debug output, and is not the
  complete safe evidence record required here.
- If PSM2's user-side token trace does not identify the failure, add one
  temporary `hfi1_mmap` trace event, emitted exactly once at the common
  `hfi1_file_mmap()` exit.  Record only `token`, decoded `type`, requested VMA
  length, selected expected length, mapping-kind enum (`io`, `dma`,
  `dma-segments`, `vm-fault`, `pfn`, or `reject`), and `ret`.  Initialise the
  type to an invalid sentinel so rejected magic/context/shared cases are also
  reported.
- Do not record kernel virtual addresses, user virtual addresses, PFNs, DMA
  addresses, or page contents.  For type 5, preserve the full expected eager
  length before the per-segment loop so its one event is not misleading.  This
  is a diagnostic-only tracepoint design, not an implemented patch; it needs
  a locally rebuilt/booted T7610 kernel and must be removed after the single
  controlled repro.

### One T7610 repro to collect

Run exactly one watchdog/console-protected local `psm2_ep_open` attempt with
the PSM2 agent's environment-gated mapping log enabled.  Preserve, in order,
the raw base-info token, page-aligned offset passed to `mmap64`, page delta,
length, protection, mmap result, and explicit-touch start/end.  Correlate it
with the enabled HFI context trace events and the kernel Oops.  The first
failing mapping/touch then selects whether a PSM2 correction, the temporary
safe HFI tracepoint above, or an OEM-to-upstream HFI source comparison is the
next justified action.

## Related plan

- [Active plan](../../../../plans/active/2026/08/1-10/psm2-hfi1-mapping-fix.md)

## 2026-08-04 — PSM2 `psm2_init` err=8 localized to RDMA HCA-name discovery

- The patched-HFI probe's `psm2_init: err=8` before `psm2_ep_open`, with no
  HFI ioctl, mmap, or Oops, is explained by the normal (non-mock) PSM2 source
  before it ever opens `/dev/hfi1_0`.  The GEN1 HAL constructor sets
  `hfi_sys_class_path` to `/sys/class/infiniband/hfi1`
  (`psm_hal_gen1/psm_hal_gen1.c`, lines 73--74).  Its constructor-time
  registration calls `sysfs_init()` (`psm2_hal.c`, lines 168--171), which
  probes `/sys/class/infiniband/hfi1_0` by default
  (`opa/opa_sysfs.c`, lines 75--119).  On this host the registered HCA is
  `opap129s0`, so that probe fails and the HAL is deliberately not registered.
- `__psm2_init()` then calls `psmi_hal_initialize()` for the default IPS
  device, maps every nonzero result to `PSM2_INTERNAL_ERR` (8), and returns
  (`psm.c`, lines 484--494).  With no registered HAL,
  `psmi_hal_initialize()` has no candidate and returns
  `-PSM_HAL_ERROR_INIT_FAILED` (`psm2_hal.c`, lines 348--380).  Thus the HCA
  name mismatch is a direct cause of this observed err=8, conditional only on
  the probe being the normal non-`PSM2_MOCK_TESTING` library.  It is distinct
  from, and occurs before, the already-localized type-3 endpoint-map failure.
- Do not use `rdma dev set opap129s0 name hfi1_0` as the first validation.
  Although the command is supported, it reaches `ib_device_rename()` and then
  `device_rename()` in the running kernel.  The latter's source explicitly
  warns that renaming registered devices is racy and non-atomic for sysfs
  links/uevents.  It also notifies active RDMA clients.  A live HCA rename is
  therefore broader than necessary for a one-process PSM2 diagnosis.
- The smallest safe T7610-only test is an ephemeral, process-scoped sysfs
  alias: in a fresh mode-0700 temporary directory, create
  `hfi1_0 -> /sys/class/infiniband/opap129s0`, launch the existing *init-only*
  probe with `HFI_SYSFS_PATH` set to that alias before libpsm2 is loaded, and
  remove the temporary directory afterwards.  PSM2 accepts the environment
  path, validates the alias as a directory, strips its `_0` suffix, and then
  resolves its normal `...hfi1_0/ports/...` attribute paths through the
  symlink.  No driver, RDMA-device, network, or persistent configuration
  change is involved.  This is a test design only; it was not run.
- Preconditions for that one init-only test: confirm the alias target has the
  port and unit attributes PSM2 reads, confirm the patched driver still
  exposes `/dev/hfi1_0` (PSM2 counts units from that character-device naming
  convention), set `HFI_SYSFS_PATH` before the first library load, and do not
  call `psm2_ep_open`.  `psm2_init == PSM2_OK` would confirm the immediate
  causal chain; another err=8 would require separately tracing the remaining
  HAL-admission predicates.  Endpoint opening remains separately guarded by
  the one-repro/Oops constraints.
- For a durable source-level compatibility repair, the same name assumption
  also needs an audit after endpoint open: the OPP path-record code constructs
  `hfi1_<unit>` from the fixed HAL `hfi_name` before looking up the verbs HCA
  (`ptl_ips/ips_opp_path_rec.c`, lines 562--570).  The init-only alias proves
  and bypasses the first failure but must not be presented as an end-to-end
  PSM2 name-support fix.

## Related plan

- [Active plan](../../../../plans/active/2026/08/1-10/psm2-hfi1-mapping-fix.md)

## 2026-08-04 — type-3 credit-return mapping localized; repair candidate prepared

### Confirmed failing map and backing

- The captured token `0xdabbad00030b02d8` decodes under the v6.17 token
  layout as magic `0xdabbad00`, type `3` (`PIO_CRED`), context `0x0b`,
  subcontext `0`, and intra-page delta `0x2d8`.  PSM2 correctly passes the
  page-aligned token `0xdabbad00030b0000` as the 4 KiB `mmap64()` offset,
  then performs its first 32-bit read at the returned mapping base.  The
  `mmap` succeeds; the read is the observed Oops point.
- `get_base_info()` creates that low delta from
  `sc->hw_free - cr_base[numa].va`, modulo `PAGE_SIZE`.  Type 3 selects the
  matching page by the same difference masked with `PAGE_MASK`.
  `init_credit_return()` allocated the per-NUMA backing with
  `dma_alloc_coherent()` as `TXE_NUM_CONTEXTS` (160) `struct credit_return`
  objects; each object is eight `__le64` entries (64 B), so the allocation is
  10,240 B / three 4 KiB pages.  It is neither HFI MMIO nor a lazy-fault
  `vmalloc` mapping.
- In v6.17, the selected CPU and DMA page addresses are passed to
  `dma_mmap_coherent()` for a 4 KiB read-only map.  PSM2 requested
  `MAP_SHARED | MAP_LOCKED`; HFI1 rejects `VM_WRITE`, clears
  `VM_MAYWRITE`, and adds `VM_DONTCOPY | VM_DONTEXPAND`.  The terminal
  `remap_pfn_range()` path adds `VM_IO | VM_PFNMAP | VM_DONTDUMP` (and keeps
  `VM_DONTEXPAND`).  There is no HFI1 fault handler for type 3.

### Regression candidate and constrained patch

- The retained Cornelis OPXS 10.11.0.1.2 source clone
  (`reference/opa-hfi1-10.11.0.1-source/`,
  `bfb3da47c6f90d89015c7c7ce01c88a4e7581c7c`) uses the same token and
  coherent allocation but maps `PIO_CRED` with
  `virt_to_phys(cr_base) + page_offset`, `mapio = 1`, and the final
  `io_remap_pfn_range()` branch.
- Upstream commit
  `1ec82317a1daac78c04b0c15af89018ccf9fa2b7` (2023-01-09,
  `IB/hfi1: Use dma_mmap_coherent for matching buffers`) replaced precisely
  that type-3 code with the v6.17 `memvirt + memdma` /
  `dma_mmap_coherent()` path.  It also changed other DMA buffers, but the
  candidate intentionally restores only the type-3 hunk.  Its resulting
  `PIO_CRED` statements exactly match the pre-commit hunk.
- [`hfi1-pio-cred-legacy-pfn-map-v6.17.patch`](../../../../../patches/hfi1-pio-cred-legacy-pfn-map-v6.17.patch)
  is prepared against Linux v6.17 commit
  `e5f0a698b34ed76002dc5cff3804a61c80233a7a`.  It changes only
  `drivers/infiniband/hw/hfi1/file_ops.c` (six additions, eight removals),
  only in `case PIO_CRED`, and passed `git apply --check --verbose` against
  the retained fixed source clone.  It does not change token construction,
  ioctl ABI, PSM2 request length or protection, VMA-flag updates,
  credit-return allocation, HFI credit-return DMA programming, or any other
  DMA map type.  It was not applied to the reference clone.
- This is the strongest source-level repair candidate, not yet a root-cause
  proof: a successful `mmap` followed by a bad-page-table fault could also
  expose prior device DMA corruption.  The experiment therefore remains one
  console/watchdog-supervised repro only.  The previously designed kernel
  mapping trace patch was deliberately not added: the already captured
  userspace trace, `strace`, and HFI ftrace establish the type-3 path, and
  target minimisation takes priority.

### T7610 build, signing, activation, and rollback gates

- Use the exact source/config/ABI for the running T7610 OEM release, not the
  upstream v6.17 clone: record `uname -r`, `modinfo hfi1`, `modinfo -n hfi1`,
  the HFI1 module source package revision, `.config`, generated headers, and
  `Module.symvers`.  The patch must first pass `git apply --check` there.
  `CONFIG_INFINIBAND_HFI1=m` is required for a separately loadable `hfi1.ko`.
- An isolated build needs the full matching configured kernel source plus its
  matching `linux-headers` build tree (generated headers, scripts,
  `Module.symvers`, compiler configuration, and any BTF/pahole prerequisites).
  The in-tree HFI1 `Makefile` is module Kbuild input, so a plain upstream
  source tree or headers for another ABI are insufficient.  No build was run
  during this investigation.
- If kernel lockdown/Secure Boot requires signed modules, sign the resulting
  `hfi1.ko` with a key enrolled for that host, using the matching kernel
  `scripts/sign-file`; otherwise module insertion must be expected to fail.
  Preserve the original module path, checksum, signature state, and an
  unchanged boot entry before activation.
- Runtime replacement cannot run in parallel with the loaded `hfi1` module:
  stop consumers, retain physical console and watchdog recovery, unload only
  after dependencies are identified, load the experimental module for the
  one repro, and roll back by unloading it and restoring/loading the exact
  original module (or by rebooting into the unchanged kernel if recovery is
  uncertain).  Do not overwrite the packaged module or alter WRX80.  No T7610
  connection, build, module operation, or reboot was performed here.

## 2026-08-05 — interrupted-session recovery and current execution gate

- Chronology clarification: the statements above that no build, T7610
  connection, module operation, or alias test was performed describe the
  source-audit and candidate-design steps at the time they were written.
  Subsequent work, summarized near the start of this history, did build and
  temporarily admit the exact-OEM-ABI candidate on T7610, verify HFI
  `ACTIVE`, prove process-local alias admission through `psm2_init` and
  `psm2_finalize`, and reboot back to the stock module.  Those later results
  supersede the earlier execution-status statements without changing their
  design rationale.
- `tools/t7610-hfi1-psm2-endpoint-one-run` was added after the last history
  update.  It pins the stock and patched modules, probe source/binary, and
  PSM2 library by hash; checks consumers, Secure Boot, kernel taint and HFI
  identity/link state; creates a single-use fuse and process-local sysfs
  alias; captures `strace`, PSM trace, isolated ftrace, journal, and pstore;
  arms a 120-second watchdog; and reboots for stock-module rollback and
  postboot verification.  It also provides a non-destructive `--dry-run`.
- All seven shell tools pass `bash -n`.  The integrated runner has not yet
  been dry-run or used for the patched `psm2_ep_open` acceptance, and no
  endpoint result or no-Oops evidence exists for it yet.
- Read-only health confirmation at 2026-08-05 00:28--00:30 JST found both
  HFI links `ACTIVE` at 4xEDR and both PCIe links at Gen3 x16.  T7610 was on
  the stock `6.17.0-1030-oem` HFI1 module with `opa-fm` MASTER, no residual
  probe/benchmark/device consumer, and no new Bad pagetable, Oops, or AER
  fault in the current boot.  WRX80 was not changed and still retains the
  known earlier PSM2 Oops records, so its no-reprobe/no-reload boundary
  remains in force.
- The next action is to audit and dry-run the integrated runner, then perform
  exactly one guarded T7610 patched endpoint-open attempt if every preflight
  gate is satisfied.  Evidence must be reviewed after stock rollback before
  any additional test or coordinated WRX80 work.

## 2026-08-05 — patched endpoint-open acceptance run executed; candidate REJECTED

The previous session was force-terminated before it could record its results.
The following is reconstructed from the on-host evidence under
`/var/tmp/opa-psm2-pio-cred-endpoint-one-run/runs/` on T7610.

### What actually ran

| UTC | run | outcome |
| --- | --- | --- |
| 15:18:07 | `20260804T151807Z-nPdpoe` | interrupted before preflight completed; no module or fuse operation |
| 15:20:41 | `20260804T152041Z-dry-run-1DJiSL` | dry-run PASS (ftrace not yet exercised) |
| 15:21:09 | `20260804T152109Z-run-J5MmsF` | patched module loaded, HFI `ACTIVE`, shim gates PASS, then `preprobe_failed=hfi-ftrace-setup-failed`; **endpoint not executed, fuse not consumed**; reboot rollback, postboot PASS |
| 15:25:12 | `20260804T152512Z-dry-run-bEj1hl` | dry-run PASS including isolated ftrace instance prepare/teardown |
| 15:25:38 | `20260804T152538Z-run-tProug` | **endpoint executed**; see below |

Between 15:21 and 15:25 the runner's ftrace setup was corrected, which is why
the second attempt got past the gate that stopped the first.

### Result of the executed acceptance run

- Patched module loaded (hash `11b3693…`, srcversion `B12ECD0EAFC9F54D1510975`),
  HFI `ACTIVE` after 10 s as `opap129s0`, `opa-fm` active, shim gates PASS,
  isolated ftrace armed, fuse consumed at 15:25:59, probe started.
- `unexpected_error_status=137` at 15:26:01 — the probe was killed about two
  seconds in, far inside the 120 s watchdog, so the watchdog did not fire.
- PSM2 map trace shows the same failure point as the stock module:

      mmap request member=sc_credits_addr token=0xdabbad00030302d8
                   offset=0xdabbad0003030000 delta=0x2d8 length=4096 prot=0x1
      mmap result  member=sc_credits_addr addr=0x7a14d007e000
      touch before member=sc_credits_addr addr=0x7a14d007e000 length=4096
                   <- no "touch after"

- Kernel log:

      psm2_ep_open_pr: Corrupted page table at address 7a14d007e000
      PGD 800000013886a067 P4D 800000013886a067 PUD 13886b067 PMD 13886c067
                                                 PTE 800049168e911235
      Oops: Bad pagetable: 000d [#1] SMP PTI
      RIP: 0033:0x7a14d00cc380   CR2: 00007a14d007e000
      note: psm2_ep_open_pr[3040] exited with irqs disabled

- ftrace confirms the driver-side sequence was normal and complete:
  `hfi1_file_open` → four `hfi1_file_ioctl` → `hfi1_file_mmap` →
  `hfi1_file_close`, with no error return and no further HFI1 entry.
- **Conclusion: the minimal PIO_CRED legacy-PFN candidate does not repair the
  defect.**  `mmap()` succeeds and the first read faults identically to the
  stock `dma_mmap_coherent()` path.  Stage 3's candidate is rejected as a fix;
  it remains useful only as an A/B control.
- Rollback and recovery were clean: normal reboot to the stock module,
  `postboot=PASS stock_hash=8a64cd2a… hfi=opap129s0 opa_fm=active taint=0`.
  No pstore record was produced (the Oops did not panic the kernel).

### New evidence-backed lead: the mapped frame exceeds MAXPHYADDR

- The faulting PTE `800049168e911235` decodes as present, user, read-only,
  cache-disabled, NX, with frame `0x49168e911000` ≈ 80 TiB.
- T7610 reports `address sizes: 46 bits physical` (64 TiB) and has 188 GiB of
  RAM, so that frame is above MAXPHYADDR.  A frame above MAXPHYADDR sets
  reserved bits, which is exactly the `000d` error code
  (present + user + reserved) and the `Corrupted page table` wording.
- T7610 boots with `pcie_aspm=off pci=noaer` and **no** `intel_iommu=`
  option, and the kernel reports `iommu: Default domain type: Translated`
  with the HFI in an IOMMU group of type `DMA-FQ`.
- Working hypothesis: the credit-return page is being mapped from its
  `dma_addr_t` (an IOMMU IOVA) rather than its CPU-physical address.  Under a
  translating IOMMU the IOVA is allocated from a high address space and is not
  a valid PFN, which would produce exactly this fault — and would explain why
  both the `dma_mmap_coherent()` path and the legacy PFN path fail the same
  way, since both would be operating on the same wrong base.
- This is a hypothesis derived from the PTE and platform state, not yet from
  source.  The cheap decisive test is a single T7610 boot with
  `intel_iommu=off` (or `iommu=pt`) using the **stock** module and the same
  hashed probe: if IOVA and physical address coincide and the fault
  disappears, the defect is an IOMMU-translation assumption rather than a
  mapping-API regression.

### State left behind

- T7610 is on the stock module (`AE5B6BDF…`), taint 0, HFI `ACTIVE` at 4xEDR,
  PCIe Gen3 x16, `opa-fm` active, no residual consumer, no new Oops or AER in
  the current boot.
- `RUN-ONCE-CONSUMED` in the runner root **is consumed**; any further run of
  `tools/t7610-hfi1-psm2-endpoint-one-run` requires an explicit, recorded fuse
  reset.
- The patched module bundle and the shim roots remain under `/var/tmp`,
  unloaded and inert.  WRX80 was not touched at any point.

## 2026-08-05 — root cause found and fixed: PIO_CRED pointer arithmetic

### Measurement that settled it

`tools/t7610-hfi1-piocred-map-kprobe-one-run` was written for this: a
stock-module, single-use runner that arms 11 kprobes on the mapping path
(`iommu_dma_mmap`, `dma_common_find_pages` entry/return, `vmalloc_to_pfn`
entry/return, `remap_pfn_range`, `vm_map_pages`, ...) plus the KASLR bases,
runs the hashed probe once under a watchdog, and reboots to stock.

Three stock-module runs with identical PSM2 input (`ctxt=11`, token
`0xdabbad00030b02d8`) produced **two different outcomes**, which is what
exposed the bug:

| run | outcome |
| --- | --- |
| `20260804T160737Z` | `remap_pfn_range pfn=0x29f1000000` -> Oops, exit 137 |
| `20260804T161507Z` | `dma_common_find_pages` found pages -> `vm_map_pages` -> all mappings OK |
| `20260804T162250Z` | fault again, with the full chain captured |

The third run recorded the complete chain:

    iommu_dma_mmap        cpu_addr=0xffffd3b88ef11000 dma_addr=0xffece000 size=0x1000
                          pob=0xffff8e3c00000000 vmalloc_base=0xffffd3b880000000
                          vmemmap_base=0xfffff71f00000000
    dma_common_find_pages ret=0x0
    vmalloc_to_pfn        ret=152538447872  (0x2384000000)
    remap_pfn_range       pfn=0x2384000000

`(0 - 0xfffff71f00000000) / sizeof(struct page) == 0x2384000000` exactly, so
the frame is `page_to_pfn(NULL)` after `vmalloc_to_page()` failed.  The
earlier hypothesis that an IOVA was used as a PFN is refuted: `dma_addr` is a
normal sub-4 GiB IOVA and `iommu_dma_mmap()` never derives the PFN from it.

### The defect

`drivers/infiniband/hw/hfi1/file_ops.c`, PIO_CRED case:

```c
struct credit_return { volatile __le64 cr[8]; };            /* 64 bytes */
struct credit_return_base { struct credit_return *va; dma_addr_t dma; };

cr_page_offset = ((u64)uctxt->sc->hw_free -
		  (u64)dd->cr_base[uctxt->numa_id].va) & PAGE_MASK;   /* bytes */
memvirt = dd->cr_base[uctxt->numa_id].va + cr_page_offset;   /* pointer arithmetic */
memdma  = dd->cr_base[uctxt->numa_id].dma + cr_page_offset;  /* integer, correct */
```

`cr_page_offset` is a byte offset (0, 4096 or 8192) but `va` is a
`struct credit_return *`, so the offset is scaled by 64.  When the context's
credit-return entry is on the second or third credit-return page, `memvirt`
lands 256 KiB or 512 KiB past a 10240-byte allocation - inside the vmalloc
range but in no vm_area - and the chain above produces a frame above the
host's 46-bit MAXPHYADDR.  When the entry is on the first page the offset is
0, the scaling is a no-op, and the mapping succeeds; that is the source of
the intermittency.

`memdma` is unaffected because `dma_addr_t` is an integer type.

Upstream v6.17 carries the identical line (`file_ops.c:387`), so this is not
OEM-specific.  The Cornelis OPXS 10.11.0.1 driver added the byte offset to
`virt_to_phys(memvirt)` - integer arithmetic, correct - so the defect was
introduced when commit `1ec82317a1da` converted the case to the
`memvirt`/`memdma` form.  The earlier legacy-PFN candidate failed for a
different reason (`virt_to_phys()` on a vmap'd coherent allocation), which is
why restoring it could never have worked.

### Fix, build, and acceptance

- `patches/hfi1-pio-cred-page-offset-pointer-arith-v6.17.patch` casts to
  `(u8 *)` before adding the byte offset.  One line.  `git apply --check`
  passes against the pinned v6.17 clone, which was not modified.
- Built for the exact running OEM ABI in
  `/var/tmp/opa-hfi1-piocred-typefix-build-20260804T162821Z` from a copy of
  the earlier bundle, after reverse-applying the rejected patch and
  confirming the restored `file_ops.c` matches the pristine OEM source hash
  `d2f8498a4ae45054a2473af5568a5e3349bce5d74b4c04089f9c1dc90612abf8`.
  Module SHA-256 `90459ba5a9a0b5de121c720c33ad8780753019bb0592eba29b6c54c71c253ae9`,
  srcversion `DF441A393ED48923068892C`.  `make` exit 0, no warnings.
- The bundle's validation passed: identical vermagic, depends, name, license,
  firmware and PCI aliases; all 401 required symbol versions and CRCs equal to
  stock; every undefined symbol exported in the live `Module.symvers`;
  Secure Boot disabled.
- Acceptance run `20260804T163117Z-run-patched` (candidate module, ctxt 3):
  `dma_common_find_pages` returned a valid page array, `vm_map_pages` was
  used, `touch after member=sc_credits_addr` **succeeded**, and every
  remaining mapping completed.  No `Corrupted page table`, no Oops, no AER.
  Rollback reboot and `postboot=PASS` with taint 0 on the stock module.

## 2026-08-05 — second, independent defect: PSM2 built with AVX2

With the mapping bug fixed, `psm2_ep_open` still dies - but in userspace,
with `SIGILL`/`ILL_ILLOPN`, and with no kernel fault at all.

- Locally built instrumented library: faults at `libpsm2.so.2 + 0xf66b`,
  inside `hfp_gen1_context_open`, on `vpbroadcastq 0x20(%rbx),%ymm0`.
- Distribution `libpsm2-2 11.2.185-2build1`: same failure, at
  `libpsm2.so.2.2 + 0x50018`, on `vpbroadcastq %xmm6,%ymm0`, immediately
  after `HFI1_IOCTL_ASSIGN_CTXT` (run `20260804T163628Z-run-patched`, using
  `LD_LIBRARY_PATH` because the probe records RUNPATH, not RPATH).
- `vpbroadcastq ymm` is AVX2.  T7610's CPU is a Xeon E5-2650 v2 (Ivy Bridge);
  `/proc/cpuinfo` reports `avx` and no `avx2`.
- Cause: `opa-psm2/buildflags.mak` compiles the whole library with `-mavx2`
  unless `PSM_DISABLE_AVX2` is set, in which case it uses `-mavx`.  Both the
  Ubuntu package and the local build took the default, so neither can run on
  this CPU.
- Remedy: rebuild opa-psm2 with `PSM_DISABLE_AVX2=1`.  This is a T7610-only
  concern; WRX80's CPU has AVX2.

Both defects had to be fixed to reach a working endpoint open, and only the
first is a kernel change.

## 2026-08-06 — PSM2 working end to end; the mmap defect had three parts

WRX80 was released for reboots and module reloads, so the fix could be
carried to both hosts and measured end to end.

### The complete kernel fix

The one-line pointer-arithmetic patch stopped the Oops but left the mapping
wrong.  Two further defects surfaced only under real traffic:

2. Wrong page.  `dma_mmap_coherent()` describes a whole coherent buffer and
   selects the page with `vma->vm_pgoff`; offsetting `cpu_addr` does nothing,
   because for a vmap'd allocation `iommu_dma_mmap()` uses `cpu_addr` only to
   find the vm_area and then maps `pages[vm_pgoff]`.  hfi1 sets `vm_pgoff` to
   0 before the switch, so user space always received the *first*
   credit-return page.  No fault, but every credit read was for the wrong
   context.  Symptom: `psm2_ep_open()` succeeds, then any transfer that uses
   send PIO hangs forever.  `PSM2_SDMA=2` (send PIO disabled) completed
   normally and `PSM2_SDMA=0` (send PIO only) hung every time, which is what
   identified the credit path.
3. Wrong node.  `uctxt->sc->hw_free` lives in `dd->cr_base[sc->node]`, and
   user send contexts are allocated on `dd->node`, the HFI-local node.  The
   mmap code indexed `dd->cr_base[uctxt->numa_id]`, the node of whatever CPU
   the process happens to run on.  On T7610 the HFI is on node 1 while the
   process ran on node 0, so the offset was computed against a different
   allocation.  Fixing (2) without (3) turned the hang into
   `mmap of sc_credits_addr ... failed: No such device or address`, the
   `-ENXIO` from `iommu_dma_mmap()`'s bounds check.

[The complete patch](../../../../../../patches/hfi1-pio-cred-mmap-fix-v6.17.patch)
resolves the buffer through `uctxt->sc->node`, keeps the byte arithmetic in
integers, and passes the base and full allocation length to
`dma_mmap_coherent()` with `vm_pgoff` selecting the page.  A separate
`memdmalen` is needed because `memlen` must keep describing the VMA for the
existing `(vm_end - vm_start) != memlen` check.  The two partial patches are
retained for the record.

Built for both running ABIs from the same pristine source
(`file_ops.c` `d2f8498a…`), srcversion `134CE834F521AFC581D3594` on both:

| host | kernel | module SHA-256 |
| --- | --- | --- |
| T7610 | 6.17.0-1030-oem | `5aa5b75dfb4e2fa97054fa8d4e073c31d7c456a7bb528841e58ae454847489e3` |
| WRX80 | 6.17.0-35-generic | `1d23fdddd83d998657d257efcc583de300926b8ed194bebafa0a87221d5d6164` |

Both pass the stock-ABI checks: identical vermagic, name, license, depends,
firmware and PCI aliases, all 401 required symbol versions and CRCs equal to
the packaged module, and every undefined symbol exported in the live
`Module.symvers`.  Only `file_ops.o` is recompiled.

### Third defect: rdma-core renames the device out from under PSM2

PSM2 discovers the HFI through a fixed `/sys/class/infiniband/hfi1_<unit>`
path.  T7610 has `rdma-core` installed, whose
`60-rdma-persistent-naming.rules` runs `rdma_rename %k NAME_FALLBACK` and
renames the device to `opap129s0`; `psm2_init` then fails with err=8.  WRX80
has no such rule, which is why it was never affected and why the name looked
like it alternated across boots — the kernel name is transient there.

`/etc/udev/rules.d/60-rdma-persistent-naming.rules` on T7610 now overrides the
policy with `NAME_KERNEL`.  This retires the process-local `HFI_SYSFS_PATH`
alias entirely; it is no longer used anywhere.

### Deployment state

- `tools/hfi1-typefix-activate` loads or restores the candidate on either
  host, with `--status`, `--rehearse` (stock unload/reload, to prove the link
  recovers first), `--activate` and `--restore`.  The rehearsal passed on
  WRX80 before any candidate was loaded: link back to ACTIVE in 12 s.
- Nothing is written to `/lib/modules`, so a reboot restores stock on both.
- T7610 also carries persistent changes: the udev naming override, the
  AVX2-free `libpsm2` installed in `/usr/local/lib` (ahead of the distribution
  copy in the loader path), and `uuid-dev` for the rebuild.

### Measurements

`psm2_ep_open()` now succeeds on both hosts with no shim, and MPI over the
PSM2 MTL works cross-host.  Bandwidth with 8 MiB messages, one MPI rank per
HFI-local core group, all pairs verified cross-host from the reported
`MPI_Get_processor_name` mapping, and the transfer confirmed against the
`TxWords`/`RxWords` HFI counters (25.00 GiB of payload showed 25.65 GiB on the
wire, ~2.6% protocol overhead):

| path | direction | result |
| --- | --- | --- |
| direct Verbs, 4 QP | WRX80 -> T7610 | 50.0 Gb/s |
| direct Verbs, 4 QP | T7610 -> WRX80 | 67.4 Gb/s |
| PSM2, 1 pair | WRX80 -> T7610 | 22.6 Gb/s |
| PSM2, 4 pairs | WRX80 -> T7610 | 90.3, 29.6, 90.4 Gb/s |
| PSM2, 4 pairs | T7610 -> WRX80 | 60.2, 42.1, 41.8 Gb/s |

So PSM2 with four processes reaches about 90 Gb/s from WRX80 to T7610,
against the 50 Gb/s that direct Verbs manages in the same direction, and
clears the 90 Gb/s target that the Verbs-only work had concluded was
unreachable.  Two caveats are unresolved: run-to-run variance is large (one
WRX80 -> T7610 run in three fell to 29.6 Gb/s), and the T7610 -> WRX80
direction is slower (42-60 Gb/s), plausibly because T7610 must use the
AVX2-free PSM2 build and has the much older CPU.

A verification error is worth recording: an intermediate check appeared to
show no fabric traffic during the PSM2 runs and briefly suggested the result
was an artefact.  That check read `RxWords` on the *sending* host.  Reading
`TxWords` on the sender and `RxWords` on the receiver confirms the transfer.

## 2026-08-07 — tuning: governor, core pinning, and making the fix survive a reboot

### The fix was silently lost to a reboot

WRX80 rebooted at 2026-08-07 14:20 and came back on the packaged module, so
every PSM2 transfer hung again.  That cost a round of confused debugging.

`tools/hfi1-typefix-activate` now has `--install-persistent`, which copies the
candidate to `/lib/modules/<version>/updates/hfi1.ko` and runs `depmod`.  That
directory takes precedence over `kernel/drivers/...`, so the packaged module is
left byte-for-byte untouched and `--uninstall-persistent` restores it.  Both
hosts now keep the fix across a reboot.  `--status` reports what `modprobe`
would actually load.

### CPU governor was the dominant factor for T7610 as sender

T7610 ran `schedutil` and sat at 1200 MHz of a 3400 MHz maximum.  PSM2 is far
more CPU-bound than the Verbs path, so this mattered much more than it did for
the earlier Verbs work:

| governor | T7610 -> WRX80 (4 pairs) |
| --- | --- |
| `schedutil` | 42-60 Gb/s |
| `performance` | 78-94 Gb/s |

Both hosts are now on `performance`.  Note this is not persistent across a
reboot; nothing has been installed to restore it automatically.

### Per-core pinning removed most of the variance

`hfi_local_run` used to bind only to the HFI-local NUMA node, which let the
scheduler put two polling ranks on the two SMT threads of one physical core.
That halves the pair's throughput and showed up as a bimodal aggregate.  It is
now a small Python wrapper that hands local rank *i* the *i*-th HFI-local CPU,
first thread of each physical core first, so ranks only start sharing physical
cores once every core is used, and never share a logical CPU.

T7610's HFI-local node has just 8 physical cores (CPUs 8-15, with SMT siblings
24-31); WRX80's has 64.

### Bandwidth sweep

`tools/psm2-bw-sweep` runs the matrix and prints each repetition, so variance
is visible rather than averaged away.  8 MiB messages, window 8, ~50 GiB per
run, after both fixes above:

| pairs | WRX80 -> T7610 | T7610 -> WRX80 |
| --- | --- | --- |
| 1 | 24-29 | 5-16 |
| 2 | 55 | 22-35 |
| 4 | 43-96 | 53-71 |
| 8 | 83-93 | 76-90 |
| 12 | 87-91 | 74-77 |
| 16 | 86-90 | 71-82 |

Six consecutive runs at 12 pairs gave 90.4, 81.1, 91.0, 90.0, 90.7, 89.8 Gb/s.
Best single observation so far is 95.8 Gb/s.

Single-stream throughput is very asymmetric: WRX80 sends at 24-29 Gb/s per
process while T7610 manages only 5-16, which is why T7610 needs more processes
to reach the same aggregate.  T7610's AVX2-free PSM2 build and much older CPU
are the leading explanations; this has not been isolated.

PSM2 knobs made no useful difference at 12 pairs: `PSM2_MTU` 8192 and 10240 and
`PSM2_MQ_RNDV_HFI_WINDOW=4 MiB` all landed within the baseline's spread.

### The remaining "variance" was a short-run artefact

Chasing the spread through `PSM2_SHAREDCONTEXTS=0`, moving the hfi1/sdma
interrupts off the rank cores (`tools/hfi1-irq-place`), and an idle-gap test
produced no consistent improvement — an idle of 90 s or 300 s before a run made
no difference (91.0 and 89.9 Gb/s), so it was not a wake-up effect either.

The cause was the measurement: runs of a few seconds are dominated by start-up
and ramp.  Raising the transferred volume to ~165 GiB per run (about 15-19 s)
collapses the spread:

| configuration | short runs (~5 GiB) | long runs (~165 GiB) |
| --- | --- | --- |
| 12 pairs, WRX80 -> T7610 | 54-91 | 89.2, 89.9, 89.9 |
| 12 pairs, T7610 -> WRX80 | 60-77 | 73.7, 77.3, 75.9 |

`tools/psm2-bw-sweep` now defaults to 165 GiB per run for this reason.  The
hfi1/sdma interrupt affinity was restored to its original placement, since
moving it changed nothing once the measurement was sound.

### Final sustained results

Long runs, 8 MiB messages, window 8, ~165 GiB per run:

| pairs | WRX80 -> T7610 | T7610 -> WRX80 |
| --- | --- | --- |
| 4 | **94.5, 94.6, 94.7, 95.5** | 40.8, 41.9 |
| 8 | 89.9, 91.6 | **81.1, 84.9** |
| 12 | 89.2, 89.9, 89.9 | 73.7, 75.9, 77.3 |
| 16 | 87.1, 87.4 | 74.9, 77.5 |

The optimum differs by direction: four processes per host for WRX80 -> T7610,
eight for T7610 -> WRX80, matching the per-process asymmetry (T7610 sends far
more slowly per process, so it needs more of them).

The best configuration was verified against the HFI counters over three
consecutive 165 GiB runs: 94.72, 94.55, 94.35 Gb/s, with WRX80 `TxWords` and
T7610 `RxWords` both showing 499.7 GiB on the wire for 495 GiB of payload —
0.95% protocol overhead, and the two counters agreeing exactly.

94.5 Gb/s is about 97.5% of the 96.97 Gb/s that 100 Gb/s of signalling yields
after 64b/66b encoding, so this direction is at practical line rate.  For
comparison, direct Verbs with 4 QPs manages 50.0 Gb/s on the same path.

### The AVX2-free build is not what makes T7610 slow per process

T7610 sends at 5-16 Gb/s per process against WRX80's ~19-20, and the leading
suspicion was its AVX2-free PSM2 rebuild.  Running WRX80 against the *same*
AVX2-free library refutes that: single-pair WRX80 -> T7610 gave 19.33 and 18.85
Gb/s with the distribution AVX2 library and 20.03 and 19.06 Gb/s with the
AVX2-free one.  The per-process asymmetry is the CPU itself (Xeon E5-2650 v2
against a Threadripper PRO 3995WX), not the build flag.

### Persistence

`tools/cpu-performance-governor.service` is installed and enabled on both
hosts, because the governor mattered a great deal and did not survive a reboot.
`ExecStop` puts the governor back.  Together with the `updates/hfi1.ko`
install, a reboot now comes back in the tuned configuration.

## 2026-08-09 — TID RDMA (Accelerated RDMA) enabled for the Verbs path

### What it is and why it needed a reload

Omni-Path receives either into a shared eager buffer that the CPU then copies
from, or — with expected receive — straight into the application's registered
buffer via TID (Token ID) descriptors, with no copy.  PSM2 already used the TID
path for its rendezvous protocol, which is part of why it reaches ~95 Gb/s.
Kernel Verbs RC QPs did not: that is hfi1's `HFI1_CAP_TID_RDMA` (cap_mask bit
5), negotiated with the peer through `HFI1_CAP_OPFN` (bit 16).  Both were off.

Two constraints from the source:

- bit 5 is **not** in `HFI1_CAP_WRITABLE_MASK` (`common.h:82`), so it cannot be
  set at runtime; it must be a load-time module parameter.
- it applies to `IB_QPT_RC` only (`opfn.c:252`, `tid_rdma.c:368`), so it affects
  `ib_write_bw`/`ib_read_bw`, NFS/RDMA and similar — not PSM2.

`/etc/modprobe.d/hfi1-tid-rdma.conf` (kept as `tools/hfi1-tid-rdma.conf`) sets
`cap_mask=0x4c09a09cbba`, the driver default `0x4c09a08cb9a` plus those two
bits, on both hosts.  `tools/hfi1-typefix-activate --reload` was added to cycle
the module with `modprobe` so `modprobe.d` options take effect; because the
fixed module now lives in `updates/`, that reload keeps the PIO_CRED fix.

### Result: a large gain on RDMA Write, none on Read

`ib_write_bw`/`ib_read_bw`, 4 QPs, 8 MiB, 20 s runs, HFI-local NUMA binding:

| operation | TID RDMA off | TID RDMA on |
| --- | --- | --- |
| Write WRX80 -> T7610 | 50.54, 50.12, 50.01 | 72.31, 71.79, 71.61, 71.18, 71.17 |
| Write T7610 -> WRX80 | 58.06 | 62.65, 62.31, 61.25 |
| Read WRX80 <- T7610 | 62.82 | 61.94, 61.01, 46.49 |

Write from WRX80 gains about 43%; the reverse direction about 7%; Read does not
benefit and one Read sample was an outlier at 46.49.  The Write result was
confirmed by a full A/B — the mask was reverted, the modules reloaded, and the
number returned to 50.12/50.01 before going back to 71.61/71.17 with the bits
set again.

Correctness looks clean.  Over a 20 s run at 71.79 Gb/s the sender saw no
`RcSeqNak`, `RcResend`, `RcTimeOut`, `OtherNak` or `RcvCstrErr`, and the
receiver logged 11 `PktDrop`; `DmaWait` on the sender is ordinary flow control.
No hfi1 warning or Oops appeared on either host.  (WRX80's dmesg does carry ten
`Call Trace` entries, but they are OOM-killer dumps from an unrelated `python3`
workload on 08-08.)

### PSM2 is unaffected, as the source predicted

An A/B on the PSM2 path found no difference: T7610 -> WRX80 with 8 pairs gave
92.71/92.92/92.91 Gb/s with TID RDMA off and 92.86/92.90/92.87 with it on.

Both PSM2 numbers are nevertheless higher than the 08-07 measurements
(95.4-95.6 versus 94.5 for WRX80 -> T7610, and 92.7-92.9 versus 84.9 for
T7610 -> WRX80).  That gain is **not** from TID RDMA.  The one other thing that
changed is that WRX80's HFI moved from PCI `0000:21:00.0` to `0000:45:00.0`
when the host rebooted on 08-07; both addresses are Gen3 x16, so this is a
plausible but unverified explanation.

### Correction: RDMA Read benefits as much as Write

The first reading of these numbers — that Read gains nothing from TID RDMA —
was wrong, and the error was in the comparison, not the measurement.  Write was
measured with data flowing WRX80 -> T7610 while Read was measured with data
flowing the other way, and the two directions are not equivalent.

Tracing settled the first question: TID RDMA READ does engage.  With the
tracepoints enabled during a Read run, the requester fired
`hfi1_tid:hfi1_sge_check_align` and the responder logged 37442
`hfi1_tid:hfi1_rsp_rcv_tid_read_req` events, so the local-SGE page-alignment
condition in `setup_tid_rdma_wqe()` is satisfied and the TID path is taken.

Measured with the data direction held fixed at WRX80 -> T7610, 4 QPs:

| operation | TID RDMA off | TID RDMA on | gain |
| --- | --- | --- | --- |
| Write | 50.54, 50.12, 50.01 | 72.31, 71.79, 71.61, 71.18, 71.17 | +43% |
| Read | 48.24, 48.42 | 72.32, 72.23 | +49% |

So Read gains slightly more than Write.  What looked like a Read weakness was
the direction: with data flowing T7610 -> WRX80, Write manages 62.3 and Read
61.5, and Read measured across the two directions is 71.66 versus 61.5.

### The real asymmetry is directional, and more QPs lift it

T7610 as the data sender is the slower direction for Verbs exactly as it is for
PSM2.  It also responds to parallelism the same way — four QPs was simply too
few, a number inherited from the original Verbs baseline:

| QPs | write T7610 -> WRX80 | write WRX80 -> T7610 |
| --- | --- | --- |
| 4 | 61.70 | 71.5 |
| 8 | **76.44** | **88.70** |
| 16 | 76.00 | 79.69 |
| 32 | 75.56 | — |

At eight QPs, Read and Write agree within a few percent in both directions:

| operation | data direction | Gb/s |
| --- | --- | --- |
| Write | WRX80 -> T7610 | 88.64, 88.56 |
| Read | WRX80 -> T7610 | 87.64, 87.49 |
| Write | T7610 -> WRX80 | 76.50, 76.52 |
| Read | T7610 -> WRX80 | 73.30 |

### Current best figures

| path | data direction | bandwidth |
| --- | --- | --- |
| PSM2, 4 pairs | WRX80 -> T7610 | 95.5 Gb/s |
| PSM2, 8 pairs | T7610 -> WRX80 | 92.9 Gb/s |
| Verbs Write, 8 QP | WRX80 -> T7610 | 88.6 Gb/s |
| Verbs Read, 8 QP | WRX80 -> T7610 | 87.6 Gb/s |
| Verbs Write, 8 QP | T7610 -> WRX80 | 76.5 Gb/s |
| Verbs Read, 8 QP | T7610 -> WRX80 | 73.3 Gb/s |

Verbs is now within about 8% of PSM2 in the WRX80 -> T7610 direction, against
the 50 Gb/s it managed before this work.

## 2026-08-09 — parallelism ceiling, and why T7610 is slower as a sender

### PSM2 does not go past ~95.3 Gb/s, and should not be expected to

Long runs (~165 GiB each), WRX80 -> T7610, varying the number of process pairs:

| pairs | Gb/s |
| --- | --- |
| 4 | 95.30, 95.25 |
| 6 | 94.49, 94.14 |
| 8 | 91.79, 90.44 |
| 12 | 89.51, 89.72 |
| 16 | 88.31, 88.95 |

More parallelism makes it worse, so 4 pairs is the operating point.  That
number is essentially the wire limit.  The link carries 100 Gb/s after
encoding; OPA's link-layer framing puts 1024 payload bits in each 1056-bit LTP
(96.97%, a specification figure, not measured here), and the packet-header
overhead measured from the HFI counters is 0.95% (499.7 GiB on the wire for
495 GiB of payload).  That leaves about 96.0 Gb/s of payload capacity, so
95.3 Gb/s is roughly 99% of what the link can carry.

### Why T7610 sends more slowly than WRX80 over Verbs

T7610 as the data sender plateaus at 75-76 Gb/s for Verbs while WRX80 reaches
88-89 Gb/s.  Four things were ruled out and two measured differences remain.

**Not the hardware or the link.**  The same T7610, same HFI, same PCIe link and
same fabric sends 92.6 Gb/s over PSM2.

**Not aggregate CPU.**  Sampling `/proc/stat` across the run, Verbs costs
6.93 cores per 100 Gb/s and PSM2 costs 8.01 — Verbs is the *cheaper* path per
byte — and only 5.22 of 32 logical CPUs were busy at the plateau.

**Not the arrangement.**  The plateau is insensitive to every knob tried:

| variation | result |
| --- | --- |
| 4 / 8 / 16 / 32 QPs, one process | 61.70 / 76.44 / 76.00 / 75.56 |
| 1 / 2 / 4 processes x 4 QPs | 60.98 / 75.36 / 59.89 |
| sender pinned to a clean core (9) vs an SDMA IRQ core (12) | 75.51 / 75.81 |
| two instances on separate SLs, and four on SLs 0-3 | 46.27 / 46.54 (worse) |

**Not the SDMA engines.**  This is the decisive measurement.  `DmaWait` counts
send-side waits for SDMA resources.  Over comparable runs:

| sender | bandwidth | DmaWait |
| --- | --- | --- |
| T7610 | 75.52 Gb/s | 5,016 |
| WRX80 | 88.29 Gb/s | 103,947 |

WRX80 pushes hard enough to hit SDMA back-pressure twenty times more often and
still goes faster.  T7610 barely waits at all, so it is not being held up by the
DMA engines — it is failing to feed them.

**What is different.**  Two measurable asymmetries in the kernel Verbs path:

1. *Engine load is skewed.*  Interrupt deltas per SDMA engine during a run:

   - Verbs: 360707, 360496, 358098, 184824, 178908, 120966, 120949, 119498 —
     a 3:1.5:1 spread over the eight engines in use.
   - PSM2: 133535, 133072, 132789, 132765, 132583, 132539, 132414, 132095 —
     even to within 1%.

   The source explains it: the engine is chosen by a QP-number hash with no
   load feedback — `sdma_select_engine_sc(dd, qp->ibqp.qp_num >> dd->qos_shift,
   sc5)` (`qp.c:552`) resolving to `e->sde[selector & e->mask]` (`sdma.c:765`).
   QPs therefore collide onto engines.  PSM2 sidesteps this entirely by using
   one user context per process.

2. *Interrupts per byte.*  Verbs generated about 12.8k SDMA interrupts per GB
   against PSM2's 5.4k — 2.4 times as many.

So the limit sits in the CPU-side work of the kernel Verbs send path, which
does not parallelise across QPs or processes, and which T7610's 2.6 GHz Ivy
Bridge cores execute more slowly than WRX80's.  The engine skew and the higher
interrupt rate are measured contributors; because rearranging QPs, processes,
cores and SLs never lifted the plateau, the skew alone is not proven to be the
single binding constraint.

## Related plan

- [Active plan](../../../../plans/active/2026/08/1-10/psm2-hfi1-mapping-fix.md)
