# PIM Address Translation: Design and Implementation

This document describes the first PIM-side virtual-memory model in PIMID. It
records the design choices, the request path, the configuration and statistics,
and the limits that matter when interpreting results. The model is intended as a
starting point for MMU design studies, not as a complete operating-system or
processor MMU implementation.

## Scope

When enabled, translation applies to data accesses reported for device-side
`alu_core` PEs in `method: exec` runs with `scope: device`. QEMU supplies the
guest virtual address. PIMID translates that address to a synthetic physical
address before issuing the data access through the normal ZSim load/store path.
Translation is disabled by default.

The design borrows familiar mechanisms from hardware page walkers and Linux-like
radix page tables: per-PE TLBs, levels of page-table entries, page-frame numbers,
and accessed/dirty state. The structures and policy below are PIMID's own small
model; they are not a literal Linux page-table format or x86 page-walk
implementation.

## Address and page-table layout

The initial configuration is fixed to 4 KiB pages and a 48-bit canonical virtual
address. The low 36 VPN bits are split into three 12-bit indices:

| Field | Width | Meaning |
|---|---:|---|
| Page offset | 12 bits | Byte offset within a 4 KiB page |
| Root index | 12 bits | Selects a middle-level table |
| Middle index | 12 bits | Selects a leaf-level table |
| Leaf index | 12 bits | Selects the leaf PTE |

Each PTE occupies 8 simulated bytes. Therefore each level has 4096 entries and
occupies 32 KiB, or eight contiguous 4 KiB physical frames. This is an explicit
tradeoff: the requested three levels span 48 address bits, while conventional
4 KiB hardware page-table pages often use fewer index bits per level so one
table fits in one frame. The level count and page size are fixed in this first
version.

There is one shared radix-table root for the simulated workload and one data TLB
per PE. Middle and leaf tables are created lazily. Table contents are sparse C++
metadata; each table is also assigned simulated physical frames so that PTE
addresses can be sent through the modeled memory path. Data frames and page-table
frames come from the same monotonically allocated PFN space. Table allocations
are aligned to the eight-frame size needed by a table.

For a normal lower-half user-space address, the VPN is the low 36 bits above the
page offset. Canonical-address checking accepts either sign-extended half, but
the current VPN key uses the low 48 address bits; consequently, an upper-half
canonical address aliases its corresponding lower-half address. The intended
workload is ordinary user-space code using lower-half addresses.

## TLB organization

Each PE owns a data TLB with configurable entry count and associativity. The
number of sets is `tlb_entries / tlb_associativity`; the VPN modulo the set count
selects a set. Replacement within a set is least-recently-used, tracked by a
simulator-wide logical access counter. TLB state is private per PE, while page
table mappings are shared. Mapping or unmapping a page invalidates that VPN in
all PE TLBs.

The TLB lookup latency is a direct cycle cost charged on every lookup, on both
hits and misses. This is the only translation cost configured as a fixed local
latency. Page-walk and page-fault costs are produced by memory requests rather
than by separate fixed penalties.

## Walk and first-touch behavior

On a TLB miss, the translator computes the physical byte address of each
reachable PTE. The walker issues reads in dependency order: it waits for the
current level's request to complete before issuing the next level. If an
intermediate table does not exist, the walk stops at the last reachable PTE and
enters the first-touch path.

The first access to an unmapped VPN is treated as a simulated minor page fault:

1. Allocate any missing middle and leaf tables and one data frame.
2. Mark the new PTE as installing and issue writes for the page-table entries
   that were created.
3. Make the leaf mapping valid after those writes complete.
4. Retry the walk. The retry reads the now-reachable PTEs, updates accessed and
   (for a store) dirty state, and installs the translation in the requesting
   PE's TLB.

There is no separately charged OS handler instruction stream, scheduling delay,
or fixed page-fault penalty. The modeled fault cost is the latency of the PTE
writes and the retried reads. If multiple PEs fault on the same VPN at once, a
condition variable makes later installers wait for the in-progress mapping and
then retry the walk. The lock protecting page-table metadata is released while
memory requests are serviced, so independent walks can make progress in
parallel.

The leaf PTE tracks accessed and dirty state. The first load or store sets the
accessed bit; the first store also sets dirty. These updates are represented by
a PTE write. If a PE already has a clean translation in its TLB and later stores
to it, the TLB hit remains a hit but generates a dirty-bit PTE write. Subsequent
hits with no required PTE update do not access the page table.

## PTE request timing and memory path

PTE requests are issued by the PE that performed the access:

```text
QEMU guest VA
  -> qemu_zsim_plugin translation hook
  -> per-PE TLB / radix walker
  -> ALUCore::issuePageTableAccess
  -> PE memory interface
  -> configured local or routed memory service
```

The request is a tagged ZSim memory-interface request (`PAGETABLE`), using
`GETS` for reads and `GETX` for writes. Its address is the PTE's simulated
physical byte address converted to the memory interface's cache-line address.
The interface returns the service cycles, and the PE's simulated cycle advances
to the completion time. When the selected path has a detailed Garnet NoC,
requests follow that route; with the analytical model, the PE memory interface
uses its analytical local/remote service model. Configured placement and
connectivity therefore affect PTE service just as they affect other PE-MI
requests.

This implementation injects PTE traffic through the ZSim PE memory interface.
It does not create an independent page-walker timing model or submit each PTE
access as a live request to the standalone Ramulator controller. Do not interpret
PTE counters as standalone Ramulator controller transactions. The selected
PE-MI path supplies the modeled service and network timing; the PTE payload and
contents remain in translator metadata.

The TLB lookup cost is charged immediately before the first dependent PTE
request on a miss, or directly on a hit when no PTE update request is needed.
There is no extra fixed page-walk latency. A walk with three resident levels
normally performs three dependent reads; first-touch accesses additionally pay
for installation writes and the retry. A hit normally performs no PTE memory
request.

After translation, the original load/store is issued through the regular data
path. An operation crossing a page boundary is translated once for each page
segment; each segment may map to a different PFN.

## Configuration

```yaml
address_translation:
  enabled: true
  page_size_bytes: 4096
  tlb_entries: 64
  tlb_associativity: 4
  tlb_lookup_latency_cycles: 1
```

`examples/translation_study_ON.yaml` and
`examples/translation_study_OFF.yaml` provide matching starting points for
translation-on/off experiments. Keep the workload, memory, placement, NoC, and
other settings fixed while sweeping one translation parameter at a time. The
associativity must be a positive divisor of the entry count; invalid values are
replaced with a fully associative TLB after a warning. Zero entries are reset
to the default of 64.

Only 4096-byte pages are accepted. The supported integration is limited to
`method: exec`, device scope, and `alu_core` PEs. The parser/configuration and
validation details are in [the YAML reference](../yaml_reference.md).

## Statistics and experiment interpretation

The translator reports per-PE and aggregate statistics for both the region of
interest (ROI) and the whole program:

| Statistic | Meaning |
|---|---|
| Total translations | Number of translated page segments |
| TLB hits / misses | Data-TLB lookup outcomes |
| Page walks | Walk attempts after TLB misses, including retries |
| PTE reads / writes | Memory-interface requests for page-table entries |
| First-touch faults | Newly installed virtual pages |
| Translation cycles | TLB lookup cost plus serviced PTE request cycles |

For performance studies, report application cycles alongside these counters and
the total number of data memory accesses. The total translation cycles are
summed per PE and can overlap across PEs; they are not necessarily equal to the
translation contribution to elapsed wall-clock cycles. PTE reads/writes are
additional traffic beyond the workload's data accesses.

## Deliberate limits and next design axes

This first step leaves several behaviors out so that page size, TLB policy, walk
traffic, and memory placement can be studied independently:

- No OS-managed allocation, page reclamation, swapping, copy-on-write,
  permissions, privilege checks, or context/address-space identifiers.
- No page-table cache, walk cache, multi-level TLB, superpages, or configurable
  radix depth and PTE format.
- No full x86 page-table bit layout or hardware fault-handler instruction cost.
- Table contents are metadata, not bytes stored and read back from simulated
  memory; the PTE requests model the traffic and service time.
- PTE requests use the PE memory-interface path and do not become live standalone
  Ramulator controller requests in this implementation.
- Upper-half canonical addresses alias the corresponding lower-half VPN, as
  described above.

Useful next study axes include TLB hierarchy and replacement policies, page-walk
caches, alternative page sizes and radix shapes, shared versus private page
tables, explicit page placement/reclamation policy, and routing PTE requests to
the same cycle-accurate controller path used by the selected memory backend.

## Implementation map

| File | Responsibility |
|---|---|
| [`address_translator.h`](../../include/address_translation/address_translator.h) | Configuration, table/TLB metadata, statistics, translator interface |
| [`address_translator.cpp`](../../src/address_translation/address_translator.cpp) | Radix walk, TLB replacement, PFN allocation, faults, accessed/dirty updates |
| [`qemu_zsim_plugin.cpp`](../../external/zsim/src/qemu_zsim_plugin.cpp) | Applies translation to QEMU load/store callbacks and charges lookup latency |
| [`alu_core.cpp`](../../external/zsim/src/alu_core.cpp) | Turns PTE reads/writes into timed PE memory-interface requests |
| [`pe_memory_interface.h`](../../external/zsim/src/pe_memory_interface.h) | Routes PE requests and computes local/network service timing |
| [`main.cpp`](../../src/main.cpp) | Parses and passes address-translation configuration |

