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

#ifdef __cplusplus
static_assert(sizeof(TTWIND_DEVICE_INFO_OUT) == 120,
              "TTWIND_DEVICE_INFO_OUT wire size changed");
#else
C_ASSERT(sizeof(TTWIND_DEVICE_INFO_OUT) == 120);
#endif
