/*
 * read_kmem.c - read bootinfo.autocon[] from kernel /dev/kmem
 * bootinfo virtual address: 0x080DBAC4
 * Usage: ./read_kmem
 */
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>

/*
 * Empirically determined from live /dev/kmem scan:
 * bootinfo.autocon[0] confirmed at 0x080DC3C8.
 * ConfigDev struct = 68 bytes (AMIX-specific layout).
 * Field offsets verified against A2065, Prelude, Picasso II, VLab.
 */
#define BOOTINFO_VADDR  0x080DC3C8L
#define NAUTO           16
#define CONFIGDEV_SIZE  68

/* ConfigDev field offsets (verified from /dev/kmem dump) */
#define CD_ER_TYPE      0x14   /* UBYTE */
#define CD_ER_PRODUCT   0x15   /* UBYTE */
#define CD_ER_MANUF     0x18   /* UWORD big-endian */
#define CD_BOARDADDR    0x24   /* ULONG big-endian */
#define CD_BOARDSIZE    0x28   /* ULONG big-endian */

static unsigned short
get_ushort(buf, off)
unsigned char *buf;
int off;
{
    return ((unsigned short)buf[off] << 8) | buf[off+1];
}

static unsigned long
get_ulong(buf, off)
unsigned char *buf;
int off;
{
    return ((unsigned long)buf[off]   << 24) |
           ((unsigned long)buf[off+1] << 16) |
           ((unsigned long)buf[off+2] <<  8) |
            (unsigned long)buf[off+3];
}

main()
{
    int fd;
    int i, n;
    unsigned char buf[CONFIGDEV_SIZE];
    unsigned short manuf;
    unsigned char prod, er_type;
    unsigned long addr, size;

    fd = open("/dev/kmem", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/kmem");
        return 1;
    }

    if (lseek(fd, BOOTINFO_VADDR, 0) < 0) {
        perror("lseek");
        return 1;
    }

    printf("bootinfo.autocon[] at 0x%08lX:\n\n", BOOTINFO_VADDR);
    printf("%-4s  %-9s  %-9s  %-9s  %-5s\n",
           "Idx", "Manuf:Prod", "BoardAddr", "BoardSize", "Type");

    for (i = 0; i < NAUTO; i++) {
        n = read(fd, (char *)buf, CONFIGDEV_SIZE);
        if (n != CONFIGDEV_SIZE) {
            fprintf(stderr, "read error at entry %d\n", i);
            break;
        }
        manuf = get_ushort(buf, CD_ER_MANUF);
        prod  = buf[CD_ER_PRODUCT];
        er_type = buf[CD_ER_TYPE];
        addr  = get_ulong(buf, CD_BOARDADDR);
        size  = get_ulong(buf, CD_BOARDSIZE);

        if (manuf == 0 && addr == 0 && prod == 0)
            continue;

        printf("[%2d]  %04X:%02X     0x%08lX  0x%08lX  type=0x%02X\n",
               i, manuf, prod, addr, size, er_type);
    }

    close(fd);
    return 0;
}
