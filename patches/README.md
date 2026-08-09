# Local source patches

Patches in this directory are kept separate from the read-only source clones
under `reference/`.  They are not applied automatically.  Each patch names
the exact upstream commit it was prepared against and must be checked with
`git apply --check` in a disposable worktree before a supervised hardware
experiment.

`psm2-hfi1-map-trace-be99e354.patch` is an environment-gated diagnostic patch
for the PSM2 endpoint-open mapping path.  It changes no mapping token,
protection, length, retry, or error-handling decision.

`hfi1-pio-cred-legacy-pfn-map-v6.17.patch` is a T7610-only experimental
kernel repair candidate for Linux `v6.17`
(`e5f0a698b34ed76002dc5cff3804a61c80233a7a`).  It changes only HFI1 mmap
type 3 (`PIO_CRED`): it restores the Cornelis OPXS 10.11.0.1 direct-PFN
mapping path in place of the `dma_mmap_coherent()` path introduced by upstream
commit `1ec82317a1da`.  It does not modify map tokens, request length,
permissions, VMA flag updates, credit-return allocation, or HFI DMA address
programming.  It passed `git apply --check` against the retained v6.17 clone;
it was not applied to that clone.

`hfi1-pio-cred-mmap-fix-v6.17.patch` is the authoritative fix for the HFI1
PIO_CRED credit-return mmap.  It corrects the node used to resolve the
credit-return buffer, the byte-offset-on-a-typed-pointer arithmetic, and the
use of `dma_mmap_coherent()` (base plus full length, page selected by
`vma->vm_pgoff`).  It passed `git apply --check` against the retained v6.17
clone without modifying it, and was built and validated against the stock ABI
on both hosts.

`hfi1-pio-cred-page-offset-pointer-arith-v6.17.patch` and
`hfi1-pio-cred-map-correct-page-v6.17.patch` are the two intermediate,
incomplete versions.  They are retained only to document the investigation
and must not be applied.
