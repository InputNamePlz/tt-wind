# The userspace contract

What tt-umd needs from the kernel, distilled from tt-kmd (v2.11.x, IOCTL API v2) and
tt-umd's usage of it. `tt-kmd/test/` is the executable spec for the Linux side.

## Consumed via tt-kmd-lib (`tt-umd/tt-kmd-lib/src/tt_kmd_lib.c` — the file we replace)

- open/close per-chip device, exclusive-open mode
- device info: vendor/device/subsystem IDs, BDF, PCI domain, max DMA buf size
- driver version query
- BAR mapping discovery (BAR0/2/4, UC and WC flavors) + mmap
- TLB windows: allocate (sized), configure (NOC addr/coords/ordering/mcast), mmap UC/WC, free
- coherent DMA buffer alloc + mmap
- user page pinning → IOVA/NOC address (IOMMU/DMA-remapping path; preferred over hugepages)
- reset (with all user mappings revoked/zapped by the driver)
- power state (per-fd, aggregated across handles)
- 64-slot advisory inter-process locks
- NOC cleanup write registered for crash cleanup on handle close
- ARC/SMC firmware mailbox (POST/POLL/ABANDON)
- kernel-mediated scalar NOC read/write (reset-safe)

## Side channels tt-umd uses OUTSIDE tt-kmd-lib (need new shim entry points on Windows)

- direct BAR0/BAR2 mmap on its own fd (`pci_device.cpp`) — including writing TLB config
  registers from user space through BAR0
- sysfs reads: numa_node, revision, iommu_group/type, raw PCI config space, KMD version,
  slot mapping → Windows: SetupAPI / device properties
- device enumeration by listing `/dev/tenstorrent/` → Windows: device interface GUID +
  `CM_Get_Device_Interface_List`

## Not required (absent from the Linux design too)

- user-visible interrupt/event delivery (single MSI is driver-internal)
- read/write on the device node
- any DMA engine abstraction — bulk data moves through user-mapped TLB windows

## Deliberately dropped for v1

- EXPORT_TLB_DMABUF (RDMA P2P), MAP_PEER_BAR, hugepage semantics (use pinning instead),
  legacy reset flags, Grayskull
