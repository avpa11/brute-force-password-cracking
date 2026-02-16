#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <crypt.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <password> <salt_prefix>\n", argv[0]);
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s ABC '$1$saltsalt$'          # MD5\n", argv[0]);
        fprintf(stderr, "  %s ABC '$2b$05$saltsaltsaltsaltsalt12'  # bcrypt\n", argv[0]);
        fprintf(stderr, "  %s ABC '$5$saltsalt$'          # SHA-256\n", argv[0]);
        fprintf(stderr, "  %s ABC '$6$saltsalt$'          # SHA-512\n", argv[0]);
        fprintf(stderr, "  %s ABC '$y$j9T$n34PoBLMgFrQVl4Rn34Po/' # yescrypt\n", argv[0]);
        return 1;
    }

    struct crypt_data cd = {0};
    char *hash = crypt_r(argv[1], argv[2], &cd);
    if (!hash) {
        fprintf(stderr, "Error: crypt_r failed\n");
        return 1;
    }

    printf("%s\n", hash);
    return 0;
}
