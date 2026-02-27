#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <crypt.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include "header.h"

/* Use same search space as header (1..4 char, printable 33..111) */
#define CRANGE PW_CRANGE
#define CMIN   PW_CMIN
#define CMAX   PW_CMAX

/* Boundaries: len 1 [0,79), len 2 [79,6320), len 3 [6320,499359), len 4 [499359,TOTAL) */
#define OFF_LEN1  0ULL
#define OFF_LEN2  ((uint64_t)CRANGE)                                    /* 79 */
#define OFF_LEN3  ((uint64_t)CRANGE + (uint64_t)CRANGE*CRANGE)          /* 6320 */
#define OFF_LEN4  ((uint64_t)CRANGE + (uint64_t)CRANGE*CRANGE + (uint64_t)CRANGE*CRANGE*CRANGE)  /* 499359 */

static atomic_int g_found = 0;
static atomic_int g_stop_requested = 0;   /* set by reader thread on MSG_STOP so crack threads exit */
static atomic_uint_least64_t g_tested = 0;
static atomic_uint_least64_t g_last_reported = 0;
static atomic_int g_threads_active = 0;
static char g_password[MAX_PASSWORD_LEN];
static pthread_mutex_t g_password_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const CrackJob *job;
    char fmt[256];
    uint64_t chunk_start;
    uint64_t chunk_count;
    int thread_id;
    int num_threads;
} ThreadArg;

static double elapsed_ms(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000.0 + (now.tv_nsec - start->tv_nsec) / 1e6;
}

/* Map global index to password (1, 2, 3, or 4 chars). pw must have at least 5 bytes. */
static void idx_to_pw(uint64_t idx, char *pw) {
    if (idx < OFF_LEN2) {
        pw[0] = (char)(CMIN + (int)idx);
        pw[1] = '\0';
        return;
    }
    if (idx < OFF_LEN3) {
        uint64_t i = idx - OFF_LEN2;
        pw[1] = (char)(CMIN + (int)(i % CRANGE));
        pw[0] = (char)(CMIN + (int)(i / CRANGE));
        pw[2] = '\0';
        return;
    }
    if (idx < OFF_LEN4) {
        uint64_t i = idx - OFF_LEN3;
        pw[2] = (char)(CMIN + (int)(i % CRANGE));
        i /= CRANGE;
        pw[1] = (char)(CMIN + (int)(i % CRANGE));
        pw[0] = (char)(CMIN + (int)(i / CRANGE));
        pw[3] = '\0';
        return;
    }
    {
        uint64_t i = idx - OFF_LEN4;
        pw[3] = (char)(CMIN + (int)(i % CRANGE));
        i /= CRANGE;
        pw[2] = (char)(CMIN + (int)(i % CRANGE));
        i /= CRANGE;
        pw[1] = (char)(CMIN + (int)(i % CRANGE));
        pw[0] = (char)(CMIN + (int)(i / CRANGE));
        pw[4] = '\0';
    }
}

static void *crack_chunk_thread(void *arg) {
    ThreadArg *ta = (ThreadArg *)arg;
    atomic_fetch_add(&g_threads_active, 1);

    char pw[PW_MAX_LEN + 2];  /* up to 4 chars + null, extra for safety */
    memset(pw, 0, sizeof(pw));
    struct crypt_data cd;
    uint64_t end = ta->chunk_start + ta->chunk_count;

    for (uint64_t idx = ta->chunk_start + (uint64_t)ta->thread_id; idx < end; idx += (uint64_t)ta->num_threads) {
        if (atomic_load(&g_found)) break;
        if (atomic_load(&g_stop_requested)) break;

        idx_to_pw(idx, pw);

        memset(&cd, 0, sizeof(cd));
        char *h = crypt_r(pw, ta->fmt, &cd);
        if (!h) continue;

        char *hp = h;
        for (int d = 0; d < 2 && hp; d++) hp = strchr(hp + 1, '$');
        if (hp) hp++;

        if (hp && strcmp(hp, ta->job->target_hash) == 0) {
            atomic_store(&g_found, 1);
            pthread_mutex_lock(&g_password_lock);
            strncpy(g_password, pw, MAX_PASSWORD_LEN - 1);
            g_password[MAX_PASSWORD_LEN - 1] = '\0';
            pthread_mutex_unlock(&g_password_lock);
        }

        atomic_fetch_add(&g_tested, 1);
    }

    atomic_fetch_sub(&g_threads_active, 1);
    return NULL;
}

/* Crack one chunk [chunk_start, chunk_start+chunk_count) using num_threads. Returns 1 if found. */
static int crack_chunk(const CrackJob *job, const char *fmt, uint64_t chunk_start, uint64_t chunk_count,
                       int num_threads, double *out_elapsed_ms) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    atomic_store(&g_found, 0);

    pthread_t *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    ThreadArg *targs = malloc((size_t)num_threads * sizeof(ThreadArg));
    if (!threads || !targs) {
        free(threads);
        free(targs);
        return -1;
    }

    for (int i = 0; i < num_threads; i++) {
        targs[i].job = job;
        strncpy(targs[i].fmt, fmt, sizeof(targs[i].fmt) - 1);
        targs[i].fmt[sizeof(targs[i].fmt) - 1] = 0;
        targs[i].chunk_start = chunk_start;
        targs[i].chunk_count = chunk_count;
        targs[i].thread_id = i;
        targs[i].num_threads = num_threads;
        pthread_create(&threads[i], NULL, crack_chunk_thread, &targs[i]);
    }

    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    *out_elapsed_ms = elapsed_ms(&t0);
    free(threads);
    free(targs);
    return atomic_load(&g_found) ? 1 : 0;
}

static ssize_t recv_full(int fd, void *buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(fd, (char *)buf + received, len - received, 0);
        if (n <= 0) return n;
        received += n;
    }
    return (ssize_t)received;
}

/* Shared state for main thread <-> reader thread */
typedef struct {
    int sock;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int chunk_ready;
    int stop_received;
    ChunkAssign pending_chunk;
    struct timespec crack_start_total;
    volatile int reader_done;  /* reader thread exited (socket closed/error) */
} ReaderState;

static void *reader_thread(void *arg) {
    ReaderState *rs = (ReaderState *)arg;
    uint8_t msg;

    while (!rs->reader_done) {
        if (recv(rs->sock, &msg, 1, 0) <= 0) {
            rs->reader_done = 1;
            pthread_mutex_lock(&rs->mutex);
            rs->stop_received = 1;
            pthread_cond_signal(&rs->cond);
            pthread_mutex_unlock(&rs->mutex);
            return NULL;
        }

        if (msg == MSG_HEARTBEAT_REQ) {
            uint64_t total = atomic_load(&g_tested);
            uint64_t last = atomic_exchange(&g_last_reported, total);
            uint64_t delta = total - last;
            double elapsed = (rs->crack_start_total.tv_sec != 0)
                ? (elapsed_ms((struct timespec *)&rs->crack_start_total)) : 0;
            double rate = (elapsed > 0) ? (total / (elapsed / 1000.0)) : 0;
            HeartbeatResponse hb = {
                .delta_tested = delta,
                .total_tested = total,
                .threads_active = (uint32_t)atomic_load(&g_threads_active),
                .current_rate = rate
            };
            uint8_t resp_msg = MSG_HEARTBEAT_RESP;
            send(rs->sock, &resp_msg, 1, 0);
            send(rs->sock, &hb, sizeof(hb), 0);
            continue;
        }

        if (msg == MSG_STOP) {
            atomic_store(&g_stop_requested, 1);
            pthread_mutex_lock(&rs->mutex);
            rs->stop_received = 1;
            pthread_cond_signal(&rs->cond);
            pthread_mutex_unlock(&rs->mutex);
            continue;
        }

        if (msg == MSG_CHUNK_ASSIGN) {
            if (recv_full(rs->sock, &rs->pending_chunk, sizeof(rs->pending_chunk)) <= 0) {
                rs->reader_done = 1;
                pthread_mutex_lock(&rs->mutex);
                rs->stop_received = 1;
                pthread_cond_signal(&rs->cond);
                pthread_mutex_unlock(&rs->mutex);
                return NULL;
            }
            pthread_mutex_lock(&rs->mutex);
            rs->chunk_ready = 1;
            pthread_cond_signal(&rs->cond);
            pthread_mutex_unlock(&rs->mutex);
            continue;
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    printf("=== WORKER STARTED ===\n");

    char *host = NULL;
    int port = -1, num_threads = 1, opt;
    while ((opt = getopt(argc, argv, "c:p:t:")) != -1) {
        if (opt == 'c') host = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 't') num_threads = atoi(optarg);
    }
    if (!host || port <= 0 || num_threads <= 0) {
        fprintf(stderr, "Usage: %s -c <controller_host> -p <port> -t <threads>\n", argv[0]);
        return 1;
    }
    printf("Arguments: controller=%s, port=%d, threads=%d\n\n", host, port, num_threads);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct hostent *he = gethostbyname(host);
    if (!he) { fprintf(stderr, "Error: Cannot resolve '%s'\n", host); return 1; }

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    printf("Connecting to controller at %s:%d...\n", host, port);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot connect: %s\n", strerror(errno));
        return 1;
    }
    printf("Connected to controller\n\n");

    uint8_t msg = MSG_REGISTER;
    send(sock, &msg, 1, 0);
    printf("Sent MSG_REGISTER to controller\nWaiting for job...\n");

    if (recv(sock, &msg, 1, 0) <= 0 || msg != MSG_JOB) {
        fprintf(stderr, "Error: Expected MSG_JOB\n");
        close(sock);
        return 1;
    }
    CrackJob job;
    if (recv_full(sock, &job, sizeof(job)) <= 0) {
        fprintf(stderr, "Error: Failed to receive job\n");
        close(sock);
        return 1;
    }
    printf("Received MSG_JOB from controller\n");

    char fmt[256];
    switch (job.algorithm) {
        case ALGO_MD5:     snprintf(fmt, sizeof(fmt), "$1$%s$", job.salt); break;
        case ALGO_BCRYPT:  snprintf(fmt, sizeof(fmt), "$2b$%s", job.salt); break;
        case ALGO_SHA256:  snprintf(fmt, sizeof(fmt), "$5$%s$", job.salt); break;
        case ALGO_SHA512:  snprintf(fmt, sizeof(fmt), "$6$%s$", job.salt); break;
        case ALGO_YESCRYPT: snprintf(fmt, sizeof(fmt), "$y$%s", job.salt); break;
        default: fprintf(stderr, "Error: Unsupported algorithm %d\n", job.algorithm); close(sock); return 1;
    }

    atomic_store(&g_tested, 0);
    atomic_store(&g_last_reported, 0);
    atomic_store(&g_stop_requested, 0);
    g_password[0] = 0;

    struct timespec crack_start_total;
    clock_gettime(CLOCK_MONOTONIC, &crack_start_total);
    int found = 0;
    CrackResult res = {0};

    ReaderState rs = {
        .sock = sock,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .chunk_ready = 0,
        .stop_received = 0,
        .reader_done = 0
    };
    rs.crack_start_total = crack_start_total;

    pthread_t reader_tid;
    if (pthread_create(&reader_tid, NULL, reader_thread, &rs) != 0) {
        fprintf(stderr, "Error: pthread_create reader\n");
        close(sock);
        return 1;
    }

    for (;;) {
        pthread_mutex_lock(&rs.mutex);
        rs.chunk_ready = 0;
        rs.stop_received = 0;
        pthread_mutex_unlock(&rs.mutex);

        msg = MSG_REQUEST_CHUNK;
        if (send(sock, &msg, 1, 0) <= 0) break;

        pthread_mutex_lock(&rs.mutex);
        while (!rs.chunk_ready && !rs.stop_received && !rs.reader_done)
            pthread_cond_wait(&rs.cond, &rs.mutex);
        int stop = rs.stop_received;
        int chunk_ready = rs.chunk_ready;
        ChunkAssign ca = rs.pending_chunk;
        pthread_mutex_unlock(&rs.mutex);

        if (stop || rs.reader_done) {
            if (!found) {
                res.found = 0;
                res.password[0] = '\0';
                res.worker_crack_time_ms = elapsed_ms(&crack_start_total);
                printf("STOP received (no more work or password found elsewhere). Exiting.\n");
                msg = MSG_RESULT;
                send(sock, &msg, 1, 0);
                send(sock, &res, sizeof(res), 0);
            }
            break;
        }

        if (!chunk_ready) break;

        if (ca.count == 0) {
            if (!found) {
                res.found = 0;
                res.password[0] = '\0';
                res.worker_crack_time_ms = elapsed_ms(&crack_start_total);
                printf("STOP (no more work). Exiting.\n");
                msg = MSG_RESULT;
                send(sock, &msg, 1, 0);
                send(sock, &res, sizeof(res), 0);
            }
            break;
        }

        printf("Chunk: start=%lu count=%lu\n", (unsigned long)ca.start_idx, (unsigned long)ca.count);
        double chunk_ms;
        int chunk_found = crack_chunk(&job, fmt, ca.start_idx, ca.count, num_threads, &chunk_ms);
        if (atomic_load(&g_stop_requested)) {
            printf("STOP received during chunk. Exiting.\n");
            if (!found) {
                res.found = 0;
                res.password[0] = '\0';
                res.worker_crack_time_ms = elapsed_ms(&crack_start_total);
                msg = MSG_RESULT;
                send(sock, &msg, 1, 0);
                send(sock, &res, sizeof(res), 0);
            }
            break;
        }
        if (chunk_found) {
            res.found = 1;
            pthread_mutex_lock(&g_password_lock);
            strncpy(res.password, g_password, MAX_PASSWORD_LEN - 1);
            res.password[MAX_PASSWORD_LEN - 1] = '\0';
            pthread_mutex_unlock(&g_password_lock);
            res.worker_crack_time_ms = elapsed_ms(&crack_start_total);
            found = 1;
            printf("  PASSWORD FOUND: \"%s\"\n", res.password);
            msg = MSG_RESULT;
            send(sock, &msg, 1, 0);
            send(sock, &res, sizeof(res), 0);
            break;
        }
    }

    rs.reader_done = 1;
    close(sock);
    pthread_join(reader_tid, NULL);

    printf("Total tested: %lu candidates\n", (unsigned long)atomic_load(&g_tested));
    printf("\n=== WORKER TERMINATED ===\n");
    return found ? 0 : 1;
}
