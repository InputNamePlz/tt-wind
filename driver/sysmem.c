/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sysmem.c - host system memory (sysmem) for tt-wind.
 *
 * Sysmem gives the Blackhole device DMA access to one large host buffer
 * - the backing store for tt-metal's command queue. Three pieces:
 *
 * 1. Allocation (TtWindSysmemAllocate, from PrepareHardware): one
 *    physically contiguous, CACHED buffer - 1 GiB, falling back to
 *    512 MiB then 256 MiB - below the chip's 58-bit DMA reach,
 *    preferring the device's NUMA node, zeroed, kept for the device's
 *    started lifetime (freed at ReleaseHardware). Contiguity is what
 *    lets a single iATU region (and a single user view) cover it.
 *
 * 2. iATU programming + loopback verification (TtWindSysmemArm):
 *    outbound iATU region 0 of the PCIe controller (reached through
 *    BAR2) is pointed at the buffer: device-PCIe address
 *    [0, size-1] -> host physical [SysmemPhys, +size-1]. A NOC request
 *    to address TTWIND_BH_NOC_PCIE_OFFSET + off (4<<58 + off) on the
 *    PCIe tile then lands at buffer offset off. The register sequence
 *    mirrors tt-kmd's blackhole_configure_outbound_atu
 *    (blackhole.c:878-911) exactly.
 *
 *    Before sysmem is exposed, the driver proves the whole path with a
 *    loopback: a magic pattern is written into the buffer through the
 *    kernel VA, then read back OVER THE NOC - through the kernel TLB
 *    window aimed at the PCIe tile at 4<<58 + off. The active PCIe
 *    instance (NOC0 x = 2 or 11) is detected from the tile's NOC_ID
 *    register each time, replicating tt-kmd's
 *    blackhole_detect_pcie_noc_x (blackhole.c:356-360). A mismatch or
 *    all-1s read marks sysmem unavailable (QUERY reports size 0) but
 *    never fails device start.
 *
 *    Arm runs from SelfManagedIoInit/Restart (after the ARC power-up,
 *    so NOC traffic starts only once tile power is raised, mirroring
 *    tt-kmd where iATU setup happens after init_hardware; and a
 *    failure there is non-fatal by KMDF contract, whereas a
 *    PrepareHardware failure would fail device start) and from the
 *    POST_RESET recovery path (the chip's reset clears the iATU; the
 *    buffer and its mappings are host-side and survive).
 *
 * 3. User mapping (MAP_SYSMEM): one contiguous user VA over
 *    [Offset, Offset+Length) via a section view of
 *    \Device\PhysicalMemory. The existing MDL path was NOT reused
 *    because an MDL's 16-bit size field caps one mapping at ~32 MiB on
 *    x64 (see TTWIND_MAX_MAP_BYTES) - useless for a 1 GiB buffer -
 *    while a section view has no such cap and yields exactly one
 *    contiguous VA. \Device\PhysicalMemory (ZwOpenSection from kernel
 *    mode + ZwMapViewOfSection into the requestor process) was chosen
 *    over inventing a driver-created section because it is the one
 *    documented-object route to mapping arbitrary physical pages, the
 *    driver alone computes the physical range (bounds-checked against
 *    its own buffer, so no user influence beyond offset/length within
 *    it), and the view inherits the pages' cache attributes - MmCached,
 *    matching the kernel VA, so no attribute conflict. The view is
 *    tracked as a SECTION-kind record on the same per-file-object
 *    MappingList, so UNMAP_BAR, EvtFileCleanup, and ReleaseHardware
 *    tear it down through the shared path (mapping.c).
 *
 * Reset interaction: RESET_DEVICE clears SysmemVerified when it arms
 * (under ArcLock, before the chip can drop off the bus) and POST_RESET
 * re-arms after its MMIO gate. Suspend clears it likewise;
 * Restart re-arms. The buffer contents and any user views survive all
 * of that - only the on-chip window needs re-arming.
 */

/* ntifs.h first for Zw section APIs and the process-attach APIs. */
#include <ntifs.h>
#include "ttwind.h"

/* Allocation fallback tiers, largest first. */
static const SIZE_T TtWindSysmemTiers[] = {
    1024ull * 1024 * 1024,
    512ull * 1024 * 1024,
    256ull * 1024 * 1024,
};
C_ASSERT(ARRAYSIZE(TtWindSysmemTiers) == TTWIND_SYSMEM_ALLOC_TIERS);

/* User views of \Device\PhysicalMemory must start on the 64 KiB
 * allocation granularity; sub-granularity offsets are absorbed by
 * mapping from the aligned-down address and returning base + delta. */
#define TTWIND_VIEW_ALIGN 0x10000ull

/*
 * TtWindSysmemAllocate - called from PrepareHardware. Best-effort: on
 * total failure sysmem is simply unavailable (logged; QUERY reports 0).
 */
VOID
TtWindSysmemAllocate(
    _In_ WDFDEVICE Device
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    PHYSICAL_ADDRESS lowest;
    PHYSICAL_ADDRESS highest;
    PHYSICAL_ADDRESS boundary;
    NODE_REQUIREMENT node = MM_ANY_NODE_OK;
    USHORT numaNode = 0;
    ULONG i;

    NT_ASSERT(ctx->SysmemVa == NULL);

    lowest.QuadPart = 0;
    highest.QuadPart = (LONGLONG)TTWIND_BH_DMA_ADDRESS_LIMIT;
    boundary.QuadPart = 0;

    /* Prefer the device's NUMA node so DMA stays local. */
    if (NT_SUCCESS(IoGetDeviceNumaNode(
            WdfDeviceWdmGetPhysicalDevice(Device), &numaNode))) {
        node = numaNode;
    }

    for (i = 0; i < ARRAYSIZE(TtWindSysmemTiers); i++) {
        ctx->SysmemVa = MmAllocateContiguousMemorySpecifyCacheNode(
            TtWindSysmemTiers[i], lowest, highest, boundary, MmCached,
            node);
        if (ctx->SysmemVa != NULL) {
            ctx->SysmemSize = TtWindSysmemTiers[i];
            ctx->SysmemTierResult[i] = TTWIND_SYSMEM_TIER_OK;
            break;
        }
        ctx->SysmemTierResult[i] = TTWIND_SYSMEM_TIER_FAILED;
        KdPrint(("ttwind: sysmem tier %Iu bytes unavailable, falling "
                 "back\n", TtWindSysmemTiers[i]));
    }

    if (ctx->SysmemVa == NULL) {
        KdPrint(("ttwind: no contiguous sysmem could be allocated; "
                 "sysmem unavailable\n"));
        ctx->SysmemSize = 0;
        return;
    }

    ctx->SysmemPhys = MmGetPhysicalAddress(ctx->SysmemVa);
    RtlZeroMemory(ctx->SysmemVa, (SIZE_T)ctx->SysmemSize);

    KdPrint(("ttwind: sysmem %I64u MiB at phys 0x%I64X (numa %u)\n",
             ctx->SysmemSize >> 20, (UINT64)ctx->SysmemPhys.QuadPart,
             (node == MM_ANY_NODE_OK) ? 0xFFFFFFFFu : (UINT32)numaNode));
}

/*
 * TtWindSysmemFree - called from ReleaseHardware, strictly AFTER
 * TtWindRevokeAllMappings has torn down every user view of the buffer
 * (freeing pages a user still maps would hand reallocated memory to
 * that process).
 */
VOID
TtWindSysmemFree(
    _In_ WDFDEVICE Device
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);

    if (ctx->SysmemVa != NULL) {
        MmFreeContiguousMemorySpecifyCache(ctx->SysmemVa,
                                           (SIZE_T)ctx->SysmemSize,
                                           MmCached);
        ctx->SysmemVa = NULL;
    }
    ctx->SysmemSize = 0;
    ctx->SysmemPhys.QuadPart = 0;
    ctx->SysmemVerified = FALSE;
    ctx->PcieTileX = 0;
}

/*
 * Program one outbound iATU region. Mirrors tt-kmd's
 * blackhole_configure_outbound_atu (blackhole.c:878-911): same register
 * order (BASE, TARGET, LIMIT, then CTRL_1/2/3), same control values
 * (CTRL_1 = INCREASE_REGION_SIZE so the limit can exceed 4 GiB, CTRL_2
 * = REGION_EN unless limit == 0 which disables the region, CTRL_3 = 0),
 * same 1 TiB size cap. Caller holds ArcLock; Ctx->Bar2Iatu is mapped.
 */
static NTSTATUS
TtWindIatuProgramOutbound(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT32 Region,
    _In_ UINT64 Base,
    _In_ UINT64 Limit,
    _In_ UINT64 Target
    )
{
    UINT64 size = Limit - Base + 1;
    UINT32 ctrl1 = TTWIND_BH_IATU_INCREASE_REGION_SIZE;
    UINT32 ctrl2 = (Limit == 0) ? 0 : TTWIND_BH_IATU_REGION_EN;
    PUCHAR regs;

    NT_ASSERT(Ctx->Bar2Iatu != NULL);

    if (size > TTWIND_BH_IATU_MAX_REGION_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Region >= TTWIND_BH_IATU_OUTBOUND_REGIONS) {
        return STATUS_INVALID_PARAMETER;
    }

    regs = Ctx->Bar2Iatu + TTWIND_BH_IATU_BASE +
           (Region * TTWIND_BH_IATU_REGION_STRIDE);

#define TTWIND_IATU_WRITE(off, val) \
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + (off)), (ULONG)(val))

    TTWIND_IATU_WRITE(TTWIND_BH_IATU_LOWER_BASE,   (UINT32)Base);
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_UPPER_BASE,   (UINT32)(Base >> 32));
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_LOWER_TARGET, (UINT32)Target);
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_UPPER_TARGET, (UINT32)(Target >> 32));
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_LOWER_LIMIT,  (UINT32)Limit);
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_UPPER_LIMIT,  (UINT32)(Limit >> 32));
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_REGION_CTRL_1, ctrl1);
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_REGION_CTRL_2, ctrl2);
    TTWIND_IATU_WRITE(TTWIND_BH_IATU_REGION_CTRL_3, 0);

#undef TTWIND_IATU_WRITE

    Ctx->OutboundIatu[Region].Base = Base;
    Ctx->OutboundIatu[Region].Limit = Limit;
    Ctx->OutboundIatu[Region].Target = Target;
    Ctx->OutboundIatu[Region].Enabled = (ctrl2 != 0);

    return STATUS_SUCCESS;
}

/*
 * Detect the active PCIe instance from the NOC_ID register of the PCIe
 * tile the BAR access rides through (tt-kmd blackhole_detect_pcie_noc_x,
 * blackhole.c:356-360: x = NOC_ID & 0x3F, valid when 2 or 11; y is 0).
 * Caller holds ArcLock.
 */
static NTSTATUS
TtWindSysmemDetectPcieTile(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx
    )
{
    ULONG nocId;
    UINT32 x;

    if (Ctx->NocIdRegs == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    nocId = READ_REGISTER_ULONG((volatile ULONG *)
        (Ctx->NocIdRegs + (TTWIND_BH_NOC_ID_OFFSET & 0xFFFu)));
    Ctx->SysmemNocIdRaw = nocId; /* diagnostic (SYSMEM_STATUS) */
    if (nocId == 0xFFFFFFFFu) {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    x = nocId & 0x3Fu;
    if (x != 2 && x != 11) {
        KdPrint(("ttwind: unexpected PCIe NOC_ID 0x%08X (x=%u)\n",
                 nocId, x));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Ctx->PcieTileX = x;
    return STATUS_SUCCESS;
}

/*
 * Read one dword back over the NOC: aim the kernel TLB window at the
 * 2 MiB block of NOC address Addr on the PCIe tile (NOC0, unicast,
 * strict ordering - the same configuration arc.c uses for the ARC
 * tile) and read. The window's 43-bit address field takes Addr >> 21;
 * (4<<58 + off) >> 21 = 1<<39 + ... fits, and TtWindProgramTlb2M's
 * field packer handles fields above bit 63 correctly (the 100.3.2.0
 * shift-UB bug class; tlb.c TtWindPutField). Caller holds ArcLock.
 */
static UINT32
TtWindSysmemNocRead32(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT64 Addr
    )
{
    TTWIND_NOC_TLB_CONFIG cfg;

    RtlZeroMemory(&cfg, sizeof(cfg));
    cfg.Addr = Addr & ~((UINT64)TTWIND_BH_TLB_2M_SIZE - 1);
    cfg.XEnd = (unsigned short)Ctx->PcieTileX;
    cfg.YEnd = 0;
    cfg.Noc = 0;
    cfg.Ordering = 1; /* strict */

    TtWindProgramTlb2M(Ctx, TTWIND_BH_KERNEL_TLB_INDEX, &cfg);

    return READ_REGISTER_ULONG((volatile ULONG *)
        (Ctx->KernelTlb + (Addr & (TTWIND_BH_TLB_2M_SIZE - 1))));
}

/*
 * Loopback self-verification: write magic dwords into the buffer
 * through the kernel VA, read them back over the NOC through the
 * outbound iATU, compare. Bounded (six 32-bit NOC reads); an all-1s
 * read fails immediately. The probe locations are re-zeroed afterwards
 * so the buffer stays in its advertised all-zero state. Caller holds
 * ArcLock; iATU region 0 is programmed; PcieTileX is detected.
 */
static NTSTATUS
TtWindSysmemLoopbackVerify(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx
    )
{
    UINT64 offsets[TTWIND_SYSMEM_LOOPBACK_PROBES];
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    offsets[0] = 0;
    offsets[1] = Ctx->SysmemSize / 2;
    offsets[2] = Ctx->SysmemSize - 4096;

    /* Fresh probe records for this attempt (SYSMEM_STATUS diagnostic);
     * entries an early failure never reaches stay all-zero. */
    RtlZeroMemory(Ctx->SysmemProbes, sizeof(Ctx->SysmemProbes));

    for (i = 0; i < ARRAYSIZE(offsets); i++) {
        volatile UINT32 *p = (volatile UINT32 *)
            ((PUCHAR)Ctx->SysmemVa + offsets[i]);

        p[0] = 0x74744D30u + i;          /* "ttM0" + i */
        p[1] = ~(0x74744D30u + i);
    }

    /* Writes to cached RAM; PCIe DMA snoops caches on x64, but make
     * sure the compiler has emitted them before the NOC reads. */
    KeMemoryBarrier();

    for (i = 0; i < ARRAYSIZE(offsets); i++) {
        UINT64 noc = TTWIND_BH_NOC_PCIE_OFFSET + offsets[i];
        UINT32 v0 = TtWindSysmemNocRead32(Ctx, noc);
        UINT32 v1;

        Ctx->SysmemProbes[i].Offset = offsets[i];
        Ctx->SysmemProbes[i].Wrote0 = 0x74744D30u + i;
        Ctx->SysmemProbes[i].Wrote1 = ~(0x74744D30u + i);
        Ctx->SysmemProbes[i].Read0 = v0;

        if (v0 == 0xFFFFFFFFu) {
            KdPrint(("ttwind: sysmem loopback read all-1s at NOC "
                     "0x%I64X\n", noc));
            status = STATUS_DEVICE_DOES_NOT_EXIST;
            break;
        }
        v1 = TtWindSysmemNocRead32(Ctx, noc + 4);
        Ctx->SysmemProbes[i].Read1 = v1;

        if (v0 != 0x74744D30u + i || v1 != ~(0x74744D30u + i)) {
            KdPrint(("ttwind: sysmem loopback mismatch at offset "
                     "0x%I64X: read %08X %08X, expected %08X %08X\n",
                     offsets[i], v0, v1,
                     0x74744D30u + i, ~(0x74744D30u + i)));
            status = STATUS_IO_DEVICE_ERROR;
            break;
        }
    }

    for (i = 0; i < ARRAYSIZE(offsets); i++) {
        volatile UINT32 *p = (volatile UINT32 *)
            ((PUCHAR)Ctx->SysmemVa + offsets[i]);

        p[0] = 0;
        p[1] = 0;
    }

    return status;
}

/*
 * TtWindSysmemArm - (re)program outbound iATU region 0 and run the
 * loopback verification; sysmem is exposed (SysmemVerified) only when
 * both pass. Idempotent; called from SelfManagedIoInit/Restart and the
 * POST_RESET recovery path. Callers treat failure as "sysmem
 * unavailable", never as a device-start failure.
 */
NTSTATUS
TtWindSysmemArm(
    _In_ WDFDEVICE Device
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    NTSTATUS status;

    WdfWaitLockAcquire(ctx->ArcLock, NULL);

    ctx->SysmemVerified = FALSE;

    if (ctx->NeedsHwInit) {
        /*
         * Restricted: no MMIO while a reset is in flight (reset.c).
         * Not a real attempt - deliberately do NOT overwrite the
         * recorded stage/status of the last real one.
         */
        status = STATUS_REINITIALIZATION_NEEDED;
        goto out;
    }
    if (ctx->SysmemVa == NULL) {
        ctx->SysmemStage = TTWIND_SYSMEM_STAGE_NO_ALLOC;
        status = STATUS_INSUFFICIENT_RESOURCES; /* allocation failed */
        goto record;
    }
    if (ctx->Bar2Iatu == NULL || ctx->KernelTlb == NULL ||
        ctx->TlbRegs == NULL) {
        ctx->SysmemStage = TTWIND_SYSMEM_STAGE_NO_REGS;
        status = STATUS_DEVICE_NOT_READY;
        goto record;
    }

    ctx->SysmemStage = TTWIND_SYSMEM_STAGE_TILE_DETECT;
    status = TtWindSysmemDetectPcieTile(ctx);
    if (!NT_SUCCESS(status)) {
        goto record;
    }

    ctx->SysmemStage = TTWIND_SYSMEM_STAGE_IATU;
    status = TtWindIatuProgramOutbound(ctx, 0, 0, ctx->SysmemSize - 1,
                                       (UINT64)ctx->SysmemPhys.QuadPart);
    if (!NT_SUCCESS(status)) {
        goto record;
    }

    ctx->SysmemStage = TTWIND_SYSMEM_STAGE_LOOPBACK;
    status = TtWindSysmemLoopbackVerify(ctx);
    if (!NT_SUCCESS(status)) {
        goto record;
    }

    ctx->SysmemStage = TTWIND_SYSMEM_STAGE_VERIFIED;
    ctx->SysmemVerified = TRUE;
    KdPrint(("ttwind: sysmem armed: %I64u MiB at NOC 0x%I64X via PCIe "
             "tile (%u, 0)\n", ctx->SysmemSize >> 20,
             TTWIND_BH_NOC_PCIE_OFFSET, ctx->PcieTileX));

record:
    ctx->SysmemLastStatus = (UINT32)status;
out:
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: sysmem arm failed 0x%08X; sysmem "
                 "unavailable\n", status));
    }
    WdfWaitLockRelease(ctx->ArcLock);
    return status;
}

/*
 * TtWindSysmemInvalidate - mark sysmem unavailable (no MMIO). Called
 * from SelfManagedIoSuspend before the power-down; the matching
 * Restart re-arms.
 */
VOID
TtWindSysmemInvalidate(
    _In_ WDFDEVICE Device
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);

    WdfWaitLockAcquire(ctx->ArcLock, NULL);
    ctx->SysmemVerified = FALSE;
    WdfWaitLockRelease(ctx->ArcLock);
}

/*
 * Handler for IOCTL_TTWIND_QUERY_SYSMEM. Reports all zeros when sysmem
 * is unavailable (including while a reset is in flight - this ioctl is
 * on the restricted-state allow list because it touches no hardware).
 */
NTSTATUS
TtWindIoctlQuerySysmem(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    TTWIND_QUERY_SYSMEM_OUT *out;
    NTSTATUS status;

    *BytesWritten = 0;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(out, sizeof(*out));

    WdfWaitLockAcquire(ctx->ArcLock, NULL);
    if (ctx->SysmemVerified) {
        out->TotalSize = ctx->SysmemSize;
        out->NocAddress = TTWIND_BH_NOC_PCIE_OFFSET;
        out->DeviceIoAddr = 0;
        out->ChannelSize = ctx->SysmemSize;
        out->ChannelCount = 1;
        out->MaxMapBytes = (unsigned int)ctx->SysmemSize;
        out->PcieTileX = ctx->PcieTileX;
    }
    WdfWaitLockRelease(ctx->ArcLock);

    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}

/*
 * Handler for IOCTL_TTWIND_SYSMEM_STATUS - the diagnostic mirror of the
 * arm path (the ARC_STATUS pattern): report how far the last arm
 * attempt got and what it observed, changing nothing. The only hardware
 * touch is a bounded read-only readback of outbound iATU region 0's
 * nine registers, skipped (IatuValid = 0) while restricted or when BAR2
 * is unmapped - the dispatcher already blocks this ioctl while
 * restricted; the in-handler check is defense in depth.
 */
NTSTATUS
TtWindIoctlSysmemStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    TTWIND_SYSMEM_STATUS_OUT *out;
    ULONG i;
    NTSTATUS status;

    *BytesWritten = 0;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(out, sizeof(*out));

    WdfWaitLockAcquire(ctx->ArcLock, NULL);

    out->Stage = ctx->SysmemStage;
    out->LastStatus = ctx->SysmemLastStatus;
    out->TotalSize = ctx->SysmemSize;
    out->SysmemPhys = (unsigned __int64)ctx->SysmemPhys.QuadPart;
    for (i = 0; i < TTWIND_SYSMEM_ALLOC_TIERS; i++) {
        out->TierBytes[i] = TtWindSysmemTiers[i];
        out->TierResult[i] = ctx->SysmemTierResult[i];
    }
    out->NocIdRaw = ctx->SysmemNocIdRaw;
    out->PcieTileX = ctx->PcieTileX;
    out->Verified = ctx->SysmemVerified ? 1u : 0u;

    RtlCopyMemory(out->Probes, ctx->SysmemProbes, sizeof(out->Probes));

    if (!ctx->NeedsHwInit && ctx->Bar2Iatu != NULL) {
        /* Live region-0 readback, in the arm path's write order. */
        static const UINT32 regOrder[TTWIND_SYSMEM_IATU_REGS] = {
            TTWIND_BH_IATU_LOWER_BASE,   TTWIND_BH_IATU_UPPER_BASE,
            TTWIND_BH_IATU_LOWER_TARGET, TTWIND_BH_IATU_UPPER_TARGET,
            TTWIND_BH_IATU_LOWER_LIMIT,  TTWIND_BH_IATU_UPPER_LIMIT,
            TTWIND_BH_IATU_REGION_CTRL_1, TTWIND_BH_IATU_REGION_CTRL_2,
            TTWIND_BH_IATU_REGION_CTRL_3,
        };
        PUCHAR regs = ctx->Bar2Iatu + TTWIND_BH_IATU_BASE;

        for (i = 0; i < TTWIND_SYSMEM_IATU_REGS; i++) {
            out->Iatu[i] = READ_REGISTER_ULONG(
                (volatile ULONG *)(regs + regOrder[i]));
        }
        out->IatuValid = 1;
    }

    WdfWaitLockRelease(ctx->ArcLock);

    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}

/*
 * Map [Phys, Phys+Length) of the sysmem buffer into Process as one
 * contiguous cached view of \Device\PhysicalMemory. On success *OutBase
 * is the view base (for ZwUnmapViewOfSection) and *OutVa the address of
 * the first requested byte (base plus the sub-64K alignment delta).
 * Runs at PASSIVE_LEVEL.
 */
static NTSTATUS
TtWindMapSysmemToUser(
    _In_ UINT64 Phys,
    _In_ SIZE_T Length,
    _In_ PEPROCESS Process,
    _Out_ PVOID *OutBase,
    _Out_ PVOID *OutVa
    )
{
    static const UNICODE_STRING physMemName =
        RTL_CONSTANT_STRING(L"\\Device\\PhysicalMemory");
    OBJECT_ATTRIBUTES oa;
    HANDLE sectionHandle = NULL;
    KAPC_STATE apcState;
    BOOLEAN attached = FALSE;
    LARGE_INTEGER sectionOffset;
    SIZE_T viewSize;
    PVOID base = NULL;
    UINT64 alignedPhys;
    UINT64 delta;
    NTSTATUS status;

    *OutBase = NULL;
    *OutVa = NULL;

    InitializeObjectAttributes(&oa, (PUNICODE_STRING)&physMemName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    status = ZwOpenSection(&sectionHandle,
                           SECTION_MAP_READ | SECTION_MAP_WRITE, &oa);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: ZwOpenSection(PhysicalMemory) failed 0x%08X\n",
                 status));
        return status;
    }

    alignedPhys = Phys & ~(TTWIND_VIEW_ALIGN - 1);
    delta = Phys - alignedPhys;
    sectionOffset.QuadPart = (LONGLONG)alignedPhys;
    viewSize = Length + (SIZE_T)delta;

    if (Process != PsGetCurrentProcess()) {
        KeStackAttachProcess((PRKPROCESS)Process, &apcState);
        attached = TRUE;
    }

    /*
     * Map into the (now current) process's user address space. No
     * PAGE_NOCACHE/PAGE_WRITECOMBINE modifier: the view takes the
     * pages' existing attribute, MmCached, matching the kernel VA. No
     * execute access is granted (PAGE_READWRITE only).
     */
    status = ZwMapViewOfSection(sectionHandle, ZwCurrentProcess(), &base,
                                0,          /* ZeroBits              */
                                0,          /* CommitSize            */
                                &sectionOffset, &viewSize, ViewUnmap,
                                0,          /* AllocationType        */
                                PAGE_READWRITE);

    if (attached) {
        KeUnstackDetachProcess(&apcState);
    }
    ZwClose(sectionHandle);

    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: ZwMapViewOfSection(sysmem) failed 0x%08X\n",
                 status));
        return status;
    }

    *OutBase = base;
    *OutVa = (PUCHAR)base + delta;
    return STATUS_SUCCESS;
}

/*
 * Handler for IOCTL_TTWIND_MAP_SYSMEM. Structure mirrors
 * TtWindCreateUserMapping (mapping.c) with a SECTION-kind record.
 */
NTSTATUS
TtWindIoctlMapSysmem(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    TTWIND_MAP_SYSMEM_IN *in;
    TTWIND_MAP_SYSMEM_OUT *out;
    PTTWIND_USER_MAPPING mapping;
    PEPROCESS process;
    BOOLEAN available;
    NTSTATUS status;

    *BytesWritten = 0;

    if (fileObject == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if ((in->Offset & (PAGE_SIZE - 1)) != 0 ||
        (in->Length & (PAGE_SIZE - 1)) != 0 || in->Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Availability check under ArcLock (the arm path writes outside the
     * ioctl queue). A reset cannot slip in afterwards: RESET_DEVICE
     * runs on this same sequential queue. The size/phys fields are
     * stable while the device is started.
     */
    WdfWaitLockAcquire(ctx->ArcLock, NULL);
    available = ctx->SysmemVerified;
    WdfWaitLockRelease(ctx->ArcLock);
    if (!available) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Overflow-safe bounds check: Offset + Length <= SysmemSize. */
    if (in->Length > ctx->SysmemSize ||
        in->Offset > ctx->SysmemSize - in->Length) {
        return STATUS_INVALID_PARAMETER;
    }

    process = IoGetRequestorProcess(WdfRequestWdmGetIrp(Request));
    if (process == NULL) {
        process = PsGetCurrentProcess();
    }

    mapping = (PTTWIND_USER_MAPPING)
        ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*mapping),
                        TTWIND_POOL_TAG);
    if (mapping == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ObReferenceObject(process);

    status = TtWindMapSysmemToUser(
        (UINT64)ctx->SysmemPhys.QuadPart + in->Offset,
        (SIZE_T)in->Length, process,
        &mapping->SectionBase, &mapping->UserVa);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(process);
        ExFreePoolWithTag(mapping, TTWIND_POOL_TAG);
        return status;
    }

    mapping->FileObject = fileObject;
    mapping->Process = process;
    mapping->Kind = TTWIND_MAPPING_KIND_SECTION;
    mapping->Mdl = NULL;
    mapping->Length = (SIZE_T)in->Length;
    mapping->TlbId = -1;

    WdfWaitLockAcquire(ctx->StateLock, NULL);
    InsertTailList(&ctx->MappingList, &mapping->ListEntry);
    WdfWaitLockRelease(ctx->StateLock);

    RtlZeroMemory(out, sizeof(*out));
    out->UserVa = (unsigned __int64)(ULONG_PTR)mapping->UserVa;
    out->Length = in->Length;
    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}
