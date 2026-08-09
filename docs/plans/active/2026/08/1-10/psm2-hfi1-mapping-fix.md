# PSM2/HFI1 mapping-fix plan

## Objective

Make `psm2_ep_open` safe and functional on T7610 by identifying and fixing the PSM2/HFI1 userspace-mapping defect. The first acceptance check is a successful local endpoint open without a kernel Oops; end-to-end PSM2 requires a matching repair on WRX80 later.

## Decisions and boundaries

- Treat PSM2 and Linux `hfi1` as a single source-level debugging target. PSM2 is open source; no binary reverse engineering is required.
- T7610 may be rebooted and patched. Keep WRX80 free of reboot, HFI/PCIe/network reload, and persistent driver changes until a T7610 repair is proven.
- Each known-crashing probe must run once with a watchdog and physical-console recovery available. Preserve evidence before another probe.
- Do not accept a PSM2-only guard that avoids the Oops but leaves a nonfunctional endpoint as a fix.

## Stages

1. Pin PSM2 and Linux HFI1 source revisions; retain the minimal reproducer and kernel evidence.
2. Correlate PSM2 ioctl/mmap requests with the HFI1 token-to-VMA mapping path.
3. Implement and test the smallest evidence-backed repair on T7610.
4. Document the result and the coordinated WRX80 requirements for end-to-end PSM2.

## Status

- 2026-08-04: Active. The defect reproduces on Ubuntu 6.8 and 6.17 with both distribution and current upstream PSM2. `psm2_init` succeeds; `psm2_ep_open` triggers `Corrupted page table` / `Bad pagetable`.
- 2026-08-04: Stages 1 and 2 are complete. The first fault is the initial read of the type-3 `PIO_CRED` / `sc_credits_addr` mapping. A minimal patch restores only that mapping to the legacy PFN path.
- 2026-08-04: The candidate was built for the exact running OEM ABI and temporarily admitted on T7610. HFI returned `ACTIVE`; a process-local `HFI_SYSFS_PATH` alias allowed patched PSM2 initialization and finalization. T7610 was then rebooted normally and verified on the stock module.
- 2026-08-05: Stage 3 is complete and its candidate is **rejected**. The hash-pinned single-use runner dry-ran PASS and then executed exactly one guarded patched `psm2_ep_open` attempt at 15:25:38Z. `mmap()` of `sc_credits_addr` succeeded and the first read faulted identically to the stock module: `Corrupted page table` / `Oops: Bad pagetable: 000d`, probe killed with status 137. Reboot rollback and postboot stock verification passed; T7610 is clean and WRX80 untouched.
- 2026-08-05: New lead from that evidence. The faulting PTE `800049168e911235` carries frame `0x49168e911000` (~80 TiB), above T7610's 46-bit MAXPHYADDR, which is what makes the entry "corrupted". T7610 runs with the Intel IOMMU in `Translated`/`DMA-FQ` mode and no `intel_iommu=` boot option. Hypothesis: the credit-return page is mapped from a `dma_addr_t` IOVA instead of a CPU-physical address, which would explain why both the upstream `dma_mmap_coherent()` path and the legacy PFN path fail identically.
- 2026-08-05: **Root cause found and fixed.** The IOVA hypothesis is refuted. kprobes on the mapping path measured `dma_common_find_pages` returning NULL and `vmalloc_to_pfn` returning `page_to_pfn(NULL)`, which matches `(0 - vmemmap_base)/sizeof(struct page)` exactly. The defect is in `hfi1_file_mmap()`'s PIO_CRED case: a byte offset (`cr_page_offset`) is added to `dd->cr_base[].va`, a `struct credit_return *`, so it is scaled by 64 and `memvirt` lands outside the allocation whenever the entry is not on the first credit-return page. That is also why the fault was intermittent. Upstream v6.17 has the same line; the OPXS 10.11 original was correct.
- 2026-08-05: [The one-line fix](../../../../../../patches/hfi1-pio-cred-page-offset-pointer-arith-v6.17.patch) was built for the exact OEM ABI, passed full stock-ABI validation (401 symbol versions and CRCs), and its acceptance run mapped and touched `sc_credits_addr` successfully with no kernel fault. Reboot rollback and postboot verification passed.
- 2026-08-05: **Second, independent defect.** With the mapping fixed, `psm2_ep_open` dies in userspace with `SIGILL` on `vpbroadcastq ymm`, an AVX2 instruction, in the context-open path. T7610's Xeon E5-2650 v2 (Ivy Bridge) has AVX but not AVX2, and both the Ubuntu `libpsm2-2` package and the local build were compiled with `opa-psm2`'s default `-mavx2`. WRX80's CPU is unaffected.
- 2026-08-06: **Objective met.** WRX80 was released for reboots and reloads, so the fix was carried to both hosts. `psm2_ep_open()` succeeds on both, and MPI over the PSM2 MTL transfers across the fabric.
- 2026-08-06: The mmap defect had three parts, not one. Beyond the pointer-arithmetic bug, `dma_mmap_coherent()` selects the page with `vma->vm_pgoff` rather than by offsetting `cpu_addr`, so user space received the wrong credit-return page and every send-PIO transfer hung; and the buffer was indexed by `uctxt->numa_id` instead of the send context's own node, which produced `-ENXIO` once the page selection was corrected. [The complete patch](../../../../../../patches/hfi1-pio-cred-mmap-fix-v6.17.patch) fixes all three; the two partial patches are kept for the record.
- 2026-08-06: Two non-kernel defects were also required: `opa-psm2` built with `-mavx2` (rebuilt with `PSM_DISABLE_AVX2=1` for T7610's Ivy Bridge CPU) and `rdma-core`'s `NAME_FALLBACK` rename breaking PSM2's `hfi1_<unit>` discovery on T7610 (udev override to `NAME_KERNEL`). The process-local sysfs alias workaround is retired.
- 2026-08-06: Bandwidth, counter-verified: PSM2 with four cross-host pairs reaches about 90 Gb/s WRX80->T7610 versus 50.0 Gb/s for direct Verbs on the same path; T7610->WRX80 is 42-60 Gb/s versus 67.4 Gb/s for Verbs.
- 2026-08-07: Tuning complete for now. The `performance` governor (T7610 was capped at 1200 of 3400 MHz), per-physical-core rank pinning, and measuring over ~165 GiB per run instead of a few seconds together give **94.5 Gb/s sustained WRX80->T7610** (4 processes per host, counter-verified) and **84.9 Gb/s T7610->WRX80** (8 processes). The apparent variance was a short-run measurement artefact; `PSM2_SHAREDCONTEXTS`, `PSM2_MTU`, `PSM2_MQ_RNDV_HFI_WINDOW`, hfi1 interrupt placement and idle gaps all made no difference once runs were long enough.
- 2026-08-07: The fix is now installed as `/lib/modules/<version>/updates/hfi1.ko` on both hosts, so a reboot no longer silently reverts it. A WRX80 reboot had already caused exactly that.
- 2026-08-09: **TID RDMA enabled and measured.** `cap_mask=0x4c09a09cbba` (default plus `HFI1_CAP_TID_RDMA` bit 5 and `HFI1_CAP_OPFN` bit 16) is set through `/etc/modprobe.d/hfi1-tid-rdma.conf` on both hosts. Bit 5 is not runtime-writable, so a reload is required; `--reload` was added to the activation tool. Verbs RDMA Write WRX80->T7610 rose from 50.0 to 71.5 Gb/s (+43%), confirmed by reverting the mask and getting 50.1 back; the reverse direction gained 7%; RDMA Read did not benefit. No NAKs, resends or timeouts. PSM2 is unaffected (A/B: 92.7-92.9 Gb/s either way), which matches the source gating TID RDMA on `IB_QPT_RC`.
- 2026-08-09: PSM2 itself now measures 95.5 Gb/s WRX80->T7610 and 92.9 Gb/s T7610->WRX80, up from 94.5 and 84.9 on 08-07. The T7610->WRX80 gain is not from TID RDMA; the only other change is that WRX80's HFI moved from PCI `0000:21:00.0` to `0000:45:00.0` across the 08-07 reboot (both Gen3 x16). Not isolated.
- 2026-08-09: **Upstream submission prepared** in `patches/linux-rdma-submission/`. Split into two patches because the defects have different origins: the node selection has been wrong since `7724105686e7 ("IB/hfi1: add driver files")` (2015), while the byte-offset-on-a-typed-pointer and the `dma_mmap_coherent()` misuse came in with `1ec82317a1da`. Based on `rdma/for-rc` at `31b7c700`, where the defect is present unchanged; `checkpatch --strict` is clean apart from a shallow-clone false positive; recipients come from `get_maintainer.pl`. The exact submitted code was built and exercised on both hosts (srcversion `764EE580A197C141E2DCCFC`) and passes endpoint open, PIO-forced transfer, 95.08 Gb/s PSM2 and 88.57 Gb/s Verbs with no faults. The only thing left is the author's real name in `From:`/`Signed-off-by:`, which cannot be supplied on their behalf.
- 2026-08-09: **Series sent to linux-rdma.** All three messages accepted (SMTP 250) and correctly threaded; [archived thread](https://lore.kernel.org/linux-rdma/20260809032743.2671579-1-jyohuku.alterego@gmail.com/). Awaiting review.
- Remaining work: responding to review; why T7610 sends more slowly than WRX80 over Verbs (characterised, not eliminated); DKMS if the fix must survive kernel upgrades before it lands upstream.

## Related history

- [Initial work log](../../../../../history/2026/08/1-10/psm2-hfi1-mapping-fix.md)
