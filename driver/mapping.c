/* SPDX-License-Identifier: Apache-2.0 */
/*
 * mapping.c - user-mode mappings of device memory (BAR ranges and TLB
 * windows) for tt-wind.
 *
 * Mapping technique
 * -----------------
 * Device (I/O space) physical memory is handed to user mode via an MDL
 * built by hand over the physical range:
 *
 *   1. IoAllocateMdl(NULL-ish VA, length) sizes the MDL for the range.
 *      The physical address is passed as the pseudo virtual address so
 *      the framework computes the correct ByteOffset/page count (we only
 *      accept page-aligned ranges, so the offset is always zero).
 *   2. The PFN array is filled directly from the BAR physical address -
 *      MmBuildMdlForNonPagedPool is NOT used: it is only defined for
 *      pool memory and reads PFNs out of PTEs, which is unsupported for
 *      an MmMapIoSpace mapping of I/O space.
 *   3. MDL_PAGES_LOCKED is set by hand: the "pages" are device registers
 *      and can never be paged, and the flag tells Mm the MDL is ready to
 *      map. MmUnlockPages is never called on these MDLs (there is no PFN
 *      database entry to unlock); teardown is MmUnmapLockedPages +
 *      IoFreeMdl with the flag cleared again.
 *   4. MmMapLockedPagesSpecifyCache(UserMode, MmNonCached/MmWriteCombined,
 *      MdlMappingNoExecute) creates the user view. It raises on failure
 *      for UserMode, so it runs under __try/__except.
 *
 * This is the long-standing production pattern for exposing MMIO to user
 * mode (graphics and accelerator drivers); the alternative -
 * ZwMapViewOfSection on \Device\PhysicalMemory - relies on an even less
 * documented section object and makes cache-attribute conflicts easier
 * to create, so the MDL route was chosen.
 *
 * Process context
 * ---------------
 * A user mapping must be created and destroyed in the context of the
 * process that owns it. Ioctl handlers usually run in the caller's
 * context, but a sequential WDFQUEUE may present a queued request from
 * whatever thread completed the previous one, so the requestor process
 * is taken from the IRP (IoGetRequestorProcess) and KeStackAttachProcess
 * is used whenever the current process differs - both at map and at
 * unmap time. The process object is referenced for the life of the
 * mapping so it can be attached to at teardown.
 *
 * Tracking and teardown
 * ---------------------
 * Every mapping is recorded in the device context's MappingList together
 * with its owning WDFFILEOBJECT. EvtFileCleanup unmaps everything the
 * closing handle owns (and releases its TLB windows);
 * TtWindRevokeAllMappings (ReleaseHardware) force-unmaps whatever is
 * left so no user mapping of BAR space ever outlives the device's
 * resources. No mapping survives its handle.
 */

/*
 * ntifs.h (before ttwind.h's ntddk.h) for the process-context APIs:
 * KAPC_STATE, KeStackAttachProcess/KeUnstackDetachProcess,
 * IoGetRequestorProcess, PsGetProcessExitStatus.
 */
#include <ntifs.h>
#include "ttwind.h"

/*
 * TtWindMappingInitDevice - one-time init of mapping/TLB state, called
 * from EvtDeviceAdd after the device context exists (context memory is
 * zero-initialized by the framework).
 */
NTSTATUS
TtWindMappingInitDevice(
    _In_ WDFDEVICE Device
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    InitializeListHead(&ctx->MappingList);
    RtlInitializeBitMap(&ctx->TlbBitmap, ctx->TlbBitmapBits,
                        TTWIND_BH_TLB_2M_COUNT);
    RtlClearAllBits(&ctx->TlbBitmap);

    /*
     * The topmost 2 MiB window is the kernel's (ARC mailbox access,
     * arc.c); permanently allocated so no user handle can take it. Its
     * TlbOwner slot stays NULL, so ownership checks reject every user
     * operation on it.
     */
    RtlSetBit(&ctx->TlbBitmap, TTWIND_BH_KERNEL_TLB_INDEX);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;

    status = WdfWaitLockCreate(&attributes, &ctx->StateLock);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: WdfWaitLockCreate failed 0x%08X\n", status));
        return status;
    }

    status = WdfWaitLockCreate(&attributes, &ctx->ArcLock);
    if (!NT_SUCCESS(status)) {
        KdPrint(("ttwind: WdfWaitLockCreate(ArcLock) failed 0x%08X\n",
                 status));
    }
    return status;
}

/*
 * Map Length bytes of I/O space at Phys (both page aligned) into
 * Process with the given cache type. On success *OutMdl/*OutVa describe
 * the mapping. Runs at PASSIVE_LEVEL.
 */
static NTSTATUS
TtWindMapIoToUser(
    _In_ PHYSICAL_ADDRESS Phys,
    _In_ SIZE_T Length,
    _In_ MEMORY_CACHING_TYPE CacheType,
    _In_ PEPROCESS Process,
    _Out_ PMDL *OutMdl,
    _Out_ PVOID *OutVa
    )
{
    PMDL mdl;
    PPFN_NUMBER pfns;
    PFN_NUMBER basePfn;
    ULONG pageCount;
    ULONG i;
    PVOID userVa = NULL;
    KAPC_STATE apcState;
    BOOLEAN attached = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    *OutMdl = NULL;
    *OutVa = NULL;

    NT_ASSERT((Phys.QuadPart & (PAGE_SIZE - 1)) == 0);
    NT_ASSERT(Length != 0 && (Length & (PAGE_SIZE - 1)) == 0);
    NT_ASSERT(Length <= TTWIND_MAX_MAP_BYTES);

    /*
     * The physical address doubles as the pseudo VA so ByteOffset and
     * page count come out right; the VA itself is never dereferenced.
     */
    mdl = IoAllocateMdl((PVOID)(ULONG_PTR)Phys.QuadPart, (ULONG)Length,
                        FALSE, FALSE, NULL);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    basePfn = (PFN_NUMBER)(Phys.QuadPart >> PAGE_SHIFT);
    pageCount = (ULONG)(Length >> PAGE_SHIFT);
    pfns = MmGetMdlPfnArray(mdl);
    for (i = 0; i < pageCount; i++) {
        pfns[i] = basePfn + i;
    }
    mdl->MdlFlags |= MDL_PAGES_LOCKED;

    if (Process != PsGetCurrentProcess()) {
        KeStackAttachProcess((PRKPROCESS)Process, &apcState);
        attached = TRUE;
    }

    __try {
        userVa = MmMapLockedPagesSpecifyCache(mdl,
                                              UserMode,
                                              CacheType,
                                              NULL,  /* let Mm pick the VA */
                                              FALSE, /* no bugcheck        */
                                              NormalPagePriority |
                                              MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        userVa = NULL;
    }

    if (attached) {
        KeUnstackDetachProcess(&apcState);
    }

    if (userVa == NULL) {
        mdl->MdlFlags &= ~MDL_PAGES_LOCKED;
        IoFreeMdl(mdl);
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }

    *OutMdl = mdl;
    *OutVa = userVa;
    return STATUS_SUCCESS;
}

/*
 * Tear down one mapping record: unmap the user view (attaching to the
 * owning process if needed), free the MDL, drop the process reference,
 * free the record. The record must already be off MappingList.
 */
static VOID
TtWindDestroyMapping(
    _In_ PTTWIND_USER_MAPPING Mapping
    )
{
    KAPC_STATE apcState;

    if (Mapping->Process == PsGetCurrentProcess()) {
        /* Normal path: cleanup/unmap runs in the owning process. */
        MmUnmapLockedPages(Mapping->UserVa, Mapping->Mdl);
    } else if (PsGetProcessExitStatus(Mapping->Process) == STATUS_PENDING) {
        /*
         * Foreign but live process (duplicated handle, queued request):
         * attach and unmap there.
         */
        KeStackAttachProcess((PRKPROCESS)Mapping->Process, &apcState);
        MmUnmapLockedPages(Mapping->UserVa, Mapping->Mdl);
        KeUnstackDetachProcess(&apcState);
    } else {
        /*
         * The owning process has begun (or finished) termination while
         * some other handle holder kept this record alive. Its address
         * space - including our view - is being torn down by Mm;
         * MmUnmapLockedPages against a reclaimed VAD would be fatal, so
         * deliberately skip the unmap and only release our bookkeeping.
         */
        KdPrint(("ttwind: owner process of mapping %p exited; "
                 "skipping user unmap\n", Mapping->UserVa));
    }

    Mapping->Mdl->MdlFlags &= ~MDL_PAGES_LOCKED;
    IoFreeMdl(Mapping->Mdl);
    ObDereferenceObject(Mapping->Process);
    ExFreePoolWithTag(Mapping, TTWIND_POOL_TAG);
}

/*
 * TtWindCreateUserMapping - shared implementation of MAP_BAR and
 * MAP_TLB: map [Phys, Phys+Length) for the request's process, record it
 * against the request's file object, and return the user VA.
 *
 * Phys/Length are already validated (page aligned, in bounds). TlbId is
 * -1 for plain BAR mappings.
 */
NTSTATUS
TtWindCreateUserMapping(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ PHYSICAL_ADDRESS Phys,
    _In_ SIZE_T Length,
    _In_ UINT32 CacheMode,
    _In_ INT32 TlbId,
    _Out_ PVOID *OutUserVa
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    PEPROCESS process;
    PTTWIND_USER_MAPPING mapping;
    MEMORY_CACHING_TYPE cacheType;
    NTSTATUS status;

    *OutUserVa = NULL;

    if (fileObject == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    cacheType = (CacheMode == TTWIND_CACHE_WC) ? MmWriteCombined
                                               : MmNonCached;

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

    /* Keep the process object alive until the mapping is destroyed. */
    ObReferenceObject(process);

    status = TtWindMapIoToUser(Phys, Length, cacheType, process,
                               &mapping->Mdl, &mapping->UserVa);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(process);
        ExFreePoolWithTag(mapping, TTWIND_POOL_TAG);
        return status;
    }

    mapping->FileObject = fileObject;
    mapping->Process = process;
    mapping->Length = Length;
    mapping->TlbId = TlbId;

    /*
     * Insert under the lock, revalidating TLB ownership at insert time:
     * EvtFileCleanup (another thread closing this handle mid-ioctl) may
     * have released the window between the caller's ownership check and
     * here. Inserting after cleanup would leak the record, so back out
     * instead.
     */
    WdfWaitLockAcquire(ctx->StateLock, NULL);
    if (TlbId >= 0 && ctx->TlbOwner[TlbId] != fileObject) {
        WdfWaitLockRelease(ctx->StateLock);
        TtWindDestroyMapping(mapping);
        return STATUS_ACCESS_DENIED;
    }
    InsertTailList(&ctx->MappingList, &mapping->ListEntry);
    WdfWaitLockRelease(ctx->StateLock);

    *OutUserVa = mapping->UserVa;
    return STATUS_SUCCESS;
}

/*
 * TtWindTlbHasMappings - true if any live mapping of TlbId exists.
 * Caller holds StateLock.
 */
BOOLEAN
TtWindTlbHasMappings(
    _In_ PTTWIND_DEVICE_CONTEXT Ctx,
    _In_ INT32 TlbId
    )
{
    PLIST_ENTRY entry;

    for (entry = Ctx->MappingList.Flink; entry != &Ctx->MappingList;
         entry = entry->Flink) {
        PTTWIND_USER_MAPPING m =
            CONTAINING_RECORD(entry, TTWIND_USER_MAPPING, ListEntry);
        if (m->TlbId == TlbId) {
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Handler for IOCTL_TTWIND_MAP_BAR.
 */
NTSTATUS
TtWindIoctlMapBar(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ size_t *BytesWritten
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    TTWIND_MAP_BAR_IN *in;
    TTWIND_MAP_BAR_OUT *out;
    PHYSICAL_ADDRESS phys;
    UINT64 barSize;
    PVOID userVa;
    NTSTATUS status;

    *BytesWritten = 0;

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

    if (in->BarIndex >= ctx->BarCount || in->BarIndex >= TTWIND_MAX_BARS) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in->CacheMode != TTWIND_CACHE_UC && in->CacheMode != TTWIND_CACHE_WC) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((in->Offset & (PAGE_SIZE - 1)) != 0 ||
        (in->Length & (PAGE_SIZE - 1)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in->Length == 0 || in->Length > TTWIND_MAX_MAP_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Overflow-safe bounds check: Offset + Length <= barSize. */
    barSize = ctx->Bars[in->BarIndex].Size;
    if (in->Length > barSize || in->Offset > barSize - in->Length) {
        return STATUS_INVALID_PARAMETER;
    }

    phys.QuadPart = ctx->Bars[in->BarIndex].Phys.QuadPart +
                    (LONGLONG)in->Offset;

    status = TtWindCreateUserMapping(Device, Request, phys,
                                     (SIZE_T)in->Length, in->CacheMode,
                                     -1 /* not a TLB window */, &userVa);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(out, sizeof(*out));
    out->UserVa = (unsigned __int64)(ULONG_PTR)userVa;
    out->Length = in->Length;
    *BytesWritten = sizeof(*out);
    return STATUS_SUCCESS;
}

/*
 * Handler for IOCTL_TTWIND_UNMAP_BAR. Unmaps any mapping (BAR or TLB
 * window) this handle created at exactly UserVa.
 */
NTSTATUS
TtWindIoctlUnmapBar(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    TTWIND_UNMAP_BAR_IN *in;
    PTTWIND_USER_MAPPING found = NULL;
    PLIST_ENTRY entry;
    NTSTATUS status;

    if (fileObject == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfWaitLockAcquire(ctx->StateLock, NULL);
    for (entry = ctx->MappingList.Flink; entry != &ctx->MappingList;
         entry = entry->Flink) {
        PTTWIND_USER_MAPPING m =
            CONTAINING_RECORD(entry, TTWIND_USER_MAPPING, ListEntry);
        if (m->FileObject == fileObject &&
            (unsigned __int64)(ULONG_PTR)m->UserVa == in->UserVa) {
            RemoveEntryList(&m->ListEntry);
            found = m;
            break;
        }
    }
    WdfWaitLockRelease(ctx->StateLock);

    if (found == NULL) {
        return STATUS_NOT_FOUND;
    }

    TtWindDestroyMapping(found);
    return STATUS_SUCCESS;
}

/*
 * TtWindEvtFileCleanup - the last handle reference to a file object is
 * going away: unmap every mapping it owns and release every TLB window
 * it allocated. Runs at PASSIVE_LEVEL, usually in the context of the
 * closing process.
 */
VOID
TtWindEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    )
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(device);
    LIST_ENTRY toDestroy;
    PLIST_ENTRY entry;
    PLIST_ENTRY next;
    ULONG i;

    InitializeListHead(&toDestroy);

    WdfWaitLockAcquire(ctx->StateLock, NULL);

    for (entry = ctx->MappingList.Flink; entry != &ctx->MappingList;
         entry = next) {
        PTTWIND_USER_MAPPING m =
            CONTAINING_RECORD(entry, TTWIND_USER_MAPPING, ListEntry);
        next = entry->Flink;
        if (m->FileObject == FileObject) {
            RemoveEntryList(&m->ListEntry);
            InsertTailList(&toDestroy, &m->ListEntry);
        }
    }

    for (i = 0; i < TTWIND_BH_TLB_2M_COUNT; i++) {
        if (ctx->TlbOwner[i] == FileObject) {
            ctx->TlbOwner[i] = NULL;
            RtlClearBit(&ctx->TlbBitmap, i);
        }
    }

    WdfWaitLockRelease(ctx->StateLock);

    while (!IsListEmpty(&toDestroy)) {
        PTTWIND_USER_MAPPING m = CONTAINING_RECORD(
            RemoveHeadList(&toDestroy), TTWIND_USER_MAPPING, ListEntry);
        TtWindDestroyMapping(m);
    }
}

/*
 * TtWindRevokeAllMappings - force-unmap every live user mapping and
 * clear the TLB allocator. Called from ReleaseHardware so no user
 * mapping of BAR space outlives the device's resources (surprise
 * removal included). Later file cleanups then find nothing to do.
 */
VOID
TtWindRevokeAllMappings(
    _In_ WDFDEVICE Device
    )
{
    PTTWIND_DEVICE_CONTEXT ctx = TtWindGetDeviceContext(Device);
    LIST_ENTRY toDestroy;
    ULONG i;

    InitializeListHead(&toDestroy);

    WdfWaitLockAcquire(ctx->StateLock, NULL);
    if (!IsListEmpty(&ctx->MappingList)) {
        /* Move the whole list in one splice. */
        toDestroy = ctx->MappingList;
        toDestroy.Flink->Blink = &toDestroy;
        toDestroy.Blink->Flink = &toDestroy;
        InitializeListHead(&ctx->MappingList);
    }
    for (i = 0; i < TTWIND_BH_TLB_2M_COUNT; i++) {
        ctx->TlbOwner[i] = NULL;
    }
    RtlClearAllBits(&ctx->TlbBitmap);
    /* The kernel window reservation survives every revocation. */
    RtlSetBit(&ctx->TlbBitmap, TTWIND_BH_KERNEL_TLB_INDEX);
    WdfWaitLockRelease(ctx->StateLock);

    while (!IsListEmpty(&toDestroy)) {
        PTTWIND_USER_MAPPING m = CONTAINING_RECORD(
            RemoveHeadList(&toDestroy), TTWIND_USER_MAPPING, ListEntry);
        KdPrint(("ttwind: revoking leftover user mapping %p (%Iu bytes)\n",
                 m->UserVa, m->Length));
        TtWindDestroyMapping(m);
    }
}
