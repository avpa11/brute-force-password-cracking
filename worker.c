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

#define CMIN 33
#define CMAX 111
#define CRANGE (CMAX - CMIN + 1)
#define TOTAL (CRANGE * CRANGE * CRANGE)

static atomic_int g_found = 0;
static atomic_uint_least64_t g_tested = 0;
static atomic_uint_least64_t g_last_reported = 0;
static atomic_int g_threads_active = 0;
static char g_password[MAX_PASSWORD_LEN];
static pthread_mutex_t g_password_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const CrackJob *job;
    char fmt[256];
    int thread_id;
    int num_threads;
} ThreadArg;

static double elapsed_ms(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000.0 + (now.tv_nsec - start->tv_nsec) / 1e6;
}

static void idx_to_pw(int idx, char pw[4]) {
    pw[2] = CMIN + (idx % CRANGE);
    idx /= CRANGE;
    pw[1] = CMIN + (idx % CRANGE);
    idx /= CRANGE;
    pw[0] = CMIN + idx;
    pw[3] = 0;
}

static void *crack_thread(void *arg) {
    ThreadArg *ta = (ThreadArg *)arg;
    atomic_fetch_add(&g_threads_active, 1);

    char pw[4] = {0};
    struct crypt_data cd;

    for (int idx = ta->thread_id; idx < TOTAL; idx += ta->num_threads) {
        if (atomic_load(&g_found)) break;

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
            pthread_mutex_unlock(&g_password_lock);
        }

        atomic_fetch_add(&g_tested, 1);
    }

    atomic_fetch_sub(&g_threads_active, 1);
    return NULL;
}

typedef struct {
    int sock;
    volatile int running;
    struct timespec crack_start;
} HeartbeatArg;

static void *heartbeat_thread(void *arg) {
    HeartbeatArg *ha = (HeartbeatArg *)arg;

    while (ha->running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ha->sock, &fds);
        struct timeval tv = {0, 200000};

        int ret = select(ha->sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        uint8_t msg;
        ssize_t n = recv(ha->sock, &msg, 1, MSG_DONTWAIT);
        if (n <= 0) break;

        if (msg == MSG_HEARTBEAT_REQ) {
            uint64_t total = atomic_load(&g_tested);
            uint64_t last = atomic_exchange(&g_last_reported, total);
            uint64_t delta = total - last;
            double elapsed = elapsed_ms(&ha->crack_start);
            double rate = (elapsed > 0) ? (total / (elapsed / 1000.0)) : 0;

            HeartbeatResponse resp = {
                .delta_tested = delta,
                .total_tested = total,
                .threads_active = (uint32_t)atomic_load(&g_threads_active),
                .current_rate = rate
            };

            uint8_t resp_msg = MSG_HEARTBEAT_RESP;
            send(ha->sock, &resp_msg, 1, 0);
            send(ha->sock, &resp, sizeof(resp), 0);

            printf("[Heartbeat] delta=%lu total=%lu/%d active_threads=%u rate=%.0f/s\n",
                   (unsigned long)delta, (unsigned long)total, TOTAL,
                   resp.threads_active, rate);
        }
    }
    return NULL;
}

static int crack(const CrackJob *job, CrackResult *res, int num_threads, int sock) {
    printf("Starting password cracking with %d thread(s)...\n", num_threads);
    printf("  Algorithm: %d\n  Salt: %s\n  Target hash: %s\n", job->algorithm, job->salt, job->target_hash);
    printf("  Target hash length: %zu\n  Candidate space: %d^3 = %d candidates\n\n", strlen(job->target_hash), CRANGE, TOTAL);

    char fmt[256];
    switch (job->algorithm) {
        case ALGO_MD5:     snprintf(fmt, sizeof(fmt), "$1$%s$", job->salt); break;
        case ALGO_BCRYPT:  snprintf(fmt, sizeof(fmt), "$2b$%s", job->salt); break;
        case ALGO_SHA256:  snprintf(fmt, sizeof(fmt), "$5$%s$", job->salt); break;
        case ALGO_SHA512:  snprintf(fmt, sizeof(fmt), "$6$%s$", job->salt); break;
        case ALGO_YESCRYPT: snprintf(fmt, sizeof(fmt), "$y$%s", job->salt); break;
        default: fprintf(stderr, "Error: Unsupported algorithm %d\n", job->algorithm); return -1;
    }

    atomic_store(&g_found, 0);
    atomic_store(&g_tested, 0);
    atomic_store(&g_last_reported, 0);
    atomic_store(&g_threads_active, 0);
    g_password[0] = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    HeartbeatArg hb_arg = { .sock = sock, .running = 1, .crack_start = t0 };
    pthread_t hb_tid;
    pthread_create(&hb_tid, NULL, heartbeat_thread, &hb_arg);

    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    ThreadArg *targs = malloc(sizeof(ThreadArg) * num_threads);

    for (int i = 0; i < num_threads; i++) {
        targs[i].job = job;
        strncpy(targs[i].fmt, fmt, sizeof(targs[i].fmt) - 1);
        targs[i].fmt[sizeof(targs[i].fmt) - 1] = 0;
        targs[i].thread_id = i;
        targs[i].num_threads = num_threads;

        int count = (TOTAL - i + num_threads - 1) / num_threads;
        printf("  Thread %d: stride=%d, ~%d candidates\n", i, num_threads, count);
        pthread_create(&threads[i], NULL, crack_thread, &targs[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    double crack_time = elapsed_ms(&t0);

    hb_arg.running = 0;
    pthread_join(hb_tid, NULL);

    res->worker_crack_time_ms = crack_time;
    if (atomic_load(&g_found)) {
        res->found = 1;
        pthread_mutex_lock(&g_password_lock);
        strncpy(res->password, g_password, MAX_PASSWORD_LEN - 1);
        pthread_mutex_unlock(&g_password_lock);
        printf("\n  PASSWORD FOUND: \"%s\"\n  Tested %lu candidates in %.3f ms\n",
               res->password, (unsigned long)atomic_load(&g_tested), crack_time);
    } else {
        res->found = 0;
        res->password[0] = 0;
        printf("\n  Password NOT found after testing all %lu candidates in %.3f ms\n",
               (unsigned long)atomic_load(&g_tested), crack_time);
    }

    free(threads);
    free(targs);
    return 0;
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
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    printf("Connecting to controller at %s:%d...\n", host, port);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot connect: %s\n", strerror(errno));
        return 1;
    }
    printf("Connected to controller\n\n");

    uint8_t msg = MSG_REGISTER;
    send(sock, &msg, 1, 0);
    printf("Sent MSG_REGISTER to controller\nWaiting for cracking job...\n");

    if (recv(sock, &msg, 1, 0) <= 0 || msg != MSG_JOB) {
        fprintf(stderr, "Error: Expected MSG_JOB\n");
        return 1;
    }
    CrackJob job;
    recv(sock, &job, sizeof(job), 0);
    printf("Received MSG_JOB from controller\n\n");

    CrackResult res = {0};
    if (crack(&job, &res, num_threads, sock) < 0) { close(sock); return 1; }

    printf("\nSending result to controller...\n");
    msg = MSG_RESULT;
    send(sock, &msg, 1, 0);
    send(sock, &res, sizeof(res), 0);
    printf("Sent MSG_RESULT to controller\n");

    close(sock);
    printf("\n=== WORKER TERMINATED ===\n");
    return 0;
}
