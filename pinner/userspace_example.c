#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "pinner.h"

#define BUF_SIZE 5215

int main() {
    char *mybuf = NULL;
    int fd = -1;
    
    int i;
    int n;
    
    fd = open("/dev/pinner", O_RDWR);
    if (fd == -1) {
        perror("Could not open /dev/pinner");
        goto cleanup;
    }
    
    mybuf = malloc(BUF_SIZE);
    
    struct pinner_handle handle;
    struct pinner_physlist plist;
    
    struct pinner_cmd pin_cmd = {
        .cmd = PINNER_PIN,
        .usr_buf = mybuf,
        .usr_buf_sz = BUF_SIZE,
        .handle = &handle,
        .physlist = &plist
    };
    
    n = write(fd, &pin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write pin command to pinner");
        goto cleanup;
    }
    
    printf("plist.num_entries = %u\n", plist.num_entries);
    for (int i = 0; i < plist.num_entries; i++) {
        printf("SG entry: address 0x%lX with length %u\n", plist.entries[i].addr, plist.entries[i].len);
    }
    
    struct pinner_cmd unpin_cmd = {
        .cmd = PINNER_UNPIN,
        .handle = &handle
    };
    n = write(fd, &unpin_cmd, sizeof(struct pinner_cmd));
    if (n < 0) {
        perror("Could not write unpin command to pinner");
        goto cleanup;
    }
    
    cleanup:
    if (mybuf) free(mybuf);
    if (fd != -1) close(fd);
}
