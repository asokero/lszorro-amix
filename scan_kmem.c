/*
 * scan_kmem.c - search /dev/kmem for bootinfo.autocon[] by known board address
 * Searches 0x08000000 to 0x08200000 for known board addresses:
 *   0x00E90000 = A2065 at slot 0
 *   0x00EA0000 = Prelude at slot 1
 */
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>

#define SEARCH_START  0x08000000L
#define SEARCH_END    0x08200000L
#define BLOCKSIZE     4096

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
    unsigned char buf[BLOCKSIZE + 4];
    unsigned long pos;
    int n, i;
    unsigned long val;

    fd = open("/dev/kmem", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/kmem");
        return 1;
    }

    if (lseek(fd, SEARCH_START, 0) < 0) {
        perror("lseek");
        return 1;
    }

    pos = SEARCH_START;
    printf("Scanning 0x%08lX - 0x%08lX for 0x00E90000 or 0x00EA0000...\n",
           SEARCH_START, SEARCH_END);

    while (pos < SEARCH_END) {
        n = read(fd, (char *)buf, BLOCKSIZE);
        if (n <= 0) {
            fprintf(stderr, "read error at 0x%08lX\n", pos);
            break;
        }
        for (i = 0; i <= n - 4; i++) {
            val = get_ulong(buf, i);
            if (val == 0x00E90000L || val == 0x00EA0000L ||
                val == 0x00200000L || val == 0x00EB0000L) {
                printf("Found 0x%08lX at kmem offset 0x%08lX\n",
                       val, pos + i);
            }
        }
        pos += n;
    }

    printf("Done.\n");
    close(fd);
    return 0;
}
