# lszorro

Dynamic Zorro expansion board lister for Amiga UNIX (AMIX), in the style of `lspci`/`lsusb`.

Identifies all Kickstart-configured expansion boards from a built-in database of 461 manufacturer/product entries (sourced from the Linux kernel's `zorro.ids`). This includes cards whose Zorro address space is inaccessible from userspace on a running AMIX — such as RTG graphics cards and video digitizers.

This is a hobby project developed out of curiosity about forgotten Amiga Unix hardware. It builds on the earlier static scanner `amix_zp1_scan.c` from the [va2000-amix](https://github.com/asokero/va2000-amix) project.

---

## Usage

```sh
lszorro          # list found boards
lszorro -v       # verbose: decoded fields and raw hex dump
lszorro -a       # all 461 database entries, marked found or not found
```

Must be run as root (requires `/dev/mem` and `/dev/kmem` access).

---

## Building

Copy the source files to the AMIX machine and compile with the native C compiler:

```sh
cc lszorro.c -o lszorro
```

No installer, no dependencies beyond the standard C library.

---

## How it works

lszorro uses two detection methods, tried in order:

**1. `bootinfo.autocon[]` via `/dev/kmem`** (primary)

AMIX does not tear down the Kickstart-configured board list after booting. The kernel global `bootinfo.autocon[16]` (a `struct ConfigDev` array in `kernel/support.c`) permanently retains the manufacturer ID, product ID, board address, and size for every board Kickstart configured — regardless of whether AMIX has a driver for the board.

This is the most complete detection method. It finds boards that the `/dev/mem` scan cannot reach:

- **RTG cards returning all-0xFF** (e.g. Picasso II): framebuffer and register windows return bus float without driver initialisation
- **Video digitizers with null nibble response** (e.g. VLab): return `FF 00 FF 00...` which decodes to manufacturer 0x0000
- **RTG cards with standard AutoConfig** (e.g. VA2000): found here with the correct product ID

The `struct ConfigDev` layout on AMIX is 68 bytes and differs slightly from the standard AmigaOS definition (Node padded to 16 bytes, trimmed ExpansionRom). The `bootinfo.autocon[0]` address (`0x080DC3C8`) was determined empirically on this machine and is kernel-version dependent.

**2. AutoConfig ROM nibble decode** (fallback)

Standard Zorro II mechanism. Each logical byte N of the board's ID ROM is stored as two nibbles in consecutive 16-bit words at physical offsets N×4 and N×4+2, with most fields ones-complement inverted. A decoded manufacturer ID of zero is rejected as invalid data. This method catches any boards present but missing from `bootinfo` (unlikely in practice).

---

## Why some boards are still not detected

**Accelerator boards whose RAM is AMIX system memory** (e.g. PP&S Mercury): the board's fast RAM is used directly as AMIX kernel/user memory. The board may not appear in `bootinfo.autocon[]` if Kickstart treats it as memory rather than an expansion board.

**Boards requiring initialisation before AutoConfig exposure**: some designs do not expose the config ROM without prior setup that AMIX never performs.

---

## Tested on

- Amiga 3000, 68030, AMIX SVR4 2.1p2a
- Compiled with the native AT&T System V C compiler (`cc`)

Confirmed boards on this machine:

| Board | ID | Method |
|---|---|---|
| Commodore A2065 Ethernet | `0202:70` | kmem + AutoConfig |
| ACT Prelude Audio | `4231:01` | kmem + AutoConfig |
| Village Tronic Picasso II RAM | `0877:0B` | kmem only |
| Village Tronic Picasso II | `0877:0C` | kmem only |
| MacroSystems VLab | `4754:04` | kmem only |
| MNT VA2000 RTG | `6D6E:01` | kmem |
| Commodore A2088 ISA Bridge | `0201:01` | kmem |

---

## Files

```
lszorro.c        Main source
zorro_ids.h      Board database (auto-generated from Linux kernel zorro.ids)
probe_mem.c      Diagnostic: raw /dev/mem dump across Zorro II address ranges
test_execbase.c  Diagnostic: ExecBase -> expansion.library approach (fails on AMIX)
read_kmem.c      Standalone tool: dump bootinfo.autocon[] from /dev/kmem
PROGRESS.md      Research notes: bootinfo discovery, struct layout, kmem address
```

---

## Disclaimer

This is a hobby project. Use at your own risk.

- The `bootinfo` kernel address (`0x080DC3C8`) is specific to this machine and AMIX version. On a system compiled from different sources or with a different amount of fast RAM, the address will differ.
- Tested on one specific machine and AMIX version.
- Not affiliated with MNT Research, Commodore, Village Tronic, or any other organisation mentioned.

---

## License

MIT License. See LICENSE file.
