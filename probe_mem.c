/*
 * probe_mem.c - Raw /dev/mem probe for Zorro II address diagnostics
 *
 * Tries to mmap every 64KB slot in the Zorro II range and prints
 * the first 32 bytes. Helps diagnose why lszorro misses certain cards.
 *
 * Compile: cc probe_mem.c -o probe_mem
 * Run as root.
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>

#define STEP  0x10000UL
#define DUMP  32

static unsigned long ranges[][2] = {
    { 0x00200000UL, 0x009FFFFFUL },  /* Zorro II memory area */
    { 0x00A00000UL, 0x00BFFFFFUL },  /* extended Zorro II */
    { 0x00E90000UL, 0x00EFFFFFUL },  /* Zorro II I/O slots */
    { 0, 0 }
};

static int
all_same(mem, len, val)
    unsigned char *mem;
    int len;
    unsigned char val;
{
    int i;
    for (i = 0; i < len; i++)
        if (mem[i] != val) return 0;
    return 1;
}

int
main(argc, argv)
    int argc;
    char **argv;
{
    int fd, r, i;
    unsigned long addr;
    unsigned char *mem;
    unsigned char first;

    fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    printf("%-10s  %-6s  %s\n", "Address", "Status", "First 32 bytes");
    printf("%-10s  %-6s  %s\n", "----------", "------",
           "--------------------------------");

    for (r = 0; ranges[r][0] != 0; r++) {
        for (addr = ranges[r][0]; addr <= ranges[r][1]; addr += STEP) {
            mem = (unsigned char *)mmap(
                (caddr_t)0, DUMP, PROT_READ, MAP_SHARED,
                fd, (off_t)addr);

            if (mem == (unsigned char *)-1) {
                printf("0x%08lX  FAIL  (mmap error)\n", addr);
                continue;
            }

            first = mem[0];

            /* Skip bus-float: all 0xFF */
            if (all_same(mem, DUMP, 0xFF)) {
                munmap((caddr_t)mem, DUMP);
                continue;
            }

            printf("0x%08lX  OK    ", addr);
            for (i = 0; i < DUMP; i++)
                printf("%02X ", (unsigned)mem[i]);
            printf("\n");

            munmap((caddr_t)mem, DUMP);
        }
    }

    close(fd);
    return 0;
}
