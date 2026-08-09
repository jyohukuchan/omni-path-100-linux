# PSM2 userspace mapping audit

This note covers the PSM2 userspace side of the reproducible
`psm2_ep_open` bad-pagetable failure.  It is source analysis only: no device
was opened, built, or exercised while producing it.

## Source and ABI boundary

- Source: `cornelisnetworks/opa-psm2` commit
  `be99e35465b0de6a37d9c30408256f1dc5b12ea6`, retained as the depth-one
  clone `reference/opa-psm2-source/`.
- License: dual BSD 3-Clause or GPL-2.0-only (`COPYING`).
- The current Linux UAPI supplies HFI1 user ABI major 6/minor 3.  When
  `IB_IOCTL_MAGIC` is available, PSM2 selects the ioctl command interface
  and translates its compatibility `hfi1_user_info_dep` into current
  `struct hfi1_user_info` before `HFI1_IOCTL_ASSIGN_CTXT`.  The compatibility
  structure contains the obsolete `hfi1_alg` field; the major-6 structure
  instead has a 32-bit pad at that location.  PSM2 deliberately does not
  pass `hfi1_alg` to a major-6 driver.

The relevant command payloads are therefore:

| Driver operation | PSM2 source | Payload type / requested length |
| --- | --- | --- |
| Assign context | `psm_hal_gen1/opa_proto_gen1.c` | `struct hfi1_user_info` and `sizeof(struct hfi1_user_info)` on the ioctl ABI; `hfi1_user_info_dep` only on legacy write ABI |
| Context info | same | `struct hfi1_ctxt_info`, `sizeof(*cinfo)` |
| User/base info | same | `struct hfi1_base_info`, `sizeof(*binfo)` |

The diagnostic patch records the runtime `sizeof` values and the selected
transport so an ABI/type/length mismatch is observable rather than inferred.

## Endpoint-open to mapping call graph

```text
psm2_ep_open
  -> __psm2_ep_open_internal
    -> psmi_ep_open_device                         (psm_ep.c)
      -> psmi_context_open                         (psm_context.c)
        -> psmi_hal_context_open
          -> hfp_gen1_context_open                 (psm_hal_inline_i.h)
            -> hfi_context_open_ex                 (opens /dev/hfi1_0)
            -> hfi_userinit_internal               (opa_proto_gen1.c)
              -> ASSIGN_CTXT
              -> CTXT_INFO
              -> USER_INFO (returns base-info map tokens)
              -> map_hfi_mem
                -> mmap64(token rounded down to page) / selected first-touch
```

`hfp_gen1_context_open()` retries a `NULL` `hfi_userinit_internal()` result,
but `hfi_userinit_internal()` aborts after a post-assignment context/base-info
failure.  A mapping failure returns `NULL` from that function through the
normal error path.  The bad-pagetable failure occurs below the userspace
faulting instruction, so the last successful trace record is essential.

## Base-info token mapping requests

`HFI_MMAP_ALIGNOFF()` is defined in
`psm_hal_gen1/opa_user_gen1.h`.  For every base-info field, it calls:

```c
hfi_mmap64(NULL, length, protections, MAP_SHARED | MAP_LOCKED, fd,
           (__off64_t)(token & ~(page_size - 1)))
```

It passes the page-aligned raw 64-bit token as the file offset.  It does
**not** pass `token >> PAGE_SHIFT`; the kernel derives `vm_pgoff` from the
file offset in the normal `mmap` path.  PSM2 replaces each base-info token
with the returned userspace mapping address after success.  `events_bufbase`
is the exception: PSM2 restores the original intra-page token offset into
the resulting userspace address after mapping.

| Order | `hfi1_base_info` token | Requested length | Protections | First touched by PSM2 |
| ---: | --- | --- | --- | --- |
| 1 | `sc_credits_addr` | one page | read | yes |
| 2 | `pio_bufbase_sop` | `credits * 64` | write | no |
| 3 | `pio_bufbase` | `credits * 64` | write | no |
| 4 | `rcvhdr_bufbase` | `rcvhdrq_cnt * rcvhdrq_entsize` (`size_t`) | read | yes |
| 5 | `rcvegr_bufbase` | `egrtids * rcvegr_size` (`size_t`) | read | yes |
| 6 | `sdma_comp_bufbase` | `sdma_ring_size * sizeof(struct hfi1_sdma_comp_entry)` | read | no; only when `HFI1_CAP_SDMA` |
| 7 | `user_regbase` | one page | read/write | no |
| 8 | `rcvhdrtail_base` | one page | read | no; only when `HFI1_CAP_DMA_RTAIL` |
| 9 | `events_bufbase` | one page | read | no |
| 10 | `status_bufbase` | one page | read | no |
| 11a | `subctxt_uregbase` | one page | read/write | yes; only for a shared context |
| 11b | `subctxt_rcvhdrbuf` | `ALIGN(rcvhdrq_cnt * rcvhdrq_entsize, page) * subctxt_cnt` | read/write | yes; shared only |
| 11c | `subctxt_rcvegrbuf` | `ALIGN(egrtids * rcvegr_size, page) * subctxt_cnt` | read/write | yes; shared only |

The explicit `size_t` casts on receive-buffer products are present in the
upstream source.  The only explicit multiplication-overflow check here is
for the final shared eager-buffer request; the trace preserves the actual
length passed to every request.

## Diagnostic patch and one-shot T7610 validation

[`../patches/psm2-hfi1-map-trace-be99e354.patch`](../patches/psm2-hfi1-map-trace-be99e354.patch)
adds no guard or workaround.  With `PSM2_HFI1_MAP_TRACE=1`, it emits:

- chosen command interface, command/type/length and context-assignment data;
- returned context and every base-info token before PSM2 overwrites it;
- mmap request token, page-aligned offset, intra-page delta, requested length,
  protections, and the successful returned mapping address;
- a `touch before`/`touch after` bracket for each mapping PSM2 dereferences
  during initialization.

With the variable absent, empty, or `0`, the trace function returns before
performing output and the existing open path is unchanged.

The next T7610 probe must be a single, supervised run only after physical
console recovery and a watchdog are available.  Apply the patch to a separate
build checkout at the pinned commit, retain the build and kernel identities,
then run the existing minimal local endpoint-open reproducer with
`PSM2_HFI1_MAP_TRACE=1` and capture stderr plus the kernel journal.  Do not
repeat the probe after an Oops.  Correlate the final userspace map/touch record
with the HFI1-side mapping trace; only then select a minimal driver or
userspace repair.
