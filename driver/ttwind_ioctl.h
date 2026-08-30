/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ttwind_ioctl.h - public user/kernel interface of the tt-wind driver.
 *
 * This header is shared between the driver and the Windows backend of
 * tt-kmd-lib (tt-umd). It must compile in both kernel mode (wdm.h) and
 * user mode (windows.h / winioctl.h), so it uses only fixed-width types
 * and defines nothing beyond the wire format.
 *
 * The IOCTL wire format is private to tt-wind and tt-umd's tt-kmd-lib
 * backend; the stable cross-project contract is the tt_kmd_lib.h API.
 */

#pragma once

/*
 * Device interface class GUID.
 *
 * Exactly one translation unit per binary must #include <initguid.h>
 * before this header so the GUID below is instantiated rather than
 * merely declared.
 *
 * User mode enumerates Tenstorrent devices with
 * CM_Get_Device_Interface_List(&GUID_DEVINTERFACE_TTWIND, ...) and opens
 * the returned symbolic links with CreateFile.
 *
 * {1f26945f-8d80-4dbe-b4af-7c60eb3c630f}
 */
DEFINE_GUID(GUID_DEVINTERFACE_TTWIND,
    0x1f26945f, 0x8d80, 0x4dbe, 0xb4, 0xaf, 0x7c, 0x60, 0xeb, 0x3c, 0x63, 0x0f);

/*
 * IOCTL codes.
 *
 * Device type: values 0x8000-0xFFFF are reserved for vendors. Function
 * codes likewise start at 0x800. METHOD_BUFFERED and FILE_ANY_ACCESS
 * throughout; access control is done at open time via the device's
 * security descriptor.
 */
#define TTWIND_DEVICE_TYPE 0x8D80u /* arbitrary vendor value, from the GUID */

#define IOCTL_TTWIND_GET_DEVICE_INFO \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_MAP_BAR \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_UNMAP_BAR \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_ALLOCATE_TLB \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_FREE_TLB \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_CONFIGURE_TLB \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_MAP_TLB \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_SMC_MSG \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_RESET_DEVICE \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_ARC_STATUS \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_POST_RESET \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_QUERY_SYSMEM \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TTWIND_MAP_SYSMEM \
    CTL_CODE(TTWIND_DEVICE_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* A PCI device decodes at most six 32-bit BARs. */
#define TTWIND_MAX_BARS 6u

/*
 * One PCI base address register as seen by the kernel.
 *
 * Phys/Size are zero for BARs the device does not implement. Phys is the
 * translated CPU physical address of the region; Size its length in
 * bytes. 64-bit BARs occupy one slot (the index of their low half).
 */
typedef struct _TTWIND_BAR_DESC {
    unsigned __int64 Phys;
    unsigned __int64 Size;
} TTWIND_BAR_DESC;

/*
 * Output of IOCTL_TTWIND_GET_DEVICE_INFO. No input buffer.
 *
 * All padding is explicit so the layout is identical for every compiler
 * and packing setting: 24-byte fixed header + 6 * 16-byte BAR table =
 * 120 bytes total.
 */
typedef struct _TTWIND_DEVICE_INFO_OUT {
    unsigned short VendorId;            /* PCI config 0x00 (0x1E52)      */
    unsigned short DeviceId;            /* PCI config 0x02               */
    unsigned short SubsystemVendorId;   /* PCI config 0x2C               */
    unsigned short SubsystemId;         /* PCI config 0x2E               */
    unsigned char  Bus;                 /* PCI location: bus             */
    unsigned char  Device;              /* PCI location: device          */
    unsigned char  Function;            /* PCI location: function        */
    unsigned char  Reserved0;           /* explicit pad, zero            */
    unsigned short PciDomain;           /* PCI segment/domain            */
    unsigned short Reserved1;           /* explicit pad, zero            */
    unsigned int   BarCount;            /* implemented BARs (Phys != 0)  */
    unsigned int   Reserved2;           /* explicit pad, zero            */
    TTWIND_BAR_DESC Bars[TTWIND_MAX_BARS]; /* indexed by BAR number      */
} TTWIND_DEVICE_INFO_OUT;

/* --- BAR / TLB window mapping ---------------------------------------- */

/*
 * Cache modes for user mappings of device memory.
 *
 * UC (MmNonCached) for register access, WC (MmWriteCombined) for bulk
 * data movement through TLB windows.
 */
#define TTWIND_CACHE_UC 0u
#define TTWIND_CACHE_WC 1u

/*
 * Upper bound on a single MAP_BAR request. A user mapping is described
 * by one MDL, and an MDL's PFN array shares a 16-bit size field with its
 * header, which caps a single MDL at roughly 32 MiB on x64. 8 MiB keeps
 * a wide margin while covering every register block and TLB window;
 * larger BAR regions must be mapped in chunks.
 */
#define TTWIND_MAX_MAP_BYTES (8ull * 1024ull * 1024ull)

/* Blackhole exposes 202 2 MiB TLB windows at the bottom of BAR0. */
#define TTWIND_TLB_WINDOW_SIZE_2M   (2ull * 1024ull * 1024ull)
#define TTWIND_TLB_2M_WINDOW_COUNT  202u

/*
 * Input of IOCTL_TTWIND_MAP_BAR.
 *
 * Maps [Offset, Offset+Length) of the given BAR into the calling
 * process. Offset and Length must be page aligned, Length nonzero and
 * at most TTWIND_MAX_MAP_BYTES. CacheMode is TTWIND_CACHE_*.
 */
typedef struct _TTWIND_MAP_BAR_IN {
    unsigned int     BarIndex;   /* index into the GET_DEVICE_INFO BAR table */
    unsigned int     CacheMode;  /* TTWIND_CACHE_UC / TTWIND_CACHE_WC        */
    unsigned __int64 Offset;     /* byte offset into the BAR, page aligned   */
    unsigned __int64 Length;     /* bytes to map, page aligned, nonzero      */
} TTWIND_MAP_BAR_IN;

/* Output of IOCTL_TTWIND_MAP_BAR. */
typedef struct _TTWIND_MAP_BAR_OUT {
    unsigned __int64 UserVa;     /* base of the new user mapping */
    unsigned __int64 Length;     /* bytes actually mapped        */
} TTWIND_MAP_BAR_OUT;

/*
 * Input of IOCTL_TTWIND_UNMAP_BAR. No output buffer.
 *
 * UserVa must be the exact base address returned by a prior MAP_BAR or
 * MAP_TLB on the same handle. (Despite the name, this unmaps any user
 * mapping the driver created for this handle.) All mappings that are
 * still live when the handle closes are unmapped automatically.
 */
typedef struct _TTWIND_UNMAP_BAR_IN {
    unsigned __int64 UserVa;
} TTWIND_UNMAP_BAR_IN;

/*
 * Input of IOCTL_TTWIND_ALLOCATE_TLB.
 *
 * Size selects the window kind; only TTWIND_TLB_WINDOW_SIZE_2M is
 * accepted for now (Blackhole's 4 GiB windows come later). The returned
 * TlbId is owned by the issuing handle: only that handle may configure,
 * map, or free it, and it is freed automatically when the handle closes.
 */
typedef struct _TTWIND_ALLOCATE_TLB_IN {
    unsigned __int64 Size;
} TTWIND_ALLOCATE_TLB_IN;

/* Output of IOCTL_TTWIND_ALLOCATE_TLB. */
typedef struct _TTWIND_ALLOCATE_TLB_OUT {
    unsigned int TlbId;
    unsigned int Reserved;       /* zero */
} TTWIND_ALLOCATE_TLB_OUT;

/*
 * Input of IOCTL_TTWIND_FREE_TLB. No output buffer.
 *
 * Fails with STATUS_INVALID_DEVICE_STATE while a user mapping of the
 * window is still live; unmap first (IOCTL_TTWIND_UNMAP_BAR).
 */
typedef struct _TTWIND_FREE_TLB_IN {
    unsigned int TlbId;
    unsigned int Reserved;       /* zero */
} TTWIND_FREE_TLB_IN;

/*
 * NOC addressing configuration for a TLB window. Semantics mirror the
 * Linux driver's struct tenstorrent_noc_tlb_config.
 *
 * Addr must be aligned to the window size. For unicast, (XEnd, YEnd) is
 * the target core and XStart/YStart are 0; for multicast, the Start/End
 * pairs bound the target rectangle and Mcast is 1. Ordering is 0..3
 * (0 = default/relaxed, 1 = strict, 2 = posted); StaticVc enables the
 * static virtual channel selection. Reserved fields must be zero.
 */
typedef struct _TTWIND_NOC_TLB_CONFIG {
    unsigned __int64 Addr;
    unsigned short   XEnd;
    unsigned short   YEnd;
    unsigned short   XStart;
    unsigned short   YStart;
    unsigned char    Noc;        /* 0 or 1        */
    unsigned char    Mcast;      /* 0 or 1        */
    unsigned char    Ordering;   /* 0..3          */
    unsigned char    Linked;     /* 0 or 1        */
    unsigned char    StaticVc;   /* 0 or 1        */
    unsigned char    Reserved0[3];
    unsigned int     Reserved1[2];
} TTWIND_NOC_TLB_CONFIG;

/* Input of IOCTL_TTWIND_CONFIGURE_TLB. No output buffer. */
typedef struct _TTWIND_CONFIGURE_TLB_IN {
    unsigned int          TlbId;
    unsigned int          Reserved;  /* zero */
    TTWIND_NOC_TLB_CONFIG Config;
} TTWIND_CONFIGURE_TLB_IN;

/*
 * Input of IOCTL_TTWIND_MAP_TLB.
 *
 * Maps the window's 2 MiB slice of BAR0 into the calling process. The
 * window may be (re)configured while mapped.
 */
typedef struct _TTWIND_MAP_TLB_IN {
    unsigned int TlbId;
    unsigned int CacheMode;      /* TTWIND_CACHE_UC / TTWIND_CACHE_WC */
} TTWIND_MAP_TLB_IN;

/* Output of IOCTL_TTWIND_MAP_TLB. */
typedef struct _TTWIND_MAP_TLB_OUT {
    unsigned __int64 UserVa;
} TTWIND_MAP_TLB_OUT;

/* --- Host system memory (sysmem) ------------------------------------- */

/*
 * Output of IOCTL_TTWIND_QUERY_SYSMEM. No input buffer.
 *
 * The driver allocates one physically contiguous, cached host buffer at
 * device start and exposes it to the chip through outbound iATU region 0
 * of the PCIe controller: NOC reads/writes addressed to
 * [NocAddress, NocAddress + TotalSize) on the PCIe tile land in the
 * buffer. This is the backing store for tt-metal's command queue.
 *
 * @TotalSize: bytes of sysmem, 0 when sysmem is unavailable (allocation
 *      failed, the iATU could not be programmed, or the loopback
 *      self-verification failed - including transiently while a reset
 *      is in flight). All other fields are 0 whenever TotalSize is 0.
 * @NocAddress: NOC address of byte 0 of the buffer as seen by any tile
 *      issuing a request to the PCIe tile (Blackhole noc_pcie_offset,
 *      4 << 58).
 * @DeviceIoAddr: device-PCIe-space (iATU region base) address of byte 0
 *      (0 in this version; NocAddress = noc_pcie_offset + DeviceIoAddr).
 * @ChannelSize: bytes per channel; ChannelCount * ChannelSize ==
 *      TotalSize. One channel in this version.
 * @MaxMapBytes: largest Length a single MAP_SYSMEM accepts (the whole
 *      buffer in this version - section mapping has no MDL size cap).
 * @PcieTileX: NOC0 x-coordinate of the active PCIe tile (2 or 11 on
 *      Blackhole, read from the tile's NOC_ID register), y is 0. This
 *      is the tile to target when reaching sysmem through a TLB window.
 */
typedef struct _TTWIND_QUERY_SYSMEM_OUT {
    unsigned __int64 TotalSize;    /* 0 = sysmem unavailable            */
    unsigned __int64 NocAddress;   /* 4 << 58                           */
    unsigned __int64 DeviceIoAddr; /* iATU region base (0)              */
    unsigned __int64 ChannelSize;  /* == TotalSize (one channel)        */
    unsigned int     ChannelCount; /* 1                                 */
    unsigned int     MaxMapBytes;  /* per-MAP_SYSMEM cap (== TotalSize) */
    unsigned int     PcieTileX;    /* active PCIe tile NOC0 x (y = 0)   */
    unsigned int     Reserved;     /* zero                              */
} TTWIND_QUERY_SYSMEM_OUT;

/*
 * Input of IOCTL_TTWIND_MAP_SYSMEM.
 *
 * Maps [Offset, Offset+Length) of the sysmem buffer into the calling
 * process as one contiguous, CACHED, read/write view (the buffer itself
 * is cached host RAM; PCIe DMA is cache-coherent on x64). Offset and
 * Length must be page aligned, Length nonzero, and the range in bounds.
 * Unmap with IOCTL_TTWIND_UNMAP_BAR (exact UserVa); mappings still live
 * when the handle closes are torn down automatically.
 */
typedef struct _TTWIND_MAP_SYSMEM_IN {
    unsigned __int64 Offset;       /* byte offset, page aligned         */
    unsigned __int64 Length;       /* bytes, page aligned, nonzero      */
} TTWIND_MAP_SYSMEM_IN;

/* Output of IOCTL_TTWIND_MAP_SYSMEM. */
typedef struct _TTWIND_MAP_SYSMEM_OUT {
    unsigned __int64 UserVa;       /* base of the new user mapping      */
    unsigned __int64 Length;       /* bytes actually mapped             */
} TTWIND_MAP_SYSMEM_OUT;

/* --- ARC (SMC) firmware messaging / reset ---------------------------- */

/*
 * Input and output of IOCTL_TTWIND_SMC_MSG.
 *
 * One synchronous 8x32-bit message exchange with the ARC (SMC) firmware,
 * the same wire format as tt-kmd's TENSTORRENT_IOCTL_SMC_MSG (msgqueue.c
 * `struct arc_msg`): Message[0] is the header (message type in the low
 * byte; type-specific fields above it), Message[1..7] the payload. The
 * response overwrites all eight words.
 *
 * Unlike the Linux POST/POLL/ABANDON interface this call blocks (bounded
 * by the driver's internal ~1.5 s timeout) and returns the response
 * directly; the driver's sequential IOCTL queue serializes concurrent
 * callers. The driver does NOT interpret the firmware status in the
 * response Message[0]; a response with a nonzero status still completes
 * successfully. Errors:
 *   STATUS_NOT_SUPPORTED         no usable message queue (old firmware)
 *   STATUS_IO_TIMEOUT            firmware never produced a response
 *   STATUS_DEVICE_DOES_NOT_EXIST all-1s reads; device is gone/hung
 *   STATUS_DEVICE_NOT_READY      device not started / firmware not ready
 */
typedef struct _TTWIND_SMC_MSG_INOUT {
    unsigned int Message[8];
} TTWIND_SMC_MSG_INOUT;

/*
 * Input of IOCTL_TTWIND_RESET_DEVICE. No output buffer.
 *
 * Split reset model (v2, hardened after the 2026-08-30 hard-freeze;
 * mirrors tt-kmd's arm-then-recover flow): RESET_DEVICE only ARMS the
 * reset and returns immediately - it never sleeps, polls, or touches
 * BAR/MMIO space after the arm writes, because on this platform a
 * driver-side MMIO read during the momentary link-down window stalls
 * the CPU uninterruptibly and hard-freezes the machine. Recovery is
 * the separate IOCTL_TTWIND_POST_RESET below.
 *
 * What RESET_DEVICE does: refuse-if-busy checks -> bump the reset
 * generation (invalidating every OTHER open handle; the resetting
 * handle carries forward) -> reclaim all TLB window allocations ->
 * save config space -> set the reset marker (PCI COMMAND parity bit,
 * config write) -> arm the DBI interface-timer (two config writes that
 * make the chip reset itself; tt-kmd's Blackhole "ASIC reset") ->
 * enter the RESTRICTED state -> return. The PCIe link and the parent
 * bridge are never touched, and no MMIO/ARC liveness is probed first -
 * this reset must be available precisely when MMIO is wedged; only the
 * config-space vendor ID is checked (all-1s => device already off the
 * bus => STATUS_DEVICE_DOES_NOT_EXIST, nothing armed).
 *
 * It is refused with STATUS_DEVICE_BUSY while ANY user mapping of
 * device memory exists on any handle (no user-PTE revocation on
 * Windows yet, and a user read of BAR space during the link-down
 * window would stall the machine exactly like a kernel one). Unmap
 * everything first. Re-arming while already restricted is permitted
 * and idempotent (the original config save is kept).
 *
 * RESTRICTED state: until POST_RESET succeeds, only GET_DEVICE_INFO,
 * QUERY_SYSMEM (which then reports sysmem unavailable without touching
 * hardware), RESET_DEVICE, and POST_RESET are accepted; every other
 * ioctl fails
 * with STATUS_REINITIALIZATION_NEEDED before touching hardware. Stale
 * handles (opened before the reset) fail everything with
 * STATUS_DEVICE_REMOVED.
 *
 * Flags and Reserved must be 0.
 */
typedef struct _TTWIND_RESET_DEVICE_IN {
    unsigned int Flags;          /* must be 0 */
    unsigned int Reserved;       /* must be 0 */
} TTWIND_RESET_DEVICE_IN;

/*
 * Input of IOCTL_TTWIND_POST_RESET. No output buffer.
 *
 * Completes a reset armed by RESET_DEVICE. Cheap and retryable: give
 * the DBI timer a ~1-2 s grace period after RESET_DEVICE (tt-kmd's
 * userspace sleeps ~2 s before polling, warm_reset.cpp), then call
 * this repeatedly (e.g. every 100 ms, total budget ~15 s) until it
 * stops returning a retryable status. It performs, in order:
 * config-space probe for the device's return (vendor ID) ->
 * reset-marker check -> full config-space restore (BARs, MSI, PCIe
 * DevCtl incl. MaxPayload/MaxReadRequest; COMMAND register last) ->
 * re-save of the restored config -> ONE bounded MMIO sanity readback
 * (the first MMIO touch since the arm) -> leave the RESTRICTED state
 * -> best-effort ARC firmware power-up (failures logged, not fatal -
 * diagnose with ARC_STATUS / retry via SMC_MSG).
 *
 * Statuses:
 *  - STATUS_SUCCESS: recovered (or nothing was pending).
 *  - STATUS_DEVICE_DOES_NOT_EXIST: device not back on the bus yet;
 *    still restricted - keep polling.
 *  - STATUS_DEVICE_BUSY: reset pending - the device answers config
 *    cycles but the reset marker is still set, i.e. the DBI timer has
 *    not fired yet; still restricted - keep polling. If the FULL
 *    polling budget expires with every poll returning this, the chip
 *    ignored the trigger - that terminal diagnosis is the caller's,
 *    never the driver's (a single early sample cannot tell "not yet"
 *    from "never", and v100.3.4's in-driver verdict un-restricted the
 *    device just before a late-firing reset). Recover by re-arming
 *    RESET_DEVICE (allowed while restricted) or PnP disable/enable.
 *  - STATUS_DEVICE_DATA_ERROR: config restore failed/incomplete;
 *    still restricted - retry.
 *  - STATUS_IO_DEVICE_ERROR: the device answers config cycles but the
 *    MMIO sanity readback failed; still restricted - retry, or re-arm
 *    RESET_DEVICE.
 * The restricted state is never permanent: POST_RESET can always be
 * retried, RESET_DEVICE can always re-arm, and only PnP removal makes
 * the device dead.
 *
 * Flags and Reserved must be 0.
 */
typedef struct _TTWIND_POST_RESET_IN {
    unsigned int Flags;          /* must be 0 */
    unsigned int Reserved;       /* must be 0 */
} TTWIND_POST_RESET_IN;

/*
 * Output of IOCTL_TTWIND_ARC_STATUS. No input buffer.
 *
 * Diagnostic: performs one live, bounded ARC message-queue discovery
 * probe and reports what it observed. Read-only on the device (NOC
 * reads of the ARC tile only); always completes with STATUS_SUCCESS
 * when the device is started - the outcome is in the payload.
 *
 * The ARC APB block (reset-unit scratch registers and the doorbell) is
 * probed through three candidate routes on tile (8,0):
 *   1. NOC, low alias: NOC address = 0x80000000 + APB offset (what
 *      tt-kmd uses; boot status at 0x80030408),
 *   2. NOC, high window: NOC address = 0x8_80000000 + APB offset
 *      (Wormhole's ARC-over-NOC addressing),
 *   3. BAR0 AXI aperture: BAR0 offset 0x1FF00000 + APB offset (what
 *      tt-umd's read_from_arc_apb uses over PCIe when available).
 * The CSM (message ring memory) is always accessed via the NOC low
 * alias (NOC address = CSM address, 0x100xxxxx), which is what tt-kmd
 * does and is verified to decode on this hardware.
 *
 * @Stage: TTWIND_ARC_STAGE_*.
 * @LastStatus: NTSTATUS of the probe (0 on full success).
 * @BootStatusLow / @BootStatusHigh / @BootStatusAxi: raw single reads
 *      of SCRATCH_RAM_2 (boot status) through routes 1 / 2 / 3.
 *      BootStatusAxi is 0xFFFFFFFF when the aperture is unmapped
 *      (BAR0 too small).
 * @Route: TTWIND_ARC_ROUTE_* the driver selected for APB access.
 * @QcbPtr: raw SCRATCH_RAM_11 (queue control block pointer) read
 *      through the selected route; 0 if none selected.
 * @QueueBase / @NumEntries: decoded firmware queue, when Stage is
 *      TTWIND_ARC_STAGE_QUEUE_OK.
 */
#define TTWIND_ARC_STAGE_NOT_STARTED   0u /* device/hw not ready       */
#define TTWIND_ARC_STAGE_NO_BOOT_READY 1u /* no route showed ready     */
#define TTWIND_ARC_STAGE_BAD_QUEUE     2u /* ready, but QCB/queue bad  */
#define TTWIND_ARC_STAGE_QUEUE_OK      3u /* queue located             */

#define TTWIND_ARC_ROUTE_NONE     0u
#define TTWIND_ARC_ROUTE_NOC_LOW  1u /* NOC 0x80000000 + APB offset    */
#define TTWIND_ARC_ROUTE_NOC_HIGH 2u /* NOC 0x8_80000000 + APB offset  */
#define TTWIND_ARC_ROUTE_AXI      3u /* BAR0 0x1FF00000 + APB offset   */

typedef struct _TTWIND_ARC_STATUS_OUT {
    unsigned int Stage;
    unsigned int LastStatus;     /* NTSTATUS of the probe              */
    unsigned int BootStatusLow;  /* SCRATCH_RAM_2 via NOC low alias    */
    unsigned int BootStatusHigh; /* SCRATCH_RAM_2 via NOC high window  */
    unsigned int BootStatusAxi;  /* SCRATCH_RAM_2 via BAR0 aperture    */
    unsigned int Route;          /* TTWIND_ARC_ROUTE_* selected        */
    unsigned int QcbPtr;         /* SCRATCH_RAM_11 via selection       */
    unsigned int QueueBase;
    unsigned int NumEntries;
    unsigned int Reserved;       /* zero */
} TTWIND_ARC_STATUS_OUT;

#ifdef __cplusplus
static_assert(sizeof(TTWIND_DEVICE_INFO_OUT) == 120,
              "TTWIND_DEVICE_INFO_OUT wire size changed");
static_assert(sizeof(TTWIND_MAP_BAR_IN) == 24, "wire size");
static_assert(sizeof(TTWIND_MAP_BAR_OUT) == 16, "wire size");
static_assert(sizeof(TTWIND_UNMAP_BAR_IN) == 8, "wire size");
static_assert(sizeof(TTWIND_ALLOCATE_TLB_IN) == 8, "wire size");
static_assert(sizeof(TTWIND_ALLOCATE_TLB_OUT) == 8, "wire size");
static_assert(sizeof(TTWIND_FREE_TLB_IN) == 8, "wire size");
static_assert(sizeof(TTWIND_NOC_TLB_CONFIG) == 32, "wire size");
static_assert(sizeof(TTWIND_CONFIGURE_TLB_IN) == 40, "wire size");
static_assert(sizeof(TTWIND_MAP_TLB_IN) == 8, "wire size");
static_assert(sizeof(TTWIND_MAP_TLB_OUT) == 8, "wire size");
static_assert(sizeof(TTWIND_QUERY_SYSMEM_OUT) == 48, "wire size");
static_assert(sizeof(TTWIND_MAP_SYSMEM_IN) == 16, "wire size");
static_assert(sizeof(TTWIND_MAP_SYSMEM_OUT) == 16, "wire size");
static_assert(sizeof(TTWIND_SMC_MSG_INOUT) == 32, "wire size");
static_assert(sizeof(TTWIND_RESET_DEVICE_IN) == 8, "wire size");
static_assert(sizeof(TTWIND_POST_RESET_IN) == 8, "wire size");
static_assert(sizeof(TTWIND_ARC_STATUS_OUT) == 40, "wire size");
#else
C_ASSERT(sizeof(TTWIND_DEVICE_INFO_OUT) == 120);
C_ASSERT(sizeof(TTWIND_MAP_BAR_IN) == 24);
C_ASSERT(sizeof(TTWIND_MAP_BAR_OUT) == 16);
C_ASSERT(sizeof(TTWIND_UNMAP_BAR_IN) == 8);
C_ASSERT(sizeof(TTWIND_ALLOCATE_TLB_IN) == 8);
C_ASSERT(sizeof(TTWIND_ALLOCATE_TLB_OUT) == 8);
C_ASSERT(sizeof(TTWIND_FREE_TLB_IN) == 8);
C_ASSERT(sizeof(TTWIND_NOC_TLB_CONFIG) == 32);
C_ASSERT(sizeof(TTWIND_CONFIGURE_TLB_IN) == 40);
C_ASSERT(sizeof(TTWIND_MAP_TLB_IN) == 8);
C_ASSERT(sizeof(TTWIND_MAP_TLB_OUT) == 8);
C_ASSERT(sizeof(TTWIND_QUERY_SYSMEM_OUT) == 48);
C_ASSERT(sizeof(TTWIND_MAP_SYSMEM_IN) == 16);
C_ASSERT(sizeof(TTWIND_MAP_SYSMEM_OUT) == 16);
C_ASSERT(sizeof(TTWIND_SMC_MSG_INOUT) == 32);
C_ASSERT(sizeof(TTWIND_RESET_DEVICE_IN) == 8);
C_ASSERT(sizeof(TTWIND_POST_RESET_IN) == 8);
C_ASSERT(sizeof(TTWIND_ARC_STATUS_OUT) == 40);
#endif
