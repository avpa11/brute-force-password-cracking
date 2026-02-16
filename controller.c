#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include "header.h"

static Timer t_parse, t_dispatch, t_return, t_total;

static const char *algo_name(uint8_t a) {
    const char *names[] = {[ALGO_MD5]="MD5", [ALGO_BCRYPT]="bcrypt",
                           [ALGO_SHA256]="SHA-256", [ALGO_SHA512]="SHA-512",
                           [ALGO_YESCRYPT]="yescrypt"};
    return (a <= ALGO_YESCRYPT && names[a]) ? names[a] : "Unknown";
}

static int parse_shadow(const char *path, const char *user, CrackJob *job) {
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "Error: Cannot open '%s': %s\n", path, strerror(errno)); return -1; }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *u = strtok(line, ":"), *h = strtok(NULL, ":");
        if (!u || !h || strcmp(u, user) != 0) continue;

        if (h[0] != '$') { fclose(fp); return -1; }

        char *p1 = strchr(h+1, '$'), *p2 = p1 ? strchr(p1+1, '$') : NULL;
        if (!p1 || !p2) { fclose(fp); return -1; }

        *p1 = 0;
        char *algo = h+1;
        if (!strcmp(algo,"1")) job->algorithm = ALGO_MD5;
        else if (algo[0]=='2') job->algorithm = ALGO_BCRYPT;
        else if (!strcmp(algo,"5")) job->algorithm = ALGO_SHA256;
        else if (!strcmp(algo,"6")) job->algorithm = ALGO_SHA512;
        else if (!strcmp(algo,"y")) job->algorithm = ALGO_YESCRYPT;
        else { fprintf(stderr, "Error: Unknown algorithm '%s'\n", algo); fclose(fp); return -1; }

        if (job->algorithm == ALGO_BCRYPT) {
            char *combined = p2+1;
            if (strlen(combined) < 53) { fclose(fp); return -1; }

            size_t rounds_len = p2 - (p1+1);
            if (rounds_len + 1 + 22 >= MAX_SALT_LEN) { fclose(fp); return -1; }

            memcpy(job->salt, p1+1, rounds_len);
            job->salt[rounds_len] = '$';
            memcpy(job->salt + rounds_len + 1, combined, 22);
            job->salt[rounds_len + 1 + 22] = 0;

            strncpy(job->target_hash, combined + 22, MAX_HASH_LEN-1);
        } else if (job->algorithm == ALGO_YESCRYPT) {
            char *p3 = strchr(p2+1, '$');
            if (!p3) { fclose(fp); return -1; }

            size_t slen = p3 - (p1+1);
            if (slen >= MAX_SALT_LEN) { fclose(fp); return -1; }
            memcpy(job->salt, p1+1, slen);
            job->salt[slen] = 0;

            strncpy(job->target_hash, p3+1, MAX_HASH_LEN-1);
        } else {
            size_t slen = p2 - (p1+1);
            if (slen >= MAX_SALT_LEN) { fclose(fp); return -1; }
            memcpy(job->salt, p1+1, slen);
            job->salt[slen] = 0;

            strncpy(job->target_hash, p2+1, MAX_HASH_LEN-1);
        }
        char *nl = strchr(job->target_hash, '\n');
        if (nl) *nl = 0;

        printf("Parsed shadow file:\n  Algorithm: %s (ID: %d)\n  Salt: %s\n  Hash: %s\n",
               algo_name(job->algorithm), job->algorithm, job->salt, job->target_hash);
        fclose(fp);
        return 0;
    }
    fprintf(stderr, "Error: User '%s' not found\n", user);
    fclose(fp);
    return -1;
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

int main(int argc, char *argv[]) {
    clock_gettime(CLOCK_MONOTONIC, &t_total.start);
    printf("=== CONTROLLER STARTED ===\n");

    char *shadow = NULL, *user = NULL;
    int port = -1, heartbeat_sec = 0, opt;
    while ((opt = getopt(argc, argv, "f:u:p:b:")) != -1) {
        if (opt == 'f') shadow = optarg;
        else if (opt == 'u') user = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 'b') heartbeat_sec = atoi(optarg);
    }
    if (!shadow || !user || port <= 0 || heartbeat_sec <= 0) {
        fprintf(stderr, "Usage: %s -f <shadow_file> -u <username> -p <port> -b <heartbeat_seconds>\n", argv[0]);
        return 1;
    }
    printf("Arguments: shadow_file=%s, username=%s, port=%d, heartbeat=%ds\n\n", shadow, user, port, heartbeat_sec);

    clock_gettime(CLOCK_MONOTONIC, &t_parse.start);
    CrackJob job = {0};
    if (parse_shadow(shadow, user, &job) < 0) return 1;
    clock_gettime(CLOCK_MONOTONIC, &t_parse.end);
    printf("\n");

    int server = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {.sin_family=AF_INET, .sin_addr.s_addr=INADDR_ANY, .sin_port=htons(port)};
    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(server, 1) < 0) {
        fprintf(stderr, "Error: Cannot bind/listen: %s\n", strerror(errno));
        return 1;
    }
    printf("Listening on port %d...\nWaiting for worker connection...\n", port);

    struct sockaddr_in waddr;
    socklen_t wlen = sizeof(waddr);
    int worker = accept(server, (struct sockaddr*)&waddr, &wlen);
    if (worker < 0) { fprintf(stderr, "Error: accept: %s\n", strerror(errno)); return 1; }
    printf("Worker connected from %s:%d\n\n", inet_ntoa(waddr.sin_addr), ntohs(waddr.sin_port));

    uint8_t msg;
    if (recv(worker, &msg, 1, 0) <= 0 || msg != MSG_REGISTER) {
        fprintf(stderr, "Error: Registration failed\n");
        return 1;
    }
    printf("Received MSG_REGISTER from worker\n");

    clock_gettime(CLOCK_MONOTONIC, &t_dispatch.start);
    msg = MSG_JOB;
    send(worker, &msg, 1, 0);
    send(worker, &job, sizeof(job), 0);
    clock_gettime(CLOCK_MONOTONIC, &t_dispatch.end);
    printf("Sent MSG_JOB to worker\nJob details: algorithm=%d, salt=%s\n\n", job.algorithm, job.salt);
    printf("Worker is cracking password (searching 79^3 = 493,039 candidates)...\n\n");

    int heartbeat_count = 0;
    int got_result = 0;
    CrackResult result;

    while (!got_result) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(worker, &fds);
        struct timeval tv = { .tv_sec = heartbeat_sec, .tv_usec = 0 };

        int ret = select(worker + 1, &fds, NULL, NULL, &tv);

        if (ret == 0) {
            msg = MSG_HEARTBEAT_REQ;
            send(worker, &msg, 1, 0);
            heartbeat_count++;
            printf("[Heartbeat #%d] Sent heartbeat request\n", heartbeat_count);
            continue;
        }

        if (ret < 0) {
            fprintf(stderr, "Error: select: %s\n", strerror(errno));
            break;
        }

        if (recv(worker, &msg, 1, 0) <= 0) {
            fprintf(stderr, "Error: Worker disconnected\n");
            break;
        }

        if (msg == MSG_HEARTBEAT_RESP) {
            HeartbeatResponse hb;
            if (recv_full(worker, &hb, sizeof(hb)) <= 0) {
                fprintf(stderr, "Error: Failed to receive heartbeat response\n");
                break;
            }
            printf("[Heartbeat #%d] delta=%lu total=%lu threads=%u rate=%.0f/s\n",
                   heartbeat_count, (unsigned long)hb.delta_tested, (unsigned long)hb.total_tested,
                   hb.threads_active, hb.current_rate);
        } else if (msg == MSG_RESULT) {
            clock_gettime(CLOCK_MONOTONIC, &t_return.start);
            if (recv_full(worker, &result, sizeof(result)) <= 0) {
                fprintf(stderr, "Error: Failed to receive result\n");
                break;
            }
            clock_gettime(CLOCK_MONOTONIC, &t_return.end);
            got_result = 1;
            printf("\nReceived MSG_RESULT from worker\n\n");
        } else {
            fprintf(stderr, "Warning: Unknown message type %d\n", msg);
        }
    }

    if (!got_result) {
        fprintf(stderr, "Error: Did not receive result from worker\n");
        close(worker);
        close(server);
        return 1;
    }

    printf("========================================\n===== PASSWORD CRACKING RESULT =====\n========================================\n");
    if (result.found) printf("  Password FOUND: \"%s\"\n", result.password);
    else printf("  Password NOT found (searched all 493,039 candidates)\n");

    clock_gettime(CLOCK_MONOTONIC, &t_total.end);
    printf("\n========================================\n===== TIMING BREAKDOWN =====\n========================================\n");
    printf("Parsing shadow file:    %10.3f ms\n", get_elapsed_ms(&t_parse));
    printf("Job dispatch latency:   %10.3f ms\n", get_elapsed_ms(&t_dispatch));
    printf("Worker cracking time:   %10.3f ms\n", result.worker_crack_time_ms);
    printf("Result return latency:  %10.3f ms\n", get_elapsed_ms(&t_return));
    printf("Heartbeats exchanged:   %10d\n", heartbeat_count);
    printf("----------------------------------------\n");
    printf("Total elapsed time:     %10.3f ms\n", get_elapsed_ms(&t_total));
    printf("========================================\n");

    close(worker);
    close(server);
    printf("\n=== CONTROLLER TERMINATED ===\n");
    return result.found ? 0 : 1;
}
