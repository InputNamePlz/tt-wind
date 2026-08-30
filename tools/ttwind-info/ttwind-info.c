/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ttwind-info.c - user-mode test CLI for the tt-wind driver.
 *
 * Enumerates device interfaces of GUID_DEVINTERFACE_TTWIND, opens each
 * one, issues IOCTL_TTWIND_GET_DEVICE_INFO, and pretty-prints the
 * result. Exits 0 if at least one device was queried successfully,
 * 1 if no devices are present, 2 on error.
 */

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../driver/ttwind_ioctl.h"

/* Print "<prefix>: <err> (<message>)" using FormatMessage. */
static void print_win32_error(const char *prefix, DWORD err)
{
    char msg[512];
    DWORD n;

    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       msg, sizeof(msg), NULL);
    if (n != 0) {
        /* Strip trailing CR/LF that FormatMessage appends. */
        while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n'))
            msg[--n] = '\0';
        fprintf(stderr, "%s: error %lu (%s)\n", prefix, err, msg);
    } else {
        fprintf(stderr, "%s: error %lu\n", prefix, err);
    }
}

/* Format a byte count into "16.0 MiB (0x1000000)" style. buf must be
 * at least 64 chars. */
static const char *format_size(unsigned __int64 bytes, char *buf, size_t buflen)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    size_t u = 0;

    while (v >= 1024.0 && u + 1 < ARRAYSIZE(units)) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, buflen, "%llu %s", bytes, units[u]);
    else
        snprintf(buf, buflen, "%.1f %s (0x%llx)", v, units[u], bytes);
    return buf;
}

/* Open one interface path and print its device info. Returns 0 on
 * success, nonzero on failure. */
static int query_device(const wchar_t *path, unsigned index)
{
    TTWIND_DEVICE_INFO_OUT info;
    HANDLE h;
    DWORD returned = 0;
    char sizebuf[64];
    unsigned i;

    printf("Device %u: %ls\n", index, path);

    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        print_win32_error("  CreateFile failed", GetLastError());
        return 1;
    }

    if (!DeviceIoControl(h, IOCTL_TTWIND_GET_DEVICE_INFO, NULL, 0,
                         &info, sizeof(info), &returned, NULL)) {
        print_win32_error("  DeviceIoControl(IOCTL_TTWIND_GET_DEVICE_INFO) failed",
                          GetLastError());
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);

    if (returned < sizeof(info)) {
        fprintf(stderr, "  short IOCTL reply: %lu bytes, expected %zu\n",
                returned, sizeof(info));
        return 1;
    }

    printf("  Vendor ID     : 0x%04x\n", info.VendorId);
    printf("  Device ID     : 0x%04x\n", info.DeviceId);
    printf("  Subsystem     : 0x%04x:0x%04x\n",
           info.SubsystemVendorId, info.SubsystemId);
    printf("  PCI location  : %04x:%02x:%02x.%x\n",
           info.PciDomain, info.Bus, info.Device, info.Function);
    printf("  BAR count     : %u\n", info.BarCount);
    for (i = 0; i < TTWIND_MAX_BARS; i++) {
        if (info.Bars[i].Phys == 0 && info.Bars[i].Size == 0)
            continue;
        printf("  BAR%u          : phys 0x%016llx  size %s\n", i,
               info.Bars[i].Phys,
               format_size(info.Bars[i].Size, sizebuf, sizeof(sizebuf)));
    }
    return 0;
}

int main(void)
{
    wchar_t *list = NULL;
    wchar_t *p;
    ULONG chars = 0;
    CONFIGRET cr;
    unsigned found = 0, failed = 0;

    /* The list size can change between the two calls if devices arrive;
     * retry in that case. */
    for (;;) {
        cr = CM_Get_Device_Interface_List_SizeW(
                &chars, (LPGUID)&GUID_DEVINTERFACE_TTWIND, NULL,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS) {
            fprintf(stderr, "CM_Get_Device_Interface_List_Size failed: CR 0x%x\n", cr);
            return 2;
        }

        free(list);
        list = calloc(chars, sizeof(wchar_t));
        if (list == NULL) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }

        cr = CM_Get_Device_Interface_ListW(
                (LPGUID)&GUID_DEVINTERFACE_TTWIND, NULL, list, chars,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS)
            break;
        if (cr != CR_BUFFER_SMALL) {
            fprintf(stderr, "CM_Get_Device_Interface_List failed: CR 0x%x\n", cr);
            free(list);
            return 2;
        }
    }

    /* The list is a sequence of NUL-terminated strings ended by an
     * empty string. */
    for (p = list; *p != L'\0'; p += wcslen(p) + 1) {
        if (query_device(p, found) != 0)
            failed++;
        found++;
    }
    free(list);

    if (found == 0) {
        printf("No tt-wind devices found.\n");
        return 1;
    }
    return failed != 0 ? 2 : 0;
}
