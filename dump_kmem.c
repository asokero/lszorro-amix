/*
 * dump_kmem.c - dump /dev/kmem around found board cluster
 * Dumps 5*68=340 bytes starting ~80 bytes before 0x080DC3EC
 */
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>

/* Start 80 bytes before 0x080DC3EC to see full first ConfigDev */
#define DUMP_START  0x080DC398L
#define DUMP_BYTES  420

main()
{
    int fd, n, i;
    unsigned char buf[DUMP_BYTES];
    unsigned long pos;

    fd = open("/dev/kmem", O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    if (lseek(fd, DUMP_START, 0) < 0) { perror("lseek"); return 1; }
    n = read(fd, (char *)buf, DUMP_BYTES);
    close(fd);

    printf("Dump from 0x%08lX (%d bytes):\n", DUMP_START, n);
    for (i = 0; i < n; i++) {
        if (i % 16 == 0)
            printf("\n%08lX: ", DUMP_START + i);
        printf("%02X ", buf[i]);
    }
    printf("\n");
    return 0;
}
