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
#include "util.h"

#define MAX_WORKERS 64

static Timer t_parse, t_dispatch, t_return, t_total;

typedef struct {
    int fd;
    int registered;
    int disconnected;
    uint64_t chunk_start;       /* start of currently assigned chunk */
    uint64_t chunk_count;       /* size of currently assigned chunk */
    uint64_t last_checkpoint;   /* last checkpoint idx received; re-queue from here on death */
    int pending_heartbeat;      /* 1 if we sent a req but haven't seen a response yet */
} WorkerSlot;

typedef struct { uint64_t start, count; } WorkRange;
static WorkRange recovery_queue[MAX_WORKERS];
static int recovery_count = 0;

static WorkerSlot workers[MAX_WORKERS];
static int num_workers = 0;
static int heartbeat_interval_sec = 0;
static volatile int g_found = 0;
static CrackResult g_result;
static int heartbeat_count = 0;
static struct timespec t_first_chunk_dispatch;
static struct timespec t_last_activity;

static const char *algo_name(uint8_t a) {
    const char *names[] = {[ALGO_MD5]="MD5", [ALGO_BCRYPT]="bcrypt",
                           [ALGO_SHA256]="SHA-256", [ALGO_SHA512]="SHA-512",
                           [ALGO_YESCRYPT]="yescrypt"};
    return (a <= ALGO_YESCRYPT && names[a]) ? names[a] : "Unknown";
}

static int parse_shadow(const char *path, const char *user, CrackJob *job) {
    FILE *fp = fopen(path, "r");
    if (!fp) { logerrorf("Error: Cannot open '%s': %s\n", path, strerror(errno)); return -1; }

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
        else { logerrorf("Error: Unknown algorithm '%s'\n", algo); fclose(fp); return -1; }

        if (job->algorithm == ALGO_BCRYPT) {
            char *combined = p2+1;
            if (strlen(combined) < 53) { fclose(fp); return -1; }

            size_t rounds_len = p2 - (p1+1);
            if (rounds_len + 1 + 22 >= MAX_SALT_LEN) { fclose(fp); return -1; }

            memcpy(job->salt, p1+1, rounds_len);
            job->salt[rounds_len] = '$';
            memcpy(job->salt + rounds_len + 1, combined, 22);
            job->salt[rounds_len + 1 + 22] = 0;

            strncpy(job->target_hash, combined, MAX_HASH_LEN-1);
        } else if (job->algorithm == ALGO_YESCRYPT) {
            char *p3 = strchr(p2+1, '$');
            if (!p3) { fclose(fp); return -1; }

            size_t slen = p3 - (p1+1);
            if (slen >= MAX_SALT_LEN) { fclose(fp); return -1; }
            memcpy(job->salt, p1+1, slen);
            job->salt[slen] = 0;

            strncpy(job->target_hash, p2+1, MAX_HASH_LEN-1);
        } else {
            size_t slen = p2 - (p1+1);
            if (slen >= MAX_SALT_LEN) { fclose(fp); return -1; }
            memcpy(job->salt, p1+1, slen);
            job->salt[slen] = 0;

            strncpy(job->target_hash, p2+1, MAX_HASH_LEN-1);
        }
        char *nl = strchr(job->target_hash, '\n');
        if (nl) *nl = 0;

        logprintf("Parsed shadow file:\n  Algorithm: %s (ID: %d)\n  Salt: %s\n  Hash: %s\n",
               algo_name(job->algorithm), job->algorithm, job->salt, job->target_hash);
        fclose(fp);
        return 0;
    }
    logerrorf("Error: User '%s' not found\n", user);
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

static void broadcast_stop(void) {
    uint8_t msg = MSG_STOP;
    for (int i = 0; i < num_workers; i++) {
        if (!workers[i].disconnected && workers[i].fd >= 0)
            send(workers[i].fd, &msg, 1, 0);
    }
}

static int add_worker(int fd) {
    if (num_workers >= MAX_WORKERS) {
        close(fd);
        return -1;
    }
    workers[num_workers].fd = fd;
    workers[num_workers].registered = 0;
    workers[num_workers].disconnected = 0;
    num_workers++;
    return 0;
}

static void remove_worker(int idx) {
    if (workers[idx].fd >= 0) close(workers[idx].fd);
    workers[idx].fd = -1;
    workers[idx].disconnected = 1;
}

/* Re-queue a dead worker's unfinished chunk range back to the recovery queue. */
static void requeue_worker(int idx) {
    if (g_found || !workers[idx].registered) return;
    uint64_t resume = workers[idx].last_checkpoint;
    uint64_t end    = workers[idx].chunk_start + workers[idx].chunk_count;
    if (resume < end) {
        if (recovery_count < MAX_WORKERS) {
            recovery_queue[recovery_count].start = resume;
            recovery_queue[recovery_count].count = end - resume;
            recovery_count++;
            logprintf("[Recovery] Worker %d dead: re-queuing [%lu, %lu) (%lu candidates)\n",
                   idx, (unsigned long)resume, (unsigned long)end,
                   (unsigned long)(end - resume));
        } else {
            logerrorf("[Recovery] Worker %d dead: recovery queue full — range [%lu, %lu) LOST!\n",
                    idx, (unsigned long)resume, (unsigned long)end);
        }
    }
}

/* Pop the next work range: recovery queue first, then sequential space. */
static WorkRange next_work(uint64_t chunk_size, uint64_t *next_chunk_start_ptr) {
    WorkRange r = {0, 0};
    if (recovery_count > 0) {
        WorkRange *rq = &recovery_queue[recovery_count - 1];
        r.start = rq->start;
        r.count = (rq->count > chunk_size) ? chunk_size : rq->count;
        rq->start += r.count;
        rq->count -= r.count;
        if (rq->count == 0) recovery_count--;
    } else if (*next_chunk_start_ptr < TOTAL_CANDIDATES) {
        r.start = *next_chunk_start_ptr;
        r.count = chunk_size;
        if (r.start + r.count > TOTAL_CANDIDATES)
            r.count = TOTAL_CANDIDATES - r.start;
        *next_chunk_start_ptr += r.count;
    }
    return r;
}

int main(int argc, char *argv[]) {
    clock_gettime(CLOCK_MONOTONIC, &t_total.start);
    logprintf("=== CONTROLLER STARTED ===\n");

    char *shadow = NULL, *user = NULL, *logpath = NULL;
    int port = -1;
    unsigned long chunk_size = 0;
    unsigned long checkpoint_interval = 0;
    int opt;
    while ((opt = getopt(argc, argv, "f:u:p:b:c:k:l:")) != -1) {
        if (opt == 'f') shadow = optarg;
        else if (opt == 'u') user = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 'b') heartbeat_interval_sec = atoi(optarg);
        else if (opt == 'c') chunk_size = (unsigned long)atol(optarg);
        else if (opt == 'k') checkpoint_interval = (unsigned long)atol(optarg);
        else if (opt == 'l') logpath = optarg;
    }
    if (!shadow || !user || port <= 0 || heartbeat_interval_sec <= 0 || chunk_size == 0 || checkpoint_interval == 0) {
        logerrorf("Usage: %s -f <shadow_file> -u <username> -p <port> -b <heartbeat_seconds> -c <chunk_size> -k <checkpoint_attempts> [-l <logfile>]\n", argv[0]);
        return 1;
    }
    if (logpath) {
        log_open(logpath);
    }
    logprintf("Arguments: shadow_file=%s, username=%s, port=%d, heartbeat=%ds, chunk_size=%lu, checkpoint_interval=%lu\n\n",
           shadow, user, port, heartbeat_interval_sec, chunk_size, checkpoint_interval);

    clock_gettime(CLOCK_MONOTONIC, &t_parse.start);
    CrackJob job = {0};
    if (parse_shadow(shadow, user, &job) < 0) return 1;
    job.checkpoint_interval = (uint64_t)checkpoint_interval;
    clock_gettime(CLOCK_MONOTONIC, &t_parse.end);
    logprintf("Search space: %lu candidates\n\n", (unsigned long)TOTAL_CANDIDATES);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {.sin_family=AF_INET, .sin_addr.s_addr=INADDR_ANY, .sin_port=htons(port)};
    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(server, 32) < 0) {
        logerrorf("Error: Cannot bind/listen: %s\n", strerror(errno));
        return 1;
    }
    logprintf("Listening on port %d... (wait for workers to connect and register)\n", port);

    uint64_t next_chunk_start = 0;
    g_found = 0;
    memset(&g_result, 0, sizeof(g_result));
    for (int i = 0; i < MAX_WORKERS; i++) workers[i].fd = -1;

    clock_gettime(CLOCK_MONOTONIC, &t_dispatch.start);
    t_first_chunk_dispatch = t_dispatch.start;
    t_last_activity = t_dispatch.start;

    int run = 1;
    struct timespec last_heartbeat;
    clock_gettime(CLOCK_MONOTONIC, &last_heartbeat);

    while (run) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server, &fds);
        int maxfd = server;
        for (int i = 0; i < num_workers; i++) {
            if (!workers[i].disconnected && workers[i].fd >= 0) {
                FD_SET(workers[i].fd, &fds);
                if (workers[i].fd > maxfd) maxfd = workers[i].fd;
            }
        }

        /* Compute remaining time until next heartbeat deadline. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_hb = (now.tv_sec - last_heartbeat.tv_sec)
                          + (now.tv_nsec - last_heartbeat.tv_nsec) / 1e9;
        double remaining = heartbeat_interval_sec - elapsed_hb;
        struct timeval tv;
        if (remaining <= 0.0) { tv.tv_sec = 0; tv.tv_usec = 0; }
        else { tv.tv_sec = (long)remaining; tv.tv_usec = (long)((remaining - (long)remaining) * 1e6); }

        int ret = select(maxfd + 1, &fds, NULL, NULL, &tv);

        /* Check heartbeat deadline regardless of whether select timed out or got data. */
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_hb = (now.tv_sec - last_heartbeat.tv_sec)
                   + (now.tv_nsec - last_heartbeat.tv_nsec) / 1e9;
        if (elapsed_hb >= heartbeat_interval_sec) {
            last_heartbeat = now;
            heartbeat_count++;
            for (int i = 0; i < num_workers; i++) {
                if (workers[i].disconnected || !workers[i].registered || workers[i].fd < 0) continue;
                if (workers[i].pending_heartbeat) {
                    logprintf("[Heartbeat #%d] Worker %d missed heartbeat — declared dead\n",
                           heartbeat_count, i);
                    requeue_worker(i);
                    remove_worker(i);
                    continue;
                }
                workers[i].pending_heartbeat = 1;
                uint8_t hb_msg = MSG_HEARTBEAT_REQ;
                send(workers[i].fd, &hb_msg, 1, 0);
            }
            int active = 0;
            for (int i = 0; i < num_workers; i++)
                if (!workers[i].disconnected && workers[i].fd >= 0) active++;
            if (active > 0)
                logprintf("[Heartbeat #%d] Sent heartbeat request to %d worker(s)\n", heartbeat_count, active);
        }

        if (ret == 0) continue;

        if (ret < 0) {
            if (errno == EINTR) continue;
            logerrorf("Error: select: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(server, &fds)) {
            struct sockaddr_in waddr;
            socklen_t wlen = sizeof(waddr);
            int client = accept(server, (struct sockaddr*)&waddr, &wlen);
            if (client >= 0) {
                if (add_worker(client) == 0)
                    logprintf("Worker connected from %s:%d (total workers: %d)\n",
                           inet_ntoa(waddr.sin_addr), ntohs(waddr.sin_port), num_workers);
            }
        }

        for (int i = 0; i < num_workers; i++) {
            if (workers[i].disconnected || workers[i].fd < 0) continue;
            if (!FD_ISSET(workers[i].fd, &fds)) continue;

            uint8_t msg;
            ssize_t n = recv(workers[i].fd, &msg, 1, 0);
            if (n <= 0) {
                /* Unexpected disconnect — re-queue unfinished work. */
                requeue_worker(i);
                remove_worker(i);
                continue;
            }

            if (!workers[i].registered) {
                if (msg != MSG_REGISTER) {
                    remove_worker(i);
                    continue;
                }
                workers[i].registered = 1;
                msg = MSG_JOB;
                send(workers[i].fd, &msg, 1, 0);
                send(workers[i].fd, &job, sizeof(job), 0);
                clock_gettime(CLOCK_MONOTONIC, &t_last_activity);
                logprintf("Sent MSG_JOB to worker %d\n", i);
                continue;
            }

            if (msg == MSG_REQUEST_CHUNK) {
                clock_gettime(CLOCK_MONOTONIC, &t_last_activity);
                if (g_found) {
                    msg = MSG_STOP;
                    send(workers[i].fd, &msg, 1, 0);
                    continue;
                }
                WorkRange wr = next_work(chunk_size, &next_chunk_start);
                if (wr.count == 0) {
                    msg = MSG_STOP;
                    send(workers[i].fd, &msg, 1, 0);
                    continue;
                }
                /* Track this chunk on the worker slot for potential re-queue. */
                workers[i].chunk_start    = wr.start;
                workers[i].chunk_count    = wr.count;
                workers[i].last_checkpoint = wr.start;

                ChunkAssign ca = { .start_idx = wr.start, .count = wr.count };
                msg = MSG_CHUNK_ASSIGN;
                send(workers[i].fd, &msg, 1, 0);
                send(workers[i].fd, &ca, sizeof(ca), 0);
                continue;
            }

            if (msg == MSG_HEARTBEAT_RESP) {
                HeartbeatResponse hb;
                if (recv_full(workers[i].fd, &hb, sizeof(hb)) <= 0) {
                    requeue_worker(i);
                    remove_worker(i);
                    continue;
                }
                workers[i].pending_heartbeat = 0;
                logprintf("[Heartbeat #%d] worker %d: delta=%lu total=%lu threads=%u rate=%.0f/s\n",
                       heartbeat_count, i, (unsigned long)hb.delta_tested, (unsigned long)hb.total_tested,
                       hb.threads_active, hb.current_rate);
                continue;
            }

            if (msg == MSG_CHECKPOINT) {
                CheckpointReport cp;
                if (recv_full(workers[i].fd, &cp, sizeof(cp)) <= 0) {
                    requeue_worker(i);
                    remove_worker(i);
                    continue;
                }
                workers[i].last_checkpoint   = cp.last_completed_idx;
                workers[i].pending_heartbeat = 0;  /* checkpoint counts as proof of life */
                logprintf("[Checkpoint] Worker %d: safe resume idx=%lu\n",
                       i, (unsigned long)cp.last_completed_idx);
                continue;
            }

            if (msg == MSG_RESULT) {
                CrackResult res;
                if (recv_full(workers[i].fd, &res, sizeof(res)) <= 0) {
                    requeue_worker(i);
                    remove_worker(i);
                    continue;
                }
                /* Mark chunk done so a subsequent disconnect won't re-queue it. */
                workers[i].last_checkpoint = workers[i].chunk_start + workers[i].chunk_count;
                if (!g_found && res.found) {
                    clock_gettime(CLOCK_MONOTONIC, &t_return.start);
                    t_return.end = t_return.start;
                    g_result = res;
                    g_found = 1;
                    logprintf("\nWorker %d reported FOUND: \"%s\"\n", i, res.password);
                    broadcast_stop();
                    run = 0;
                }
                continue;
            }
        }

        /* If all workers disconnected and no recovery work is pending, exit. */
        int any_active = 0;
        for (int i = 0; i < num_workers; i++)
            if (!workers[i].disconnected && workers[i].fd >= 0) any_active = 1;
        if (num_workers > 0 && !any_active && !g_found) {
            if (recovery_count > 0)
                logprintf("All workers disconnected; %d range(s) pending — waiting for new workers\n",
                       recovery_count);
            else {
                logprintf("All workers disconnected; no password found.\n");
                run = 0;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_total.end);
    if (g_found)
        clock_gettime(CLOCK_MONOTONIC, &t_return.end);

    /* Cleanup: close all worker fds and server */
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].fd >= 0) close(workers[i].fd);
    }
    close(server);

    logprintf("\n========================================\n===== PASSWORD CRACKING RESULT =====\n========================================\n");
    if (g_result.found)
        logprintf("  Password FOUND: \"%s\"\n", g_result.password);
    else
        logprintf("  Password NOT found (search exhausted or workers disconnected)\n");

    logprintf("\n========================================\n===== TIMING BREAKDOWN =====\n========================================\n");
    logprintf("Parsing shadow file:    %10.3f ms\n", get_elapsed_ms(&t_parse));
    logprintf("Job dispatch start:     (first worker registered)\n");
    logprintf("Worker cracking time:   %10.3f ms (reported by worker)\n", g_result.worker_crack_time_ms);
    logprintf("Result return latency:  %10.3f ms\n", get_elapsed_ms(&t_return));
    logprintf("Heartbeats sent:        %10d\n", heartbeat_count);
    logprintf("Workers connected:      %10d\n", num_workers);
    logprintf("----------------------------------------\n");
    logprintf("Total elapsed time:     %10.3f ms\n", get_elapsed_ms(&t_total));
    logprintf("========================================\n");

    logprintf("\n=== CONTROLLER TERMINATED ===\n");
    log_close();
    return g_result.found ? 0 : 1;
}
