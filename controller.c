#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
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

        // bcrypt and yescrypt have different formats
        if (job->algorithm == ALGO_BCRYPT) {
            // bcrypt: $2b$rounds$salt_and_hash (salt=22chars, hash=31chars, total=53chars)
            // We need: salt="rounds$22char_salt", hash="31char_hash"
            char *combined = p2+1;
            if (strlen(combined) < 53) { fclose(fp); return -1; }

            // Extract rounds and first 22 chars of combined string
            size_t rounds_len = p2 - (p1+1);
            if (rounds_len + 1 + 22 >= MAX_SALT_LEN) { fclose(fp); return -1; }

            memcpy(job->salt, p1+1, rounds_len);  // Copy rounds
            job->salt[rounds_len] = '$';
            memcpy(job->salt + rounds_len + 1, combined, 22);  // Copy 22-char salt
            job->salt[rounds_len + 1 + 22] = 0;

            // Hash is the remaining 31 chars
            strncpy(job->target_hash, combined + 22, MAX_HASH_LEN-1);
        } else if (job->algorithm == ALGO_YESCRYPT) {
            // yescrypt: $y$params$salt$hash (has 4 fields)
            char *p3 = strchr(p2+1, '$');
            if (!p3) { fclose(fp); return -1; }

            size_t slen = p3 - (p1+1);
            if (slen >= MAX_SALT_LEN) { fclose(fp); return -1; }
            memcpy(job->salt, p1+1, slen);
            job->salt[slen] = 0;

            strncpy(job->target_hash, p3+1, MAX_HASH_LEN-1);
        } else {
            // MD5, SHA256, SHA512: $algo$salt$hash (has 3 fields)
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

int main(int argc, char *argv[]) {
    clock_gettime(CLOCK_MONOTONIC, &t_total.start);
    printf("=== CONTROLLER STARTED ===\n");

    char *shadow = NULL, *user = NULL;
    int port = -1, opt;
    while ((opt = getopt(argc, argv, "f:u:p:")) != -1) {
        if (opt == 'f') shadow = optarg;
        else if (opt == 'u') user = optarg;
        else if (opt == 'p') port = atoi(optarg);
    }
    if (!shadow || !user || port <= 0) {
        fprintf(stderr, "Usage: %s -f <shadow_file> -u <username> -p <port>\n", argv[0]);
        return 1;
    }
    printf("Arguments: shadow_file=%s, username=%s, port=%d\n\n", shadow, user, port);

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
    printf("Worker is cracking password (searching 79³ = 493,039 candidates)...\n");

    if (recv(worker, &msg, 1, 0) <= 0 || msg != MSG_RESULT) {
        fprintf(stderr, "Error: Result failed\n");
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_return.start);
    CrackResult result;
    recv(worker, &result, sizeof(result), 0);
    clock_gettime(CLOCK_MONOTONIC, &t_return.end);
    printf("Received MSG_RESULT from worker\n\n");

    printf("========================================\n===== PASSWORD CRACKING RESULT =====\n========================================\n");
    if (result.found) printf("✓ Password FOUND: \"%s\"\n", result.password);
    else printf("✗ Password NOT found (searched all 493,039 candidates)\n");

    clock_gettime(CLOCK_MONOTONIC, &t_total.end);
    printf("\n========================================\n===== TIMING BREAKDOWN =====\n========================================\n");
    printf("Parsing shadow file:    %10.3f ms\n", get_elapsed_ms(&t_parse));
    printf("Job dispatch latency:   %10.3f ms\n", get_elapsed_ms(&t_dispatch));
    printf("Worker cracking time:   %10.3f ms\n", result.worker_crack_time_ms);
    printf("Result return latency:  %10.3f ms\n", get_elapsed_ms(&t_return));
    printf("----------------------------------------\n");
    printf("Total elapsed time:     %10.3f ms\n", get_elapsed_ms(&t_total));
    printf("========================================\n");

    close(worker);
    close(server);
    printf("\n=== CONTROLLER TERMINATED ===\n");
    return result.found ? 0 : 1;
}
