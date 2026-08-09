# Upstream submission: hfi1 PIO_CRED credit-return mmap

The fix for the HFI1 PIO_CRED mmap was sent to the linux-rdma list on
2026-08-09 as a two-patch series.  All three messages (cover letter plus the
two patches) were accepted by the mail server and threaded correctly.

**Archived thread, including the cover letter and the patches in the exact form
they were submitted:**
<https://lore.kernel.org/linux-rdma/20260809032743.2671579-1-jyohuku.alterego@gmail.com/>

`linux-rdma-submission/` holds the two patches with the mail headers and the
sign-off removed, so the directory carries the code change and its reasoning
without carrying an author's contact details.  Use the lore link above for the
canonical submitted form.  They apply in order to `rdma/for-rc` at
`31b7c700670830a0e8a4cdcd451c88a13cc5dc48`:

```bash
git apply 0001-*.patch 0002-*.patch
```

## The series

| patch | subject | Fixes |
| --- | --- | --- |
| 1/2 | IB/hfi1: Resolve the credit-return buffer through the send context's node | `7724105686e7 ("IB/hfi1: add driver files")` |
| 2/2 | IB/hfi1: Fix the PIO_CRED credit-return mmap | `1ec82317a1da ("IB/hfi1: Use dma_mmap_coherent for matching buffers")` |

Both carry `Cc: stable@vger.kernel.org`, the documented way
(`Documentation/process/stable-kernel-rules.rst`) to have a fix picked up for
the stable trees once it is mainlined.

They are two patches because the defects have different origins: the wrong node
has been there since the driver was merged in 2015, while the byte offset
applied to a typed pointer and the misuse of `dma_mmap_coherent()` arrived with
the 2023 conversion to the DMA API.  Order matters — patch 2 without patch 1
turns the hang into an `-ENXIO` from `iommu_dma_mmap()`'s bounds check, because
the cross-node offset exceeds the buffer.

## What the bug does

On a two-socket host with a translating IOMMU, the PIO_CRED mmap returns a page
whose PTE holds a frame above MAXPHYADDR, and the first user read takes
`Corrupted page table` / `Oops: Bad pagetable`.  It is intermittent: it depends
on which of the credit-return pages the context's entry lands on, which follows
the hardware send context index and varies from boot to boot.

Correcting only the arithmetic replaces the Oops with a silent wrong-page
mapping, so every transfer that uses send PIO hangs instead — which is how the
third defect was found.

`docs/history/2026/08/1-10/psm2-hfi1-mapping-fix.md` has the full investigation,
including the kprobe traces that identified `page_to_pfn(NULL)` as the source of
the bad frame.

## State of the series when sent

- Based on `rdma/for-rc` at `31b7c700670830a0e8a4cdcd451c88a13cc5dc48`, where
  the defect is present unchanged.
- `scripts/checkpatch.pl --strict`: 0 errors, 0 checks.  The one remaining
  warning per patch is `Unknown commit id`, a false positive from the shallow
  clone used to generate them; both ids were verified against a full-history
  clone.
- Built for two kernel ABIs with no new warnings; only `file_ops.o` recompiles.
- The exact submitted code — not an equivalent earlier draft — was built and
  loaded on both hosts and passed: `psm2_ep_open()` success, PSM2 with send PIO
  forced (`PSM2_SDMA=0`, the case that hung before patch 2) at 57.5 Gb/s, PSM2
  bandwidth 95.08 Gb/s, Verbs RDMA Write 88.57 Gb/s, and no `Bad pagetable` or
  `Oops` on either host.
