/*
 * lszorro.c - List Zorro expansion boards on AMIX SVR4
 *
 * Scans Zorro II address space via /dev/mem, decodes AutoConfig
 * ROM structures, identifies boards from the built-in database.
 *
 * Usage: lszorro [-v] [-a]
 *   -v  verbose: decoded fields + raw hex dump
 *   -a  list all database entries (mark found / not found)
 *
 * AutoConfig ROM encoding (Amiga Hardware Reference Manual):
 *   The ROM is nibble-wide on the Zorro bus. Each logical byte N is
 *   stored as two 16-bit words at physical offsets N*4 and N*4+2,
 *   with only the upper nibble (D15-D12) of each word carrying data.
 *   All fields except er_Type (byte 0) are ones-complement inverted.
 *
 * Compile: cc lszorro.c -o lszorro
 * Must run as root (/dev/mem access).
 *
 * Detection methods:
 *   AutoConfig: reads nibble-encoded ROM at card base (A2065, Prelude, etc.)
 *   Fingerprint: direct register read for cards that don't expose AutoConfig
 *     VA2000: 16-bit fw_version at offset 0, range 1-511
 *
 * Note: Piccolo (Helfrich, 0x0893:05/06/0A/0B) is NOT detectable on a
 * running AMIX because its framebuffer (0x200000) and register window
 * (0xEB0000) are exclusively owned by the AMIX display driver.
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

/* Physical bytes needed to cover all AutoConfig fields (diagvec at byte 10) */
#define AC_MAP_SIZE   0x80

/* AutoConfig logical byte positions */
#define AC_TYPE    0   /* er_Type:         type + size flags (not inverted) */
#define AC_PRODUCT 1   /* er_Product:      product number */
#define AC_FLAGS   2   /* er_Flags:        board flags */
#define AC_RESV    3   /* er_Reserved03:   must be 0x00 after decode */
#define AC_MANUF   4   /* er_Manufacturer: UWORD, 2 bytes */
#define AC_SERIAL  6   /* er_SerialNumber: ULONG, 4 bytes */
#define AC_DIAGVEC 10  /* er_InitDiagVec:  UWORD, 2 bytes */

/* er_Type bit masks */
#define ERT_TYPEMASK  0xC0
#define ERT_ZORROII   0xC0
#define ERT_MEMLIST   0x20
#define ERT_DIAGVALID 0x10
#define ERT_CHAINED   0x08
#define ERT_SIZEMASK  0x07

#define MAX_BOARDS 32
#define RAW_SAVE   64   /* physical bytes saved per board for hex dump */

#define DET_AUTOCONFIG  0
#define DET_FINGERPRINT 1

struct board {
    unsigned long  base;
    unsigned long  size;
    unsigned short manuf;
    unsigned char  prod;
    unsigned char  det;      /* DET_AUTOCONFIG or DET_FINGERPRINT */
    unsigned char  er_type;
    unsigned char  er_flags;
    unsigned long  serial;
    unsigned short diagvec;
    unsigned short fw_ver;   /* fingerprint: direct firmware version */
    unsigned char  raw[RAW_SAVE];
};

static struct board found[MAX_BOARDS];
static int nfound = 0;

static int opt_verbose = 0;
static int opt_all     = 0;

/* Board size indexed by er_Type bits 2-0 */
static unsigned long size_table[8] = {
    8UL * 1024UL * 1024UL,  /* 0: 8MB  */
    64UL * 1024UL,           /* 1: 64KB */
    128UL * 1024UL,          /* 2: 128KB */
    256UL * 1024UL,          /* 3: 256KB */
    512UL * 1024UL,          /* 4: 512KB */
    1UL * 1024UL * 1024UL,   /* 5: 1MB  */
    2UL * 1024UL * 1024UL,   /* 6: 2MB  */
    4UL * 1024UL * 1024UL    /* 7: 4MB  */
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

    /* Reject bus float: all 0xFF or all 0x00 */
    for (i = 0; i < 16; i++)
        if (mem[i] != 0xFF) break;
    if (i == 16) return 0;
    for (i = 0; i < 16; i++)
        if (mem[i] != 0x00) break;
    if (i == 16) return 0;

    er_type = ac_byte(mem, AC_TYPE);
    if ((er_type & ERT_TYPEMASK) != ERT_ZORROII)
        return 0;

    /* Reserved byte must decode to 0x00 */
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

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void
print_verbose(b)
    struct board *b;
{
    int i;
    if (b->det == DET_FINGERPRINT) {
        printf("  Detected:  fingerprint (direct register read)\n");
        printf("  FW ver:    0x%04X (%u)\n",
               (unsigned)b->fw_ver, (unsigned)b->fw_ver);
        printf("  Size:      %s (Zorro II window)\n", size_str(b->size));
    } else {
        printf("  Type:    0x%02X  Zorro II  %s%s%s%s\n",
               (unsigned)b->er_type,
               size_str(b->size),
               (b->er_type & ERT_MEMLIST)   ? "  MEMLIST"   : "",
               (b->er_type & ERT_DIAGVALID) ? "  DIAGVALID" : "",
               (b->er_type & ERT_CHAINED)   ? "  CHAINED"   : "");
        printf("  Flags:   0x%02X\n", (unsigned)b->er_flags);
        printf("  Serial:  0x%08lX\n", b->serial);
        printf("  DiagVec: 0x%04X\n", (unsigned)b->diagvec);
    }
    printf("  Raw (physical bytes 0x00-0x3F):\n");
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
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

static void
record_board(b)
    struct board *b;
{
    /* Called with b == &found[nfound], already filled in.
     * Deduplicates and increments nfound. */
    int i, dup;
    dup = 0;
    if (b->det == DET_AUTOCONFIG) {
        for (i = 0; i < nfound; i++) {
            if (found[i].manuf  == b->manuf &&
                found[i].prod   == b->prod  &&
                found[i].serial == b->serial) {
                dup = 1; break;
            }
        }
    } else {
        for (i = 0; i < nfound; i++) {
            if (found[i].base == b->base) {
                dup = 1; break;
            }
        }
    }
    if (dup || nfound >= MAX_BOARDS)
        return;
    nfound++;
    if (!opt_all)
        print_board(b);
}

static void
probe(fd, base)
    int fd;
    unsigned long base;
{
    unsigned char *mem;
    struct board *b;
    unsigned short fw;
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
        b->fw_ver  = 0;
        munmap((caddr_t)mem, AC_MAP_SIZE);
        record_board(b);
        return;
    }

    /* VA2000 fingerprint: direct 16-bit fw_version at offset 0.
     * Range 1-511 matches known firmware versions; rules out
     * framebuffer data (which has large or zero values). */
    fw = (unsigned short)(((unsigned short)mem[0] << 8) | mem[1]);
    munmap((caddr_t)mem, AC_MAP_SIZE);

    if (fw >= 1 && fw <= 511) {
        b->det     = DET_FINGERPRINT;
        b->manuf   = 0x6D6E;
        b->prod    = 0x00;
        b->base    = base;
        b->size    = 64UL * 1024UL;
        b->fw_ver  = fw;
        b->er_type = 0;
        b->er_flags= 0;
        b->serial  = 0;
        b->diagvec = 0;
        record_board(b);
    }
}

static void
scan(fd, base, end, step)
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

    fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/mem");
        fprintf(stderr, "(must run as root)\n");
        return 1;
    }

    if (!opt_all) {
        printf("Address     ID        Description\n");
        printf("----------  --------  -----------\n");
    }

    scan(fd, ZII_IO_BASE,  ZII_IO_END,  ZII_IO_STEP);
    scan(fd, ZII_MEM_BASE, ZII_MEM_END, ZII_MEM_STEP);

    close(fd);

    if (opt_all) {
        print_all();
    } else if (nfound == 0) {
        printf("(no Zorro boards found)\n");
    } else {
        printf("\n%d board(s) found.\n", nfound);
    }

    return 0;
}
