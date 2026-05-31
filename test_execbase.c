/*
 * test_execbase.c - ExecBase hypothesis test for lszorro
 *
 * Probes whether AMIX SVR4 preserves AmigaOS expansion.library data
 * in low memory. Reads the Zorro board list via:
 *
 *   0x4 -> ExecBase -> LibList -> expansion.library -> eb_BoardList -> ConfigDev[]
 *
 * Build: cc -o test_execbase test_execbase.c
 * Run:   root only (requires /dev/mem)
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>

typedef unsigned char   UBYTE;
typedef unsigned short  UWORD;
typedef unsigned long   ULONG;

#define EXECBASE_PTR   0x00000004UL
#define EXEC_LIBLIST   0x0220
#define EXP_BOARDLIST  0x013C
#define LN_SUCC        0
#define LN_NAME        10
#define CD_ROM         0x10
#define CD_BOARDADDR   0x28
#define CD_BOARDSIZE   0x2C
#define ER_PRODUCT     0x01
#define ER_MANUF       0x04
#define MAX_ITER       256

static int g_fd = -1;

static int mem_read(addr, buf, len)
    ULONG addr;
    char *buf;
    int len;
{
    if (lseek(g_fd, (long)addr, 0) < 0L) {
        fprintf(stderr, "lseek 0x%lx: ", addr);
        perror("");
        return -1;
    }
    if (read(g_fd, buf, len) != len) {
        fprintf(stderr, "read 0x%lx: ", addr);
        perror("");
        return -1;
    }
    return 0;
}

static ULONG rd32(addr)
    ULONG addr;
{
    ULONG v;
    v = 0;
    mem_read(addr, (char *)&v, 4);
    return v;
}

static UWORD rd16(addr)
    ULONG addr;
{
    UWORD v;
    v = 0;
    mem_read(addr, (char *)&v, 2);
    return v;
}

static UBYTE rd8(addr)
    ULONG addr;
{
    UBYTE v;
    v = 0;
    mem_read(addr, (char *)&v, 1);
    return v;
}

static void rdstr(addr, buf, max)
    ULONG addr;
    char *buf;
    int max;
{
    int i;
    for (i = 0; i < max - 1; i++) {
        buf[i] = (char)rd8(addr + (ULONG)i);
        if (buf[i] == '\0')
            break;
    }
    buf[i] = '\0';
}

static int ptr_ok(p)
    ULONG p;
{
    /* Allow any plausible 32-bit pointer: not null, not bus-float (0xFFFFFFFF).
     * A3000 fast RAM lives at 0x08000000, well above the 16MB chip limit. */
    return (p > 0x100UL && p != 0xFFFFFFFFUL);
}

int main(argc, argv)
    int argc;
    char **argv;
{
    ULONG execbase, liblist_head, node, succ, nameptr;
    ULONG expbase, boardlist_head, cd;
    ULONG boardaddr, boardsize;
    UWORD manuf;
    UBYTE product;
    char name[64];
    int iter, board_count;

    g_fd = open("/dev/mem", O_RDONLY);
    if (g_fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    /* Step 1: ExecBase pointer at 0x00000004 */
    execbase = rd32(EXECBASE_PTR);
    printf("ExecBase @ 0x4 = 0x%08lx\n", execbase);
    if (!ptr_ok(execbase)) {
        printf("FAIL: ExecBase invalid - AMIX has overwritten low memory\n");
        close(g_fd);
        return 1;
    }
    printf("OK\n\n");

    /* Step 2: LibList at ExecBase+0x220 */
    liblist_head = rd32(execbase + EXEC_LIBLIST);
    printf("LibList.lh_Head = 0x%08lx\n", liblist_head);
    if (!ptr_ok(liblist_head)) {
        printf("FAIL: LibList.lh_Head invalid\n");
        close(g_fd);
        return 1;
    }

    /* Step 3: Walk LibList, find expansion.library */
    printf("\nWalking exec LibList:\n");
    expbase = 0UL;
    node = liblist_head;
    for (iter = 0; iter < MAX_ITER; iter++) {
        succ = rd32(node + LN_SUCC);
        if (succ == 0UL)
            break;
        nameptr = rd32(node + LN_NAME);
        name[0] = '\0';
        if (ptr_ok(nameptr))
            rdstr(nameptr, name, sizeof(name));
        printf("  0x%08lx  %s\n", node, name);
        if (strcmp(name, "expansion.library") == 0)
            expbase = node;
        node = succ;
    }

    if (expbase == 0UL) {
        printf("\nFAIL: expansion.library not found in LibList\n");
        close(g_fd);
        return 1;
    }
    printf("\nOK: ExpansionBase = 0x%08lx\n\n", expbase);

    /* Step 4: eb_BoardList at ExpansionBase+0x13C */
    boardlist_head = rd32(expbase + EXP_BOARDLIST);
    printf("eb_BoardList.lh_Head = 0x%08lx\n", boardlist_head);
    if (!ptr_ok(boardlist_head)) {
        printf("FAIL: eb_BoardList head invalid (AMIX may have cleared it)\n");
        close(g_fd);
        return 1;
    }
    printf("OK\n\n");

    /* Step 5: Walk ConfigDev list */
    printf("Zorro boards from expansion.library:\n");
    printf("  %-12s %-8s %-9s %s\n", "BoardAddr", "Manuf", "Product", "Size");
    board_count = 0;
    cd = boardlist_head;
    for (iter = 0; iter < MAX_ITER; iter++) {
        succ = rd32(cd + LN_SUCC);
        if (succ == 0UL)
            break;
        manuf     = rd16(cd + CD_ROM + ER_MANUF);
        product   = rd8(cd + CD_ROM + ER_PRODUCT);
        boardaddr = rd32(cd + CD_BOARDADDR);
        boardsize = rd32(cd + CD_BOARDSIZE);
        printf("  0x%08lx   0x%04x   0x%02x      0x%08lx\n",
               boardaddr, (unsigned)manuf, (unsigned)product, boardsize);
        board_count++;
        cd = succ;
    }

    if (board_count == 0)
        printf("  (none - eb_BoardList is empty)\n");
    else
        printf("\nOK: %d board(s) found via expansion.library\n", board_count);

    close(g_fd);
    return 0;
}
