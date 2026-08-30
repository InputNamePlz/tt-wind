# tt-wind

Windows kernel-mode driver for Tenstorrent PCIe devices — the Windows counterpart of
[tt-kmd](https://github.com/tenstorrent/tt-kmd).

## Position in the stack

```
tt-metal
   │
tt-umd  ── user-mode driver (windows branch: https://github.com/InputNamePlz/tt-umd)
   │        └─ tt-kmd-lib: C API (tt_kmd_lib.h) — the contract this driver serves
   │
tt-wind ── this repo (Windows KMD, WDF)      [on Linux: tt-kmd]
   │
Tenstorrent hardware (Wormhole / Blackhole)
```

The driver's user/kernel IOCTL interface is private to this repo and tt-umd's Windows
backend of `tt-kmd-lib`; the stable contract between the two projects is the
`tt_kmd_lib.h` function API, not the wire format.

## Scope (first milestones)

1. ~~Device enumeration + identity~~ ✅
2. BAR mapping into user space (UC + WC)
3. TLB window allocation / configuration / mmap
4. Reset with user-mapping revocation
5. DMA buffer allocation + user page pinning (phase 2)

Explicitly out of scope for now: dma-buf-style P2P export, RDMA, Grayskull (deprecated
upstream).

## Building

Requires Visual Studio 2022 Build Tools + Windows Driver Kit (WDK). See
[docs/building.md](docs/building.md) for the exact commands (MSBuild on
`ttwind.sln`, x64).

## License

Apache-2.0 (matching tt-umd; a Windows driver has no GPL obligation).
