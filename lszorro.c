/*
 * lszorro.c - List Zorro expansion boards on AMIX SVR4
 *
 * Scans Zorro II address space and kernel memory to identify expansion
 * boards from a built-in database of 461 manufacturer/product entries.
 *
 * Usage: lszorro [-v] [-a]
 *   -v  verbose: decoded fields + raw hex dump
 *   -a  list all database entries (mark found / not found)
 *
 * Detection methods (tried in order):
 *
 *   1. bootinfo.autocon[] via /dev/kmem
 *      AMIX preserves all Kickstart-configured boards in the kernel global
 *      'bootinfo.autocon[16]' (struct ConfigDev array). Reading this via
 *      /dev/kmem gives the complete board list including cards whose Zorro
 *      address space is inaccessible from userspace (Picasso II, VLab).
 *      Struct layout and address (0x080DC3C8) were verified empirically on
 *      this machine; the address is kernel-version dependent.
 *
 *   2. AutoConfig ROM nibble decode (fallback)
 *      Standard Zorro II mechanism. Each logical byte N is stored as two
 *      16-bit words at physical offsets N*4 and N*4+2, with only the upper
 *      nibble (D15-D12) carrying data. All fields except er_Type (byte 0)
 *      are ones-complement inverted. A decoded manufacturer of 0x0000 is
 *      rejected (e.g. VLab returns FF 00 FF 00... which inverts to zero).
 *
 * Compile: cc lszorro.c -o lszorro
 * Must run as root (/dev/mem and /dev/kmem access).
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>

#include "zorro_ids.h"

/* Zorro II I/O slots: 0xE90000 - 0xEFFFFF, 8 x 64KB */
#define ZII_IO_BASE   0x00E90000UL
#define ZII_IO_END    0x00EFFFFFUL
#define ZII_IO_STEP   0x00010000UL

/* Zorro II memory area: 0x200000 - 0x9FFFFF, 64KB steps */
#define ZII_MEM_BASE  0x00200000UL
#define ZII_MEM_END   0x009FFFFFUL
#define ZII_MEM_STEP  0x00010000UL

/* Physical bytes needed to cover all AutoConfig fields */
#define AC_MAP_SIZE   0x80

/* AutoConfig logical byte positions */
#define AC_TYPE    0
#define AC_PRODUCT 1
#define AC_FLAGS   2
#define AC_RESV    3
#define AC_MANUF   4   /* UWORD */
#define AC_SERIAL  6   /* ULONG */
#define AC_DIAGVEC 10  /* UWORD */

/* er_Type bit masks */
#define ERT_TYPEMASK  0xC0
#define ERT_ZORROII   0xC0
#define ERT_MEMLIST   0x20
#define ERT_DIAGVALID 0x10
#define ERT_CHAINED   0x08
#define ERT_SIZEMASK  0x07

/*
 * AMIX kernel ConfigDev struct field offsets (68 bytes total).
 * Empirically verified from /dev/kmem on A3000, AMIX SVR4 2.1p2a.
 * Differs from standard AmigaOS: Node padded to 16 bytes, reduced
 * ExpansionRom (no er_Reserved0e), cd_Unused[3] instead of [4].
 */
#define KM_ER_TYPE     0x14   /* UBYTE */
#define KM_ER_PRODUCT  0x15   /* UBYTE */
#define KM_ER_FLAGS    0x16   /* UBYTE */
#define KM_ER_MANUF    0x18   /* UWORD big-endian */
#define KM_ER_SERIAL   0x1A   /* ULONG big-endian */
#define KM_ER_DIAGVEC  0x1E   /* UWORD big-endian */
#define KM_BOARDADDR   0x24   /* ULONG big-endian */
#define KM_BOARDSIZE   0x28   /* ULONG big-endian */
#define KM_STRUCT_SIZE 68

/*
 * Virtual address of bootinfo.autocon[0] in running kernel.
 * Determined from: bindaddr (0x08000000) + .text size + .data offset
 * of 'bootinfo' symbol (nm /unix: value=18716, shndx=2).
 * Verified by finding A2065 board address (0x00E90000) at kmem+0x24
 * relative to this base.
 */
#define BOOTINFO_VADDR  0x080DC3C8L
#define NAUTO           16

#define MAX_BOARDS 32
#define RAW_SAVE   64

#define DET_AUTOCONFIG  0
#define DET_KMEM        1

struct board {
    unsigned long  base;
    unsigned long  size;
    unsigned short manuf;
    unsigned char  prod;
    unsigned char  det;
    unsigned char  er_type;
    unsigned char  er_flags;
    unsigned long  serial;
    unsigned short diagvec;
    unsigned char  raw[RAW_SAVE];
};

static struct board found[MAX_BOARDS];
static int nfound = 0;

static int opt_verbose = 0;
static int opt_all     = 0;

static unsigned long size_table[8] = {
    8UL * 1024UL * 1024UL,
    64UL * 1024UL,
    128UL * 1024UL,
    256UL * 1024UL,
    512UL * 1024UL,
    1UL * 1024UL * 1024UL,
    2UL * 1024UL * 1024UL,
    4UL * 1024UL * 1024UL
};

/* ------------------------------------------------------------------ */
/* AutoConfig nibble decoder                                           */
/* ------------------------------------------------------------------ */

static unsigned char
ac_byte(mem, n)
    unsigned char *mem;
    int n;
{
    unsigned char hi, lo, raw;
    hi  = (mem[n * 4    ] >> 4) & 0x0F;
    lo  = (mem[n * 4 + 2] >> 4) & 0x0F;
    raw = (unsigned char)((hi << 4) | lo);
    return (n == 0) ? raw : (unsigned char)(~raw & 0xFF);
}

static unsigned short
ac_word(mem, n)
    unsigned char *mem;
    int n;
{
    return (unsigned short)(
        ((unsigned short)ac_byte(mem, n) << 8) | ac_byte(mem, n + 1));
}

static unsigned long
ac_long(mem, n)
    unsigned char *mem;
    int n;
{
    return ((unsigned long)ac_word(mem, n) << 16) | ac_word(mem, n + 2);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int
is_autoconfig(mem)
    unsigned char *mem;
{
    unsigned char er_type;
    int i;

    for (i = 0; i < 16; i++)
        if (mem[i] != 0xFF) break;
    if (i == 16) return 0;
    for (i = 0; i < 16; i++)
        if (mem[i] != 0x00) break;
    if (i == 16) return 0;

    er_type = ac_byte(mem, AC_TYPE);
    if ((er_type & ERT_TYPEMASK) != ERT_ZORROII)
        return 0;
    if (ac_byte(mem, AC_RESV) != 0x00)
        return 0;

    return 1;
}

static const char *
size_str(sz)
    unsigned long sz;
{
    static char buf[16];
    if (sz >= 1024UL * 1024UL)
        sprintf(buf, "%luMB", sz / (1024UL * 1024UL));
    else
        sprintf(buf, "%luKB", sz / 1024UL);
    return buf;
}

static unsigned long
km_ulong(buf, off)
    unsigned char *buf;
    int off;
{
    return ((unsigned long)buf[off]   << 24) |
           ((unsigned long)buf[off+1] << 16) |
           ((unsigned long)buf[off+2] <<  8) |
            (unsigned long)buf[off+3];
}

static unsigned short
km_ushort(buf, off)
    unsigned char *buf;
    int off;
{
    return (unsigned short)(((unsigned short)buf[off] << 8) | buf[off+1]);
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void
print_verbose(b)
    struct board *b;
{
    int i;
    if (b->det == DET_KMEM)
        printf("  Detected:  bootinfo.autocon[] via /dev/kmem\n");
    else
        printf("  Detected:  AutoConfig ROM (/dev/mem)\n");
    printf("  Type:    0x%02X  Zorro II  %s%s%s%s\n",
           (unsigned)b->er_type,
           size_str(b->size),
           (b->er_type & ERT_MEMLIST)   ? "  MEMLIST"   : "",
           (b->er_type & ERT_DIAGVALID) ? "  DIAGVALID" : "",
           (b->er_type & ERT_CHAINED)   ? "  CHAINED"   : "");
    printf("  Flags:   0x%02X\n", (unsigned)b->er_flags);
    printf("  Serial:  0x%08lX\n", b->serial);
    printf("  DiagVec: 0x%04X\n", (unsigned)b->diagvec);
    printf("  Raw (%s 0x00-0x3F):\n",
           b->det == DET_KMEM ? "ConfigDev struct bytes" : "physical bytes");
    for (i = 0; i < RAW_SAVE; i++) {
        if ((i & 15) == 0)  printf("    %02X: ", i);
        printf("%02X ", (unsigned)b->raw[i]);
        if ((i & 15) == 15) printf("\n");
    }
    printf("\n");
}

static void
print_board(b)
    struct board *b;
{
    const char *mname, *pname;
    mname = zorro_lookup_manuf(b->manuf);
    pname = zorro_lookup_product(b->manuf, b->prod);
    printf("0x%08lX  %04X:%02X  %s%s%s\n",
           b->base,
           (unsigned)b->manuf, (unsigned)b->prod,
           mname  ? mname  : "Unknown",
           pname  ? " -- " : "",
           pname  ? pname  : "");
    if (opt_verbose)
        print_verbose(b);
}

/* ------------------------------------------------------------------ */
/* Board recording (deduplication by base address)                    */
/* ------------------------------------------------------------------ */

static void
record_board(b)
    struct board *b;
{
    int i;
    for (i = 0; i < nfound; i++) {
        if (found[i].base == b->base)
            return;
    }
    if (nfound >= MAX_BOARDS)
        return;
    nfound++;
    if (!opt_all)
        print_board(b);
}

/* ------------------------------------------------------------------ */
/* Method 1: read bootinfo.autocon[] from /dev/kmem                   */
/* ------------------------------------------------------------------ */

static void
scan_kmem()
{
    int fd, i, n;
    unsigned char buf[KM_STRUCT_SIZE];
    struct board *b;

    fd = open("/dev/kmem", O_RDONLY);
    if (fd < 0)
        return;

    if (lseek(fd, BOOTINFO_VADDR, 0) < 0) {
        close(fd);
        return;
    }

    for (i = 0; i < NAUTO; i++) {
        n = read(fd, (char *)buf, KM_STRUCT_SIZE);
        if (n != KM_STRUCT_SIZE)
            break;

        b = &found[nfound];
        b->manuf   = km_ushort(buf, KM_ER_MANUF);
        b->prod    = buf[KM_ER_PRODUCT];
        b->er_type = buf[KM_ER_TYPE];
        b->er_flags= buf[KM_ER_FLAGS];
        b->serial  = km_ulong(buf, KM_ER_SERIAL);
        b->diagvec = km_ushort(buf, KM_ER_DIAGVEC);
        b->base    = km_ulong(buf, KM_BOARDADDR);
        b->size    = size_table[b->er_type & ERT_SIZEMASK];
        b->det     = DET_KMEM;
        for (n = 0; n < RAW_SAVE; n++)
            b->raw[n] = buf[n];

        if (b->manuf == 0 && b->base == 0)
            continue;

        record_board(b);
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/* Method 2: AutoConfig ROM scan via /dev/mem                         */
/* ------------------------------------------------------------------ */

static void
probe(fd, base)
    int fd;
    unsigned long base;
{
    unsigned char *mem;
    struct board *b;
    int i;

    mem = (unsigned char *)mmap(
        (caddr_t)0, AC_MAP_SIZE, PROT_READ, MAP_SHARED,
        fd, (off_t)base);
    if (mem == (unsigned char *)-1)
        return;

    b = &found[nfound];
    for (i = 0; i < RAW_SAVE; i++)
        b->raw[i] = mem[i];

    if (is_autoconfig(mem)) {
        b->det     = DET_AUTOCONFIG;
        b->er_type = ac_byte(mem, AC_TYPE);
        b->prod    = ac_byte(mem, AC_PRODUCT);
        b->er_flags= ac_byte(mem, AC_FLAGS);
        b->manuf   = ac_word(mem, AC_MANUF);
        b->serial  = ac_long(mem, AC_SERIAL);
        b->diagvec = ac_word(mem, AC_DIAGVEC);
        b->base    = base;
        b->size    = size_table[b->er_type & ERT_SIZEMASK];
        munmap((caddr_t)mem, AC_MAP_SIZE);
        if (b->manuf == 0)
            return;
        record_board(b);
        return;
    }

    munmap((caddr_t)mem, AC_MAP_SIZE);
}

static void
scan_devmem(fd, base, end, step)
    int fd;
    unsigned long base, end, step;
{
    unsigned long addr;
    for (addr = base; addr <= end; addr += step)
        probe(fd, addr);
}

/* ------------------------------------------------------------------ */
/* -a: all database entries                                            */
/* ------------------------------------------------------------------ */

static void
print_all()
{
    const struct zorro_id_entry *e;
    int i, idx;

    printf("%-8s  %-10s  %-7s  %s\n",
           "Status", "Address", "ID", "Description");
    printf("%-8s  %-10s  %-7s  %s\n",
           "--------", "----------", "-------", "-----------");

    for (e = zorro_ids; e->manuf_name != NULL; e++) {
        if (e->prod == -1)
            continue;

        idx = -1;
        for (i = 0; i < nfound; i++) {
            if (found[i].manuf == e->manuf &&
                found[i].prod  == (unsigned char)e->prod) {
                idx = i;
                break;
            }
        }

        if (idx >= 0) {
            printf("FOUND     0x%08lX  %04X:%02X  %s -- %s\n",
                   found[idx].base,
                   (unsigned)e->manuf,
                   (unsigned)(unsigned char)e->prod,
                   e->manuf_name,
                   e->prod_name ? e->prod_name : "");
        } else {
            printf("-         ----------  %04X:%02X  %s -- %s\n",
                   (unsigned)e->manuf,
                   (unsigned)(unsigned char)e->prod,
                   e->manuf_name,
                   e->prod_name ? e->prod_name : "");
        }
    }

    if (nfound == 0)
        printf("\n(no boards found)\n");
    else
        printf("\n%d board(s) found.\n", nfound);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(argc, argv)
    int argc;
    char **argv;
{
    int fd, i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        switch (argv[i][1]) {
        case 'v': opt_verbose = 1; break;
        case 'a': opt_all     = 1; break;
        default:
            fprintf(stderr, "Usage: %s [-v] [-a]\n", argv[0]);
            return 1;
        }
    }

    if (!opt_all) {
        printf("Address     ID        Description\n");
        printf("----------  --------  -----------\n");
    }

    /* Method 1: kernel bootinfo (best coverage, no mmap needed) */
    scan_kmem();

    /* Methods 2+3: /dev/mem scan (catches VA2000 and any non-kmem boards) */
    fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/mem");
        fprintf(stderr, "(must run as root)\n");
        if (nfound == 0) return 1;
    } else {
        scan_devmem(fd, ZII_IO_BASE,  ZII_IO_END,  ZII_IO_STEP);
        scan_devmem(fd, ZII_MEM_BASE, ZII_MEM_END, ZII_MEM_STEP);
        close(fd);
    }

    if (opt_all) {
        print_all();
    } else if (nfound == 0) {
        printf("(no Zorro boards found)\n");
    } else {
        printf("\n%d board(s) found.\n", nfound);
    }

    return 0;
}
