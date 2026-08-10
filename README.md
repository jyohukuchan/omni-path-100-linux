# Omni-Path 100 on current Linux

Getting Intel Omni-Path 100 (`hfi1`) to run at line rate on Ubuntu 24.04 with a
6.17 kernel — a kernel bug fix that has been sent upstream, two build and
packaging fixes, tuning, and reproducible benchmarks.

Measured between a Threadripper PRO 3995WX and a 2013 Dell Precision T7610:

| path | data direction | sustained |
| --- | --- | --- |
| **PSM2 (MPI), 4 process pairs** | WRX80 -> T7610 | **95.5 Gb/s** |
| PSM2 (MPI), 8 process pairs | T7610 -> WRX80 | 92.9 Gb/s |
| Verbs RDMA Write, 8 QPs | WRX80 -> T7610 | 88.6 Gb/s |
| Verbs RDMA Read, 8 QPs | WRX80 -> T7610 | 87.6 Gb/s |

95.5 Gb/s is about 99% of what the link can carry once OPA's link-layer framing
and packet headers are accounted for.  Before this work, PSM2 crashed the kernel
and Verbs managed 50 Gb/s on the same path.

Both directions at once come to about **140 Gb/s aggregate, ~70 Gb/s each way**
— but only with 24-32 process pairs.  Four pairs, the one-way optimum, is the
worst bidirectional choice and reports 41.8 Gb/s.

Latency, half round-trip, same pair of hosts:

| | 8 B | 64 KiB | 1 MiB |
| --- | --- | --- | --- |
| PSM2 (MPI) | **1.31 us** | 23.0 us eager / 37.8 us TID | 206 us eager / 236 us TID |
| Verbs RDMA Write | 5.96 us | 25.9 us | 610 us TID / 481 us plain |

TID / expected receive turns out to be a **bandwidth optimisation that costs
latency**: measured at identical message sizes it adds 13-31 us in PSM2 and
27-70% in Verbs, and buys 36-49% more bandwidth.

Full numbers, including everything that turned out **not** to matter:
[docs/benchmarks.md](docs/benchmarks.md).

## The problems, and what they were

### 1. A kernel bug that Oopses the machine (fixed, sent upstream)

`psm2_ep_open()` intermittently killed the kernel with
`Corrupted page table` / `Oops: Bad pagetable`.  The PIO_CRED case of
`hfi1_file_mmap()` is wrong three ways at once:

- it resolves the credit-return buffer through `uctxt->numa_id`, the node the
  *process* happens to run on, instead of the send context's own node;
- it adds a byte offset to a `struct credit_return *`, so the offset is scaled
  by 64 and the address lands outside the allocation — which, with an IOMMU
  translating, produces a page frame above MAXPHYADDR and the Oops;
- it offsets `cpu_addr` instead of setting `vma->vm_pgoff`, which is how
  `dma_mmap_coherent()` actually selects a page — so even with the arithmetic
  corrected, user space gets the *wrong* credit-return page and every transfer
  that uses send PIO hangs silently.

It is intermittent because it depends on which credit-return page the context's
entry lands on, which follows the hardware send context index and varies from
boot to boot.

Present unchanged in mainline.  Fix sent as a two-patch series:
[archived thread on lore.kernel.org](https://lore.kernel.org/linux-rdma/20260809032743.2671579-1-jyohuku.alterego@gmail.com/),
local copies and rationale in
[patches/linux-rdma-submission.md](patches/linux-rdma-submission.md).

### 2. PSM2 is built with AVX2 (T7610 only)

`opa-psm2` compiles the whole library with `-mavx2` by default.  The T7610's
Xeon E5-2650 v2 has AVX but not AVX2, so both the Ubuntu `libpsm2-2` package and
any default local build `SIGILL` on `vpbroadcastq` during context open.  Rebuild
with `PSM_DISABLE_AVX2=1`.

### 3. rdma-core renames the device out from under PSM2

PSM2 discovers the HFI through a fixed `/sys/class/infiniband/hfi1_<unit>` path.
`rdma-core`'s `60-rdma-persistent-naming.rules` runs
`rdma_rename %k NAME_FALLBACK` and renames it to something like `opap129s0`, so
`psm2_init()` fails with err=8.  Override the policy with `NAME_KERNEL`:

```
# /etc/udev/rules.d/60-rdma-persistent-naming.rules
ACTION=="add", SUBSYSTEM=="infiniband", PROGRAM="rdma_rename %k NAME_KERNEL"
```

## Tuning that mattered

| change | effect |
| --- | --- |
| `performance` CPU governor | the T7610 sat at 1200 of 3400 MHz; its send throughput roughly doubled |
| One MPI rank per HFI-local **physical** core | stops two polling ranks sharing one core's SMT threads |
| TID RDMA (`cap_mask` bits 5 and 16) | Verbs RDMA Write +43%, Read +49% |
| 8 QPs rather than 4 for Verbs | 71.5 -> 88.7 Gb/s |
| Measuring over ~165 GiB per run | short runs swing by tens of Gb/s and send you chasing ghosts |

PSM2 knobs (`PSM2_MTU`, `PSM2_MQ_RNDV_HFI_WINDOW`, `PSM2_SHAREDCONTEXTS`) and
hfi1 interrupt placement made no measurable difference.

## Layout

| path | contents |
| --- | --- |
| `patches/linux-rdma-submission/` | the upstream fix, two patches, applied in order |
| `patches/` | earlier incomplete attempts, kept only as a record of the investigation — do not apply |
| `tools/` | activation, pinning and benchmark tooling (see below) |
| `docs/benchmarks.md` | all measurements, with the corrections |
| `docs/plans/main-plan.md` | current state, decisions, open questions |
| `docs/history/` | the full investigation log |
| `reference/` | not in git; the read-only upstream clones used during the work, inventoried with pinned commits and licences in [docs/reference-sources.md](docs/reference-sources.md) |

### Tools

| tool | purpose |
| --- | --- |
| `hfi1-typefix-activate` | load, restore, rehearse or persistently install the patched `hfi1`, with hash and ABI checks |
| `hfi_local_run` | pin an MPI rank to its own HFI-local physical core |
| `mpi_bw.c` | two-host streaming bandwidth benchmark; pairs are derived from processor names so they are never co-located |
| `mpi_lat.c` | two-host ping-pong latency, reporting min/median/p99 per size |
| `psm2-bw-sweep` | run the benchmark matrix and print every repetition |
| `hfi1-irq-place` | move hfi1/sdma interrupts to a chosen CPU list, with save and restore |
| `cpu-performance-governor.service` | keep the `performance` governor across reboots |
| `t7610-*` | the guarded single-shot probes used to find the kernel bug: hash-pinned, watchdogged, evidence-capturing, reboot-rollback |

The `t7610-*` runners are specific to the machine they were written for and are
published as a record of method rather than as general-purpose tooling.

## Caveats

- The patched module is **out of tree**.  It is installed as
  `/lib/modules/<version>/updates/hfi1.ko`, which takes precedence without
  touching the packaged module, but it is built for one exact kernel version: a
  kernel upgrade silently reverts you to the broken driver until you rebuild.
  Until the fix lands upstream, either rebuild on each upgrade or use DKMS.
- Everything was measured on exactly two machines.  The kernel bug is
  hardware-independent; the performance numbers are not.
- The remaining asymmetry — T7610 is slower as a sender — is characterised in
  [docs/benchmarks.md](docs/benchmarks.md) but not eliminated.

## Licence

GPL-2.0-only.  See [LICENSE](LICENSE).  The patches under `patches/` are
derivatives of the Linux kernel and are GPL-2.0-only for that reason; the rest
is licensed to match.
