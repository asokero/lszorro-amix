# lszorro

WIP - Dynamic Zorro expansion board lister for Amiga UNIX (AMIX), in the style of `lspci`/`lsusb`.

Scans the Zorro II address space via `/dev/mem` and identifies expansion boards from a built-in database of 461 manufacturer/product entries (sourced from the Linux kernel's `zorro.ids`).

This is a hobby project developed out of curiosity about forgotten Amiga Unix hardware. It builds on the earlier static scanner `amix_zp1_scan.c` from the [va2000-amix](https://github.com/asokero/va2000-amix) project.

---

## Usage

```sh
lszorro          # list found boards
lszorro -v       # verbose: decoded fields and raw hex dump
lszorro -a       # all 461 database entries, marked found or not found
```

Must be run as root (requires `/dev/mem` access).

---

## Building

Copy the source files to the AMIX machine and compile with the native C compiler:

```sh
cc lszorro.c -o lszorro
```

No installer, no dependencies beyond the standard C library. Transfer via NFS, floppy, or whatever is available.

---

## How it works

lszorro scans two address ranges in the Zorro II bus space:

- **I/O slots:** `0xE90000–0xEFFFFF` (8 × 64 KB)
- **Memory area:** `0x200000–0x9FFFFF` (64 KB steps)

At each address it attempts to `mmap` 128 bytes of `/dev/mem` and applies two detection methods in order:

**1. AutoConfig nibble decode** — The standard Zorro II mechanism. Each logical byte of the board's ID ROM is stored as two nibbles in consecutive 16-bit words, with most fields ones-complement inverted. Standard I/O cards (ethernet, audio, SCSI, serial) that keep their AutoConfig ROM accessible at their base address are detected this way.

**2. VA2000 firmware fingerprint** — The MNT VA2000 RTG card does not expose its AutoConfig ROM at offset 0; instead its register file is mapped there directly. The 16-bit `fw_version` field at offset 0 is used as the identification signal.

---

## Why some boards are not detected

AI Slop Analysis is that: Detection depends on being able to `mmap` the board's address and find either AutoConfig nibble data or a known register signature at offset 0.

**Boards with no driver and framebuffer at offset 0** (e.g. Piccolo / Ingenieurbüro Helfrich): the framebuffer VRAM is readable but contains pixel data, not AutoConfig nibbles. The register window may generate bus errors when accessed without prior initialisation, causing `mmap` to fail. No reliable fingerprint exists for uninitialized VRAM.

**Accelerator boards whose RAM is used as AMIX system memory** (e.g. PP&S Mercury): AMIX maps the board's fast RAM as kernel pages. Those physical addresses are inaccessible via `/dev/mem` from userspace.

**Boards outside the scanned range**: Zorro III is not supported by AMIX and is not scanned.

In general: I/O cards are detected reliably. Memory expansion cards and accelerators usually are not.

---

## Tested on

- Amiga 3000, 68030, AMIX SVR4 2.1p2a
- Compiled with the native AT&T System V C compiler (`cc`)

Boards confirmed working:

| Board | ID | Method |
|---|---|---|
| Commodore A2065 Ethernet | `0202:70` | AutoConfig |
| ACT Prelude Audio | `4231:01` | AutoConfig |
| MNT VA2000 RTG (fw 0x5A) | `6D6E:00` | Fingerprint |

---

## Files

```
lszorro.c        Main source
zorro_ids.h      Board database (auto-generated from Linux kernel zorro.ids)
```

---

## Disclaimer

This is a hobby project. Use at your own risk.

- Tested on one specific machine and AMIX version. Other configurations may behave differently.
- Not affiliated with MNT Research, Commodore, Ingenieurbüro Helfrich, or any other organisation mentioned.

---

## License

MIT License. See LICENSE file.

-Antti Sokero 2026
