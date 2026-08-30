/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tlb.c - Blackhole TLB window allocation, configuration, and mapping.
 *
 * Blackhole exposes 202 2 MiB TLB windows at the bottom of BAR0 (window
 * i covers BAR0 offset i * 2 MiB). Each window is steered by a 12-byte
 * configuration register block at BAR0 + 0x1FC00000 (12 bytes per
 * window, 2 MiB windows first, then the 4 GiB windows); the layout is
 * taken from tt-kmd's blackhole.c (struct TLB_2M_REG).
 *
 * 2 MiB window register, 96 bits, LSB first:
 *   address      [ 0..42]  NOC address >> 21
 *   x_end        [43..48]
 *   y_end        [49..54]
 *   x_start      [55..60]
 *   y_start      [61..66]
 *   noc          [67..68]
 *   multicast    [69]
 *   ordering     [70..71]
 *   linked       [72]
 *   use_static_vc[73]
 *   stream_header[74]      (not exposed; written as 0)
 *   static_vc    [75..77]  (value field, not exposed; written as 0)
 *
 * The first 32 windows additionally have a 4-byte strided-multicast
 * register (at TLB regs + 2520 + 4*i); like tt-kmd, CONFIGURE_TLB
 * clears it so a stale strided setup can never alias a fresh config.
 *
 * These TLB configuration registers are the ONLY device registers the
 * kernel writes.
 *
 * Ownership: a window belongs to the file object (handle) that
 * allocated it; only that handle may configure, map, or free it, and
 * cleanup of the handle releases it (mapping.c). Allocator state lives
 * in the device context under StateLock.
 */

#include "ttwind.h"

/*
 * OR `Value` (masked to Width bits) into a 96-bit register image held
 * as Lo (bits 0..63) and Hi (bits 64..95). Pos may be anywhere in
 * 0..95; fields may straddle the 64-bit boundary.
 *
 * Pos >= 64 must be handled explicitly: `masked << Pos` with Pos >= 64
 * is undefined behavior, and x64 executes it as a shift by (Pos % 64),
 * which would OR the value into the ADDRESS bits of Lo instead of Hi.
 * That exact bug shipped in 100.3.2.0: every field above bit 63 (noc,
 * mcast, ordering, linked, use_static_vc) leaked into address bits
 * Pos-64, so e.g. ordering=1 (bit 70) flipped NOC-address bit 27 and
 * pointed the window at addr+0x0800_0000 while the real ordering bits
 * stayed 0.
 */
static VOID
TtWindPutField(
    _Inout_ UINT64 *Lo,
    _Inout_ UINT32 *Hi,
    _In_ UINT32 Pos,
    _In_ UINT32 Width,
    _In_ UINT64 Value
    )
{
    UINT64 masked = Value & ((Width >= 64) ? ~0ull : ((1ull << Width) - 1));

    NT_ASSERT(Pos < 96 && Pos + Width <= 96);

    if (Pos < 64) {
        *Lo |= masked << Pos;
        if (Pos + Width > 64) {
            *Hi |= (UINT32)(masked >> (64 - Pos));
        }
    } else {
        *Hi |= (UINT32)(masked << (Pos - 64));
    }
}

/*
 * Pack Cfg into the 96-bit TLB_2M_REG image (Lo = bits 0..63, Hi =
 * bits 64..95) per tt-kmd's struct TLB_2M_REG; see the file header.
 */
static VOID
TtWindPackTlb2M(
    _In_ const TTWIND_NOC_TLB_CONFIG *Cfg,
    _Out_ UINT64 *Lo,
    _Out_ UINT32 *Hi
    )
{
    UINT64 lo = 0;
    UINT32 hi = 0;

    TtWindPutField(&lo, &hi,  0, 43, Cfg->Addr >> TTWIND_BH_TLB_2M_SHIFT);
    TtWindPutField(&lo, &hi, 43,  6, Cfg->XEnd);
    TtWindPutField(&lo, &hi, 49,  6, Cfg->YEnd);
    TtWindPutField(&lo, &hi, 55,  6, Cfg->XStart);
    TtWindPutField(&lo, &hi, 61,  6, Cfg->YStart);
    TtWindPutField(&lo, &hi, 67,  2, Cfg->Noc);
    TtWindPutField(&lo, &hi, 69,  1, Cfg->Mcast);
    TtWindPutField(&lo, &hi, 70,  2, Cfg->Ordering);
    TtWindPutField(&lo, &hi, 72,  1, Cfg->Linked);
    TtWindPutField(&lo, &hi, 73,  1, Cfg->StaticVc); /* use_static_vc */
    /* stream_header and the static_vc value field stay 0. */

    *Lo = lo;
    *Hi = hi;
}

/*
 * TtWindProgramTlb2M - pack Cfg per tt-kmd's struct TLB_2M_REG (see the
 * file header) and write window TlbId's configuration registers.
 *
 * No validation here: the caller has validated Cfg (ioctl path) or
 * built it from constants (kernel ARC path, arc.c). The caller also
 * provides serialization for the window it programs: user windows
 * (0..200) are written under StateLock, the kernel window (201) under
 * ArcLock; the register blocks are disjoint, so the two lock domains
 * never touch the same registers. Ctx->TlbRegs must be mapped.
 */
VOID
TtWindProgramTlb2M(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ UINT32 TlbId,
    _In_ const TTWIND_NOC_TLB_CONFIG *Cfg
    )
{
    UINT64 lo;
    UINT32 hi;
    PUCHAR regs;

    NT_ASSERT(TlbId < TTWIND_BH_TLB_2M_COUNT);
    NT_ASSERT(Ctx->TlbRegs != NULL);

    TtWindPackTlb2M(Cfg, &lo, &hi);

    regs = Ctx->TlbRegs + (TlbId * TTWIND_BH_TLB_REG_SIZE);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0), (ULONG)lo);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 4), (ULONG)(lo >> 32));
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 8), hi);

    /*
     * Strided multicast is not exposed through this API; clear any
     * configuration set by other means (mirrors tt-kmd).
     */
    if (TlbId < TTWIND_BH_TLB_STRIDED_COUNT) {
        PUCHAR strided = Ctx->TlbRegs +
            TTWIND_BH_TLB_STRIDED_REGS_OFFSET +
            (TlbId * TTWIND_BH_TLB_STRIDED_REG_SIZE);
        WRITE_REGISTER_ULONG((volatile ULONG *)strided, 0);
    }
}

/*
 * TtWindKernelTlbSanityCheck - one bounded known-good MMIO round trip.
 *
 * Programs the reserved kernel TLB window's configuration registers (a
 * BAR0 register block, no NOC traffic involved) with a distinctive
 * pattern (ARC tile (8,0), strict ordering - the exact config arc.c
 * uses) and reads the three dwords back. A healthy device echoes the
 * written values; a device whose MMIO decode is wedged returns all-1s
 * (completion timeout, when the link is up); reads never target a NOC
 * tile, so a hung NOC cannot stall this check.
 *
 * Used by POST_RESET (reset.c) as the FIRST MMIO touch after the
 * vendor ID reappears on the bus, gating all further MMIO (kernel TLB
 * reprogramming, ARC power-up) on a usable link. Safe there: the
 * device just answered config cycles, so the link is up and the read
 * completes - at worst slowly with all-1s.
 *
 * Caller holds ArcLock (kernel-window register domain) and has checked
 * Ctx->TlbRegs / Ctx->KernelTlb are mapped. The check leaves the kernel
 * window programmed to ARC address 0; arc.c reprograms it on every
 * access, so no state is disturbed.
 *
 * Returns STATUS_SUCCESS or STATUS_IO_DEVICE_ERROR (readback dead or
 * mismatched).
 */
NTSTATUS
TtWindKernelTlbSanityCheck(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx
    )
{
    TTWIND_NOC_TLB_CONFIG cfg;
    UINT64 lo;
    UINT32 hi;
    ULONG rb0, rb1, rb2;
    PUCHAR regs;

    NT_ASSERT(Ctx->TlbRegs != NULL);

    RtlZeroMemory(&cfg, sizeof(cfg));
    cfg.Addr = 0;
    cfg.XEnd = 8;      /* ARC tile, matching arc.c's kernel window use */
    cfg.YEnd = 0;
    cfg.Noc = 0;
    cfg.Ordering = 1;  /* strict - guarantees a nonzero readback image */

    TtWindPackTlb2M(&cfg, &lo, &hi);
    TtWindProgramTlb2M(Ctx, TTWIND_BH_KERNEL_TLB_INDEX, &cfg);

    regs = Ctx->TlbRegs +
           (TTWIND_BH_KERNEL_TLB_INDEX * TTWIND_BH_TLB_REG_SIZE);
    rb0 = READ_REGISTER_ULONG((volatile ULONG *)(regs + 0));
    rb1 = READ_REGISTER_ULONG((volatile ULONG *)(regs + 4));
    rb2 = READ_REGISTER_ULONG((volatile ULONG *)(regs + 8));

    if (rb0 != (ULONG)lo || rb1 != (ULONG)(lo >> 32) || rb2 != hi) {
        KdPrint(("ttwind: TLB register readback mismatch "
                 "(%08X %08X %08X != %08X %08X %08X)\n",
                 rb0, rb1, rb2,
                 (ULONG)lo, (ULONG)(lo >> 32), hi));
        return STATUS_IO_DEVICE_ERROR;
    }
    return STATUS_SUCCESS;
}

/*
 * Handler for IOCTL_TTWIND_ALLOCATE_TLB.
 */
NTSTATUS
TtWindIoctlAllocateTlb(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    TTWIND_ALLOCATE_TLB_IN *in;
    TTWIND_ALLOCATE_TLB_OUT *out;
    ULONG id;
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

    /* Only the 2 MiB kind exists for now. */
    if (in->Size != TTWIND_TLB_WINDOW_SIZE_2M) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(ctx->StateLock, NULL);
    id = RtlFindClearBitsAndSet(&ctx->TlbBitmap, 1, 0);
    if (id != 0xFFFFFFFF) {
        NT_ASSERT(ctx->TlbOwner[id] == NULL);
        ctx->TlbOwner[id] = fileObject;
    }
    WdfWaitLockRelease(ctx->StateLock);

    if (id == 0xFFFFFFFF) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(out, sizeof(*out));
    out->TlbId = id;
    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}

/*
 * Validate TlbId and ownership. Caller holds StateLock.
 */
static NTSTATUS
TtWindCheckTlbOwner(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT FileObject,
    _In_ UINT32 TlbId
    )
{
    if (TlbId >= TTWIND_BH_TLB_2M_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->TlbOwner[TlbId] != FileObject) {
        /* Not allocated, or allocated by another handle. */
        return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}

/*
 * Handler for IOCTL_TTWIND_FREE_TLB.
 */
NTSTATUS
TtWindIoctlFreeTlb(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    TTWIND_FREE_TLB_IN *in;
    NTSTATUS status;

    if (fileObject == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in->Reserved != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(ctx->StateLock, NULL);
    status = TtWindCheckTlbOwner(ctx, fileObject, in->TlbId);
    if (NT_SUCCESS(status)) {
        if (TtWindTlbHasMappings(ctx, (INT32)in->TlbId)) {
            /* Refuse to free a window that is still user-mapped. */
            status = STATUS_INVALID_DEVICE_STATE;
        } else {
            ctx->TlbOwner[in->TlbId] = NULL;
            RtlClearBit(&ctx->TlbBitmap, in->TlbId);
        }
    }
    WdfWaitLockRelease(ctx->StateLock);

    return status;
}

/*
 * Handler for IOCTL_TTWIND_CONFIGURE_TLB - write the window's config
 * registers through the kernel's own UC mapping of BAR0.
 */
NTSTATUS
TtWindIoctlConfigureTlb(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    TTWIND_CONFIGURE_TLB_IN *in;
    const TTWIND_NOC_TLB_CONFIG *cfg;
    NTSTATUS status;

    if (fileObject == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    cfg = &in->Config;

    if (in->Reserved != 0 ||
        cfg->Reserved0[0] != 0 || cfg->Reserved0[1] != 0 ||
        cfg->Reserved0[2] != 0 ||
        cfg->Reserved1[0] != 0 || cfg->Reserved1[1] != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* A 2 MiB window can only start on a 2 MiB boundary. */
    if ((cfg->Addr & (TTWIND_BH_TLB_2M_SIZE - 1)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Range-check every field so nothing truncates silently. */
    if (cfg->XEnd > 0x3F || cfg->YEnd > 0x3F ||
        cfg->XStart > 0x3F || cfg->YStart > 0x3F ||
        cfg->Noc > 1 || cfg->Mcast > 1 || cfg->Ordering > 3 ||
        cfg->Linked > 1 || cfg->StaticVc > 1) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(ctx->StateLock, NULL);

    status = TtWindCheckTlbOwner(ctx, fileObject, in->TlbId);
    if (NT_SUCCESS(status) && ctx->TlbRegs == NULL) {
        status = STATUS_DEVICE_NOT_READY;
    }

    if (NT_SUCCESS(status)) {
        TtWindProgramTlb2M(ctx, in->TlbId, cfg);
    }

    WdfWaitLockRelease(ctx->StateLock);
    return status;
}

/*
 * Handler for IOCTL_TTWIND_MAP_TLB - map the window's 2 MiB slice of
 * BAR0 into the caller.
 */
NTSTATUS
TtWindIoctlMapTlb(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    TTWIND_MAP_TLB_IN *in;
    TTWIND_MAP_TLB_OUT *out;
    PHYSICAL_ADDRESS phys;
    UINT64 offset;
    PVOID userVa;
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

    if (in->CacheMode != TTWIND_CACHE_UC && in->CacheMode != TTWIND_CACHE_WC) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Ownership check under the lock; the mapping itself happens after
     * release. A concurrent FREE_TLB from this same handle could slip
     * in between, but a handle racing itself only loses its own window;
     * cross-handle theft is impossible because only the owner can free.
     */
    WdfWaitLockAcquire(ctx->StateLock, NULL);
    status = TtWindCheckTlbOwner(ctx, fileObject, in->TlbId);
    WdfWaitLockRelease(ctx->StateLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* The windows sit at the bottom of BAR0; bounds-check against it. */
    offset = (UINT64)in->TlbId * TTWIND_BH_TLB_2M_SIZE;
    if (ctx->BarCount == 0 ||
        offset + TTWIND_BH_TLB_2M_SIZE > ctx->Bars[0].Size) {
        return STATUS_DEVICE_NOT_READY;
    }

    phys.QuadPart = ctx->Bars[0].Phys.QuadPart + (LONGLONG)offset;

    status = TtWindCreateUserMapping(Device, Request, phys,
                                     (SIZE_T)TTWIND_BH_TLB_2M_SIZE,
                                     in->CacheMode, (INT32)in->TlbId,
                                     &userVa);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(out, sizeof(*out));
    out->UserVa = (unsigned __int64)(ULONG_PTR)userVa;
    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}
