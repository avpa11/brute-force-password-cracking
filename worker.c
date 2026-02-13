#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <crypt.h>
#include <time.h>
#include "header.h"

#define CMIN 33
#define CMAX 111
#define CRANGE (CMAX - CMIN + 1)
#define TOTAL (CRANGE * CRANGE * CRANGE)
#define PROGRESS_INTERVAL 50000

static double elapsed_ms(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000.0 + (now.tv_nsec - start->tv_nsec) / 1e6;
}

static int crack(const CrackJob *job, CrackResult *res) {
    printf("Starting password cracking...\n");
    printf("  Algorithm: %d\n  Salt: %s\n  Target hash: %s\n", job->algorithm, job->salt, job->target_hash);
    printf("  Target hash length: %zu\n  Candidate space: %d³ = %d candidates\n\n", strlen(job->target_hash), CRANGE, TOTAL);

    char fmt[256];
    switch (job->algorithm) {
        case ALGO_MD5:     snprintf(fmt, sizeof(fmt), "$1$%s$", job->salt); break;
        case ALGO_BCRYPT:  snprintf(fmt, sizeof(fmt), "$2b$%s", job->salt); break;
        case ALGO_SHA256:  snprintf(fmt, sizeof(fmt), "$5$%s$", job->salt); break;
        case ALGO_SHA512:  snprintf(fmt, sizeof(fmt), "$6$%s$", job->salt); break;
        case ALGO_YESCRYPT: snprintf(fmt, sizeof(fmt), "$y$%s", job->salt); break;
        default: fprintf(stderr, "Error: Unsupported algorithm %d\n", job->algorithm); return -1;
    }

    char pw[4] = {0};
    int n = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < CRANGE; i++) {
        pw[0] = CMIN + i;
        for (int j = 0; j < CRANGE; j++) {
            pw[1] = CMIN + j;
            for (int k = 0; k < CRANGE; k++) {
                pw[2] = CMIN + k;
                n++;

                struct crypt_data cd = {0};
                char *h = crypt_r(pw, fmt, &cd);
                if (!h) { fprintf(stderr, "Error: crypt_r failed\n"); return -1; }

                char *hp = h;
                for (int d = 0; d < 2 && hp; d++) hp = strchr(hp + 1, '$');
                if (hp) hp++;

                if (hp && strcmp(hp, job->target_hash) == 0) {
                    res->found = 1;
                    strncpy(res->password, pw, MAX_PASSWORD_LEN - 1);
                    res->worker_crack_time_ms = elapsed_ms(&t0);
                    printf("\n✓ PASSWORD FOUND: \"%s\"\n  Tested %d candidates in %.3f ms\n", pw, n, res->worker_crack_time_ms);
                    return 0;
                }

                if (n % PROGRESS_INTERVAL == 0) {
                    double e = elapsed_ms(&t0);
                    printf("Progress: %d / %d candidates (%.1f%%) - %.0f candidates/sec\n", n, TOTAL, n * 100.0 / TOTAL, n / (e / 1000));
                }
            }
        }
    }

    res->found = 0;
    res->password[0] = 0;
    res->worker_crack_time_ms = elapsed_ms(&t0);
    printf("\n✗ Password NOT found after testing all %d candidates in %.3f ms\n", n, res->worker_crack_time_ms);
    return 0;
}

int main(int argc, char *argv[]) {
    printf("=== WORKER STARTED ===\n");

    char *host = NULL;
    int port = -1, opt;
    while ((opt = getopt(argc, argv, "c:p:")) != -1) {
        if (opt == 'c') host = optarg;
        else if (opt == 'p') port = atoi(optarg);
    }
    if (!host || port <= 0) {
        fprintf(stderr, "Usage: %s -c <controller_host> -p <port>\n", argv[0]);
        return 1;
    }
    printf("Arguments: controller=%s, port=%d\n\n", host, port);

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
    if (crack(&job, &res) < 0) { close(sock); return 1; }

    printf("\nSending result to controller...\n");
    msg = MSG_RESULT;
    send(sock, &msg, 1, 0);
    send(sock, &res, sizeof(res), 0);
    printf("Sent MSG_RESULT to controller\n");

    close(sock);
    printf("\n=== WORKER TERMINATED ===\n");
    return 0;
}
