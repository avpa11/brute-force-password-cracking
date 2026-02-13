#ifndef HEADER_H
#define HEADER_H

#include <stdint.h>
#include <time.h>

enum { MSG_REGISTER = 1, MSG_JOB, MSG_RESULT };
enum { ALGO_MD5 = 1, ALGO_BCRYPT, ALGO_SHA256 = 5, ALGO_SHA512, ALGO_YESCRYPT };

#define MAX_SALT_LEN     64
#define MAX_HASH_LEN     256
#define MAX_PASSWORD_LEN 64

typedef struct {
    uint8_t algorithm;
    char salt[MAX_SALT_LEN];
    char target_hash[MAX_HASH_LEN];
} __attribute__((packed)) CrackJob;

typedef struct {
    uint8_t found;
    char password[MAX_PASSWORD_LEN];
    double worker_crack_time_ms;
} __attribute__((packed)) CrackResult;

typedef struct { struct timespec start, end; } Timer;

static inline double get_elapsed_ms(Timer *t) {
    return (t->end.tv_sec - t->start.tv_sec) * 1000.0 + (t->end.tv_nsec - t->start.tv_nsec) / 1e6;
}

#endif
