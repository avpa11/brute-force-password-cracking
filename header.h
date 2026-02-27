#ifndef HEADER_H
#define HEADER_H

#include <stdint.h>
#include <time.h>

enum { MSG_REGISTER = 1, MSG_JOB, MSG_RESULT, MSG_HEARTBEAT_REQ, MSG_HEARTBEAT_RESP,
       MSG_REQUEST_CHUNK, MSG_CHUNK_ASSIGN, MSG_STOP };
enum { ALGO_MD5 = 1, ALGO_BCRYPT, ALGO_SHA256 = 5, ALGO_SHA512, ALGO_YESCRYPT };

/* Search space: 1-, 2-, 3-, or 4-char passwords, printable ASCII 33..111 (79 chars). */
#define PW_CMIN 33
#define PW_CMAX 111
#define PW_CRANGE (PW_CMAX - PW_CMIN + 1)
#define PW_MAX_LEN 4
/* Total: 79 + 79^2 + 79^3 + 79^4 */
#define TOTAL_CANDIDATES ((uint64_t)(PW_CRANGE) + (uint64_t)(PW_CRANGE)*(PW_CRANGE) + (uint64_t)(PW_CRANGE)*(PW_CRANGE)*(PW_CRANGE) + (uint64_t)(PW_CRANGE)*(PW_CRANGE)*(PW_CRANGE)*(PW_CRANGE))

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

typedef struct {
    uint64_t delta_tested;
    uint64_t total_tested;
    uint32_t threads_active;
    double current_rate;
} __attribute__((packed)) HeartbeatResponse;

typedef struct {
    uint64_t start_idx;
    uint64_t count;
} __attribute__((packed)) ChunkAssign;

typedef struct { struct timespec start, end; } Timer;

static inline double get_elapsed_ms(Timer *t) {
    return (t->end.tv_sec - t->start.tv_sec) * 1000.0 + (t->end.tv_nsec - t->start.tv_nsec) / 1e6;
}

#endif
