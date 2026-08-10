# Benchmarks

Every number here was measured on the two hosts described below.  Raw
observations are listed individually rather than averaged, so the spread is
visible.  Where a measurement turned out to be misleading, the correction is
kept alongside it.

## Hosts

| | WRX80 | T7610 |
| --- | --- | --- |
| machine | ASRock WRX80 Creator | Dell Precision T7610 |
| CPU | Threadripper PRO 3995WX, 64C/128T, one NUMA node | 2x Xeon E5-2650 v2 (Ivy Bridge), 8C/16T each, two NUMA nodes |
| kernel | 6.17.0-35-generic (Ubuntu 24.04) | 6.17.0-1030-oem (Ubuntu 24.04) |
| HFI | Omni-Path 100, PCIe Gen3 x16, NUMA node 0 | Omni-Path 100, PCIe Gen3 x16, NUMA node 1 |
| IOMMU | — | Intel VT-d, `Translated` / DMA-FQ |

Link: 100 Gb/s over 4 lanes, `opa-fm` running on T7610.  Both hosts run the
patched `hfi1` (see `patches/`), the `performance` CPU governor, and TID RDMA
enabled (`cap_mask=0x4c09a09cbba`).

## What the link can actually carry

The nominal 100 Gb/s is the post-encoding link rate.  Above that sit two
overheads:

- OPA link-layer framing puts 1024 payload bits in each 1056-bit Link Transfer
  Packet — 96.97%.  This is a specification figure, not measured here.
- Packet headers, measured from the HFI `TxWords`/`RxWords` counters: a run
  carrying 495 GiB of payload put 499.7 GiB on the wire, so 0.95%.

That leaves roughly **96.0 Gb/s of payload capacity**.  The best sustained
result below, 95.3-95.6 Gb/s, is about 99% of it.

## PSM2 (MPI over the OpenMPI PSM2 MTL)

`tools/mpi_bw.c` driven by `tools/psm2-bw-sweep`, 8 MiB messages, window 8,
about 165 GiB per run, one rank per HFI-local physical core
(`tools/hfi_local_run`).  Pairing is derived from `MPI_Get_processor_name`, so
no pair is ever co-located on one host.

| process pairs | WRX80 -> T7610 | T7610 -> WRX80 |
| --- | --- | --- |
| 1 | 19.33, 18.85 | — |
| 4 | **95.64, 95.43, 95.30, 95.25, 95.08, 94.72, 94.55, 94.35** | 40.77, 41.88 |
| 6 | 94.49, 94.14 | — |
| 8 | 91.79, 91.58, 90.44, 89.85 | **92.93, 92.90, 92.89, 92.87, 92.86, 92.71** |
| 12 | 89.91, 89.90, 89.20 | 77.30, 75.87, 73.74 |
| 16 | 88.95, 88.31, 87.42, 87.14 | 77.47, 74.87 |

The optimum differs by direction: four pairs when WRX80 sends, eight when T7610
does.  More parallelism past the optimum makes things worse, not better.

Transfers were confirmed against the HFI counters: over three consecutive
165 GiB runs at 94.72 / 94.55 / 94.35 Gb/s, the sender's `TxWords` and the
receiver's `RxWords` both moved by 499.7 GiB for 495 GiB of payload.

## Verbs (perftest)

`ib_write_bw` / `ib_read_bw`, 8 MiB, 20-second runs, HFI-local NUMA binding.
Results are listed by the direction the **data** flows, not by which host runs
the client — Read moves data from the server to the client, so comparing Read
and Write without fixing that is misleading.

### TID RDMA off versus on, 4 QPs

| operation | data direction | TID RDMA off | TID RDMA on |
| --- | --- | --- | --- |
| Write | WRX80 -> T7610 | 50.54, 50.12, 50.01 | 72.31, 71.79, 71.61, 71.18, 71.17 |
| Read | WRX80 -> T7610 | 48.24, 48.42 | 72.32, 72.23 |
| Write | T7610 -> WRX80 | 58.06 | 62.65, 62.31, 61.25 |
| Read | T7610 -> WRX80 | 62.82 | 61.94, 61.01 |

Write gains 43% and Read 49% in the WRX80 -> T7610 direction, each confirmed by
reverting `cap_mask` and getting the original number back.  In the other
direction the gain is small, for reasons in the last section.

Correctness under TID RDMA looks clean: over a 20-second run at 71.79 Gb/s the
sender logged no `RcSeqNak`, `RcResend`, `RcTimeOut`, `OtherNak` or
`RcvCstrErr`, and the receiver 11 `PktDrop`.

### QP scaling, TID RDMA on

Four QPs — the number inherited from the original baseline — is too few:

| QPs | Write T7610 -> WRX80 | Write WRX80 -> T7610 |
| --- | --- | --- |
| 4 | 61.70 | 71.5 |
| 8 | **76.44** | **88.70** |
| 16 | 76.00 | 79.69 |
| 32 | 75.56 | — |

At eight QPs, Read and Write agree within a few percent once the data direction
is matched:

| operation | data direction | Gb/s |
| --- | --- | --- |
| Write | WRX80 -> T7610 | 88.64, 88.57, 88.56 |
| Read | WRX80 -> T7610 | 87.64, 87.49 |
| Write | T7610 -> WRX80 | 76.52, 76.50 |
| Read | T7610 -> WRX80 | 73.30 |

## What moved the numbers

| change | effect |
| --- | --- |
| Fixing the `hfi1` PIO_CRED mmap | PSM2 goes from "kernel Oops" to working at all |
| Rebuilding PSM2 with `PSM_DISABLE_AVX2=1` | required on T7610; its CPU has no AVX2 and the stock library `SIGILL`s |
| `performance` CPU governor | T7610 was pinned at 1200 of 3400 MHz. T7610 -> WRX80 at 4 pairs: 42-60 -> 78-94 Gb/s |
| One rank per physical core | removes SMT co-scheduling of two polling ranks, which halved a pair's throughput |
| Measuring over ~165 GiB instead of a few seconds | not a tuning change, but see below |
| TID RDMA (`cap_mask` bits 5 and 16) | Verbs RDMA Write and Read +43% / +49% in the fast direction |
| More QPs (4 -> 8) for Verbs | 71.5 -> 88.7 Gb/s |

### Things that made no difference

`PSM2_MTU` (8192, 10240), `PSM2_MQ_RNDV_HFI_WINDOW` (4 MiB),
`PSM2_SHAREDCONTEXTS=0`, moving the hfi1/sdma interrupts off the rank cores
(`tools/hfi1-irq-place`), and idle gaps of 90 s or 300 s before a run.

The AVX2-free PSM2 rebuild is **not** why T7610 is slower per process: WRX80
running that same library measured 20.03 and 19.06 Gb/s per process against
19.33 and 18.85 with the distribution AVX2 build.

### The "variance" was a measurement artefact

Runs of a few seconds are dominated by start-up and ramp and swing wildly.  The
same configuration, same day:

| configuration | ~5 GiB per run | ~165 GiB per run |
| --- | --- | --- |
| 12 pairs, WRX80 -> T7610 | 54-91 | 89.20, 89.90, 89.91 |
| 12 pairs, T7610 -> WRX80 | 60-77 | 73.74, 75.87, 77.30 |

`tools/psm2-bw-sweep` therefore defaults to 165 GiB per run.  Chasing the spread
through PSM2 knobs and interrupt placement beforehand found nothing, because
there was nothing there to find.

## Why T7610 is slower as a sender

T7610 sending over Verbs plateaus at 75-76 Gb/s while WRX80 reaches 88-89.
Four explanations were ruled out and two measurable differences remain.

**Not the hardware or the link.**  The same host, HFI, PCIe link and fabric
sends 92.6 Gb/s over PSM2.

**Not aggregate CPU.**  Sampling `/proc/stat` across the run, Verbs costs 6.93
cores per 100 Gb/s and PSM2 costs 8.01 — Verbs is the cheaper path per byte —
and only 5.22 of 32 logical CPUs were busy at the plateau.

**Not the arrangement.**  4/8/16/32 QPs, 1/2/4 processes, a clean core versus an
SDMA-IRQ core, and splitting across service levels all plateau at 75-76 (the SL
split is worse, 46).

**Not the SDMA engines.**  `DmaWait` counts send-side waits for SDMA resources:

| sender | bandwidth | DmaWait |
| --- | --- | --- |
| T7610 | 75.52 Gb/s | 5,016 |
| WRX80 | 88.29 Gb/s | 103,947 |

WRX80 hits SDMA back-pressure twenty times more often and still goes faster.
T7610 barely waits, so it is not held up by the DMA engines — it fails to feed
them.

**What does differ.**  Interrupt deltas per SDMA engine during one run:

- Verbs: 360707, 360496, 358098, 184824, 178908, 120966, 120949, 119498 — a
  3:1.5:1 spread across the eight engines in use.
- PSM2: 133535, 133072, 132789, 132765, 132583, 132539, 132414, 132095 — even
  to within 1%.

The engine is chosen by a QP-number hash with no load feedback:
`sdma_select_engine_sc(dd, qp->ibqp.qp_num >> dd->qos_shift, sc5)`
(`drivers/infiniband/hw/hfi1/qp.c`) resolving to `e->sde[selector & e->mask]`
(`sdma.c`), so QPs collide onto engines.  PSM2 sidesteps this by using one user
context per process.  Verbs also raises about 2.4 times as many SDMA interrupts
per byte (12.8k/GB against 5.4k/GB).

So the limit sits in the CPU-side work of the kernel Verbs send path, which does
not parallelise across QPs or processes, and which T7610's 2.6 GHz Ivy Bridge
cores execute more slowly.  The engine skew and interrupt rate are measured
contributors; since rearranging QPs, processes, cores and SLs never lifted the
plateau, the skew alone is not proven to be the single binding constraint.

## Bidirectional bandwidth

Everything above is one-way.  The link is full duplex, so it is worth asking
what both directions at once come to.  `mpi_bw` mode 2 has every rank post its
receives and its sends together; the reported aggregate counts both directions,
so on a full-duplex link it can exceed the one-way line rate.

8 MiB messages, window 8, aggregate over both directions:

| process pairs | aggregate | per direction |
| --- | --- | --- |
| 2 | 18.90 | 9.5 |
| 4 | 41.81, 41.66, 40.68 | ~21 |
| 8 | 115.27 | 57.6 |
| 16 | 129.27, 120.57, 115.84 | ~60 |
| 24 | 145.47, 131.26 | ~69 |
| 32 | **140.14, 139.94** | **~70** |

So the pair sustains about **140 Gb/s of traffic, roughly 70 Gb/s in each
direction at the same time** — more than one direction alone carries, but well
short of twice it.

The parallelism story inverts here.  Four pairs is the optimum one-way and is
the *worst* possible choice bidirectionally: at four pairs the aggregate is
41.8 Gb/s, less than half what one direction manages alone.  Each host now has
to send and receive at once, so it needs far more processes to keep both halves
fed — 24 to 32 rather than 4.  Anyone quoting a bidirectional figure from a
four-pair run would understate the link by a factor of three.

Bidirectional runs are also noticeably noisier than one-way ones: 115.8 to
129.3 Gb/s across three runs at 16 pairs, against a fraction of a percent for
one-way runs of the same length.

Verified against the HFI counters: a 16-pair run reporting 115.84 Gb/s moved
168.3 GiB out of WRX80 and 168.6 GiB into it, against 165 GiB of payload in each
direction — the expected ~2% of protocol overhead, in both directions at once.

Forcing eager instead of rendezvous costs bandwidth here as well: at 8 pairs,
86.76 Gb/s eager against 115.27 rendezvous.

### `ib_write_bw -b` does not complete

perftest's own bidirectional mode fails on this setup:

```
Failed to complete run_iter_bw function successfully
```

at 4, 8 and 16 QPs, while the same command without `-b` runs normally (88.40
Gb/s).  This was not investigated further; the PSM2 measurement above stands on
its own and is counter-verified.

## Latency

`tools/mpi_lat.c` is a two-host ping-pong reporting half round-trip time, with
per-iteration samples so the distribution is visible.  `MPI_Wtime()` costs tens
of nanoseconds, which is a few percent at the 1 us end and negligible above it.

### PSM2, default configuration

5000 iterations per size, one rank per host.

| bytes | min | median | p99 | mean |
| --- | --- | --- | --- | --- |
| 8 | **1.02** | 1.31 | 1.68 | 1.35 us |
| 64 | 1.27 | 1.41 | 1.88 | 1.45 |
| 512 | 1.37 | 1.45 | 1.96 | 1.51 |
| 4096 | 2.15 | 2.53 | 5.72 | 2.57 |
| 16384 | 4.80 | 5.49 | 9.33 | 5.83 |
| 32768 | 6.33 | 8.13 | 11.67 | 7.89 |
| 63000 | 21.50 | 22.48 | 29.63 | 23.27 |
| 65536 | 30.54 | 38.70 | 50.51 | 40.19 |
| 131072 | 42.20 | 51.15 | 63.73 | 53.91 |
| 262144 | 70.64 | 83.62 | 94.86 | 83.15 |
| 1048576 | 233.32 | 284.14 | 298.58 | 280.90 |
| 4194304 | 838.69 | 894.06 | 920.00 | 894.57 |

The step between 63000 and 65536 bytes is PSM2 switching from eager to
rendezvous at `PSM2_MQ_RNDV_HFI_THRESH`, whose default on Xeon is 64000
(`MQ_HFI_THRESH_RNDV_XEON` in `psm_config.h`).

## Eager versus TID (expected receive)

Rendezvous uses the HFI's expected-receive mechanism: the receiver registers the
destination buffer and hands back TIDs, and the payload is then DMA'd straight
into it.  Eager instead lands the data in a shared receive buffer that the CPU
copies out.  Moving the threshold lets both paths be measured at the *same*
message size, which is the only way to separate the protocol from the size.

### PSM2, same size, path forced with `PSM2_MQ_RNDV_HFI_THRESH`

Median half round-trip, 3000 iterations:

| bytes | eager | rendezvous / TID | TID cost |
| --- | --- | --- | --- |
| 4096 | 2.54 | 2.55 | none observed |
| 16384 | 5.53 | 18.90 | +13.4 us |
| 32768 | 8.09 | 25.34 | +17.3 us |
| 65536 | 23.01 | 37.75 | +14.7 us |
| 131072 | 33.80 | 48.88 | +15.1 us |
| 262144 | 57.50 | 78.02 | +20.5 us |
| 1048576 | 205.81 | 236.36 | +30.6 us |

**Eager is faster at every size measured**, by a roughly constant 13-20 us —
the rendezvous handshake — which simply matters proportionally less as the
message grows.  At 4 KiB no difference appeared even with the threshold forced
down, which is not explained here.

So why does PSM2 switch to rendezvous at 64 KB?  Because latency is not what it
is optimising.  The same 8 MiB, 4-pair bandwidth run:

| path | bandwidth |
| --- | --- |
| rendezvous / TID (default) | 95.03, 94.87 Gb/s |
| forced eager | 69.73, 69.51 Gb/s |

TID buys **36% more bandwidth for 15-30 us of latency**.  For a latency-bound
exchange of medium messages, raising `PSM2_MQ_RNDV_HFI_THRESH` is a real
optimisation; for streaming, it costs a third of the link.

### Verbs, TID RDMA off versus on

`ib_write_lat`, 20000 iterations, `t_typical`:

| bytes | TID RDMA off | TID RDMA on | change |
| --- | --- | --- | --- |
| 65536 | 25.76 | 25.93 | none — TID RDMA cannot engage |
| 262144 | 99.13 | 168.79 | +70% |
| 1048576 | 480.56 | 609.84 | +27% |

The 64 KiB row is a check on the mechanism rather than a result:
`TID_RDMA_MIN_SEGMENT_SIZE` is 256 KiB (`tid_rdma.h`), so TID RDMA is not
available below that, and the measurement agrees.

Small-message Verbs write latency, for reference (TID RDMA cannot engage at
these sizes):

| bytes | t_min | t_typical |
| --- | --- | --- |
| 8 | 4.29 | 5.96 us |
| 512 | 6.02 | 7.21 |
| 4096 | 7.72 | 8.17 |

PSM2 is four to six times faster than kernel Verbs for small messages — 1.31 us
against 5.96 us at 8 bytes — which is the usual reason MPI runs on PSM2 rather
than on Verbs.

### Summary

TID / expected receive is a bandwidth optimisation that costs latency, in both
stacks and by similar proportions:

| | latency | bandwidth |
| --- | --- | --- |
| PSM2 rendezvous vs eager | +13 to +31 us | +36% |
| Verbs TID RDMA vs plain RC | +27% to +70% | +43% to +49% |

Neither default is wrong; they are tuned for streaming.  A latency-sensitive
workload with medium messages should consider raising
`PSM2_MQ_RNDV_HFI_THRESH`, and one that never sends more than 256 KiB gains
nothing from TID RDMA either way.

## Reproducing

```bash
# PSM2 bandwidth
tools/psm2-bw-sweep                       # defaults: 165 GiB/run, 3 reps
PAIRS="4 8" SIZES_MIB=8 REPS=3 tools/psm2-bw-sweep

# Verbs, 8 QPs, data flowing towards the server
ib_write_bw -d hfi1_0 -i 1 -s 8388608 -q 8 -F --report_gbits -D 20          # server
ib_write_bw -d hfi1_0 -i 1 -s 8388608 -q 8 -F --report_gbits -D 20 <server> # client
```

```bash
# Bidirectional (mode 2); needs far more pairs than a one-way run
mpirun --hostfile hf -np 64 --map-by ppr:32:node --mca pml cm --mca mtl psm2 \
       --bind-to none hfi_local_run mpi_bw 8388608 8 82 2

# Latency, and the eager/TID comparison
mpirun --hostfile hf -np 2 --map-by ppr:1:node --mca pml cm --mca mtl psm2 \
       --bind-to none hfi_local_run mpi_lat 5000
mpirun ... -x PSM2_MQ_RNDV_HFI_THRESH=8388608 hfi_local_run mpi_lat 3000 65536   # eager
mpirun ... -x PSM2_MQ_RNDV_HFI_THRESH=8       hfi_local_run mpi_lat 3000 65536   # TID
```

`tools/psm2-bw-sweep` has the host addresses at the top; adjust `PEER_IP` and
`SELF_IP` for your pair of machines.
