# Password Cracking System — Design and Implementation Report

## Design

### Overall Architecture

The system is a **distributed password-cracking** application with a **controller–worker** design. One controller process coordinates multiple worker processes over TCP. The controller holds the cracking job (target hash and algorithm), divides the candidate space into chunks, and assigns chunks to workers on demand. Workers connect as clients, receive the job, repeatedly request chunks, crack their assigned ranges in parallel using multiple threads, and report results or heartbeats back to the controller.

**Components:**

- **Controller** — TCP server that parses the target (e.g. from a shadow file), accepts worker connections, assigns work chunks, collects results, and sends periodic heartbeat requests.
- **Worker** — TCP client that connects to the controller, receives one job, requests chunks, cracks each chunk with multiple threads using `crypt_r`, and sends either a found result or heartbeat responses.
- **gen_hash** — Standalone utility that hashes a given password with a given salt prefix (for generating test hashes); not part of the distributed run.

**Protocol:** All communication is message-based. Each message starts with a one-byte type (`MSG_REGISTER`, `MSG_JOB`, `MSG_RESULT`, `MSG_HEARTBEAT_REQ`, `MSG_HEARTBEAT_RESP`, `MSG_REQUEST_CHUNK`, `MSG_CHUNK_ASSIGN`, `MSG_CHECKPOINT`, `MSG_STOP`), followed by any payload (e.g. `CrackJob`, `ChunkAssign`, `CrackResult`, `HeartbeatResponse`, `CheckpointReport`). Shared types and constants are in `header.h`.

**Search space:** Passwords are 1, 2, 3, or 4 characters from printable ASCII codes 33–111 (79 characters). The total candidate count is 79 + 79² + 79³ + 79⁴. The space is linearized into a single index range; chunks are contiguous subranges of that index space.

---

### Controller and Worker Responsibilities

#### Controller

- **Job setup:** Reads a shadow-style file and target username; parses the stored hash string to detect algorithm (MD5, bcrypt, SHA-256, SHA-512, yescrypt), and extracts salt and target hash into a `CrackJob` sent to every worker.
- **Server:** Binds to a configurable port, listens for worker connections, and maintains a fixed-size set of worker slots (`MAX_WORKERS`).
- **Registration:** Accepts connections; each new connection is expected to send `MSG_REGISTER` first. Once registered, the controller sends `MSG_JOB` plus the full `CrackJob`.
- **Chunk assignment:** Tracks `next_chunk_start` over the global candidate range. On `MSG_REQUEST_CHUNK`, if the password is not yet found and work remains, it sends `MSG_CHUNK_ASSIGN` with `start_idx` and `count` (up to the configured chunk size), then advances `next_chunk_start`. If no work is left or the password was already found, it sends `MSG_STOP` instead.
- **Heartbeats:** Uses `select()` with a timeout (heartbeat interval). When the timeout fires, sends `MSG_HEARTBEAT_REQ` to all registered workers; on `MSG_HEARTBEAT_RESP` receives and logs `HeartbeatResponse` (delta/total tested, threads active, current rate).
- **Result handling:** On `MSG_RESULT`, receives a `CrackResult`. If `found` is set, stores the result, broadcasts `MSG_STOP` to all workers, and exits the main loop.
- **Checkpointing:** Each worker slot tracks `last_checkpoint`, the last confirmed-safe global candidate index received from that worker. When a chunk is first assigned, `last_checkpoint` is set to `chunk_start`. On each `MSG_CHECKPOINT`, it is updated to `cp.last_completed_idx` and the worker's pending-heartbeat flag is cleared (a checkpoint counts as proof of life). When a worker is declared dead (missed heartbeat), `requeue_worker` pushes the range `[last_checkpoint, chunk_start + chunk_count)` onto a recovery queue so only the unconfirmed tail of the chunk is retried; confirmed work is not duplicated.
- **Cleanup and output:** Closes all worker sockets and the server, then prints the final result (password found or not), timing breakdown (parse, worker-reported crack time, result-return latency, total), heartbeat count, and number of workers connected.

#### Worker

- **Connection:** Parses `-c controller_host -p port -t num_threads`; resolves the host, connects via TCP, and sends `MSG_REGISTER`.
- **Job reception:** Expects `MSG_JOB` then receives the full `CrackJob`. Builds a hash-format string from `job.algorithm` and `job.salt` (e.g. `$1$salt$` for MD5) for use with `crypt_r`.
- **Reader thread:** Dedicated thread that reads messages from the socket. Handles `MSG_HEARTBEAT_REQ` (replies with `MSG_HEARTBEAT_RESP` and current `HeartbeatResponse`), `MSG_STOP` (sets a stop flag and signals the main thread), and `MSG_CHUNK_ASSIGN` (reads `ChunkAssign` and signals the main thread that a chunk is ready). Socket errors or closure set a “reader done” flag and signal the main thread.
- **Main loop:** Sends `MSG_REQUEST_CHUNK`; then waits (with condition variable) for either a chunk or stop/done. If a chunk is received with `count > 0`, calls `crack_chunk(chunk_start, count)`; if the password is found, sends `MSG_RESULT` with the password and worker crack time and exits. If stop or “no more work” (chunk with `count == 0`) is received, sends `MSG_RESULT` with not-found and exits.
- **Checkpointing:** Before cracking a chunk, the worker stores `g_current_chunk_start` and resets `g_last_checkpoint_sent` to 0. During cracking, each thread calls `try_send_checkpoint()` after every candidate. That function loads the atomic `g_tested_in_chunk` counter and checks whether it has crossed the next multiple of `g_checkpoint_interval`. One thread wins a CAS on `g_last_checkpoint_sent` and sends `MSG_CHECKPOINT` with `last_completed_idx = g_current_chunk_start + safe_done`; all other threads lose the CAS and return immediately, keeping checkpoint sends lock-free and non-duplicated.
- **Cracking:** `crack_chunk` spawns `num_threads` threads. Each thread iterates over the chunk range with stride `num_threads` (thread `i` takes indices `chunk_start + i`, `chunk_start + i + num_threads`, …). For each index, it converts the index to a candidate password with `idx_to_pw`, hashes with `crypt_r`, compares the hash to the target (hash part only), and increments a global “tested” count. On match or on global stop flag, threads exit. The main thread joins them and returns whether a password was found.

---

## Implementation

### Language and Libraries Used

- **Language:** C (C11-style, with `_GNU_SOURCE` / `_POSIX_C_SOURCE` where needed).
- **Build:** GCC; `-Wall -Wextra -O2`; controller is built without extra libraries; worker and gen_hash link `libxcrypt` (or system `libcrypt`) and worker also links `-pthread`.
- **Libraries / APIs:**
  - **Networking:** POSIX sockets (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`), `connect`, `send`, `recv`, `accept`, `bind`, `listen`, `select`, `gethostbyname`.
  - **Hashing:** `crypt.h` and `crypt_r()` (from libxcrypt or glibc) for all algorithms (MD5, bcrypt, SHA-256, SHA-512, yescrypt).
  - **Concurrency (worker):** `pthread.h` (threads, mutex, condition variable); `stdatomic.h` for shared flags and counters (`g_found`, `g_stop_requested`, `g_tested`, `g_last_reported`, `g_threads_active`).
  - **Time:** `time.h`, `clock_gettime(CLOCK_MONOTONIC)` for elapsed times.
- **Shared definitions:** `header.h` provides message enums, algorithm enums, search-space constants (`PW_CMIN`, `PW_CMAX`, `PW_MAX_LEN`, `TOTAL_CANDIDATES`), and packed structs (`CrackJob`, `CrackResult`, `HeartbeatResponse`, `ChunkAssign`, `Timer`).

---

### Algorithm Detection and Hash Verification

**Controller (algorithm detection and job construction):** Implemented in `parse_shadow()`. The shadow line is split on `:` to get username and hash. The hash must start with `$`. The first two `$` delimiters are found; the substring between the first `$` and the first delimiter is the algorithm id:

- `"1"` → `ALGO_MD5`
- `"2..."` → `ALGO_BCRYPT`
- `"5"` → `ALGO_SHA256`
- `"6"` → `ALGO_SHA512`
- `"y"` → `ALGO_YESCRYPT`

For **bcrypt**, the salt sent to the worker includes the rounds and the 22-character salt (so the worker can pass a full `$2b$...` prefix to `crypt_r`); the target hash is the 31-character part after that. For **yescrypt**, salt is everything from the algorithm id up to the third `$`; the rest is the target hash. For **MD5, SHA-256, SHA-512**, salt is between the first and second `$`, and the target hash is after the second `$`. Any newline in the copied target hash is stripped.

**Worker (hash verification):** The worker builds a format string `fmt` from `job.algorithm` and `job.salt` (e.g. `$1$salt$`, `$5$salt$`). For each candidate password it calls `crypt_r(pw, fmt, &cd)`. The full returned string is of the form `$algo$salt$hash`. The worker skips to the third `$` (i.e. the hash part only) and compares that to `job.target_hash` with `strcmp`. This avoids depending on the exact salt formatting in the crypt output and matches the stored hash value.

---

### Candidate Password Generation

Candidate generation is **index-based** and **deterministic**: the global candidate space is ordered by length (1-, then 2-, then 3-, then 4-character passwords), and within each length by lexicographic order over the 79 characters (ASCII 33–111).

**Offsets (from `worker.c`):**

- Length 1: indices `[0, 79)`
- Length 2: `[79, 6320)`   (79 + 79²)
- Length 3: `[6320, 499359)` (79 + 79² + 79³)
- Length 4: `[499359, TOTAL_CANDIDATES)` (79 + 79² + 79³ + 79⁴)

**`idx_to_pw(idx, pw)`:** Given a global index, it determines the length from the above ranges, then treats the index within that length as a base-79 digit string (least significant “digit” is the rightmost character). For length 1, `pw[0] = CMIN + idx`. For longer lengths, the index is adjusted by the start of that length’s range and repeatedly divided by 79; remainders give characters from right to left, stored in `pw[]` with a null terminator. This produces a unique password for each index and covers the entire search space without duplicates.

---

### Checkpointing and Fault Tolerance

Checkpointing allows the controller to recover from worker failures without re-doing confirmed work.

**Configuration:** The controller accepts `-k <checkpoint_attempts>` (e.g. `-k 25000`). This value is embedded in `CrackJob.checkpoint_interval` and sent to every worker at job start.

**Worker side — `try_send_checkpoint()`:** Before cracking each chunk, the worker records `g_current_chunk_start` and resets the atomic `g_last_checkpoint_sent` to 0. After hashing each candidate, every cracking thread calls `try_send_checkpoint()`. The function reads the atomic `g_tested_in_chunk` counter and computes whether it has crossed the next multiple of `checkpoint_interval`. If so, one thread wins a compare-and-swap on `g_last_checkpoint_sent` (setting it to the current tested count) and sends `MSG_CHECKPOINT` with `last_completed_idx = g_current_chunk_start + safe_done`. Threads that lose the CAS return immediately, ensuring exactly one checkpoint message is emitted per boundary crossing without any lock contention.

**Controller side — `requeue_worker()`:** Each worker slot stores `last_checkpoint`, the last `last_completed_idx` received from that worker. It is initialised to `chunk_start` when a chunk is assigned, updated on every `MSG_CHECKPOINT`, and also set to `chunk_start + chunk_count` when a worker reports a clean completion. A `MSG_CHECKPOINT` additionally clears the pending-heartbeat flag, so a worker that is checkpointing frequently is never falsely declared dead. When a worker misses a heartbeat and is removed, `requeue_worker` pushes the range `[last_checkpoint, chunk_start + chunk_count)` onto a recovery queue. The next worker that requests a chunk receives this recovery range first, so only the unconfirmed tail of the dead worker's chunk is retried.

---

### Error Handling and Correctness Checks

**Controller:**

- Shadow file: `fopen` failure, missing user, hash not starting with `$`, or insufficient `$` delimiters cause an error return. Unknown algorithm id is rejected. Salt/hash length checks (e.g. bcrypt minimum length, `MAX_SALT_LEN` / `MAX_HASH_LEN`) prevent buffer overflows.
- Socket/server: `bind`/`listen` failures exit with an error. `select` errors (other than `EINTR`) break the loop. New connections beyond `MAX_WORKERS` are closed and not added.
- Workers: Invalid or unexpected message from an unregistered worker (e.g. not `MSG_REGISTER`) leads to `remove_worker`. Incomplete or failed `recv_full` on heartbeat or result also removes that worker. Disconnected workers are detected by `recv` ≤ 0 and removed.

**Worker:**

- Arguments: Missing or invalid `-c`/`-p`/`-t` (e.g. port ≤ 0 or threads ≤ 0) print usage and exit.
- Network: `gethostbyname` and `connect` failures are reported and the process exits. Expected `MSG_JOB` and full `CrackJob`; on failure the socket is closed and the process exits. Unsupported algorithm in the job causes an error and exit.
- Cracking: `crypt_r` returning `NULL` is skipped (no crash). Found password is copied under `g_password_lock` with `strncpy` and null-termination to avoid overflow. Chunk allocation failure in `crack_chunk` frees any allocated memory and returns -1 (caller can treat as failure).
- Concurrency: Shared state is protected by atomics (`g_found`, `g_stop_requested`, `g_tested`, `g_threads_active`, `g_last_reported`) or mutex (`g_password_lock`, reader `mutex`/`cond`). Reader thread signals the main thread only after writing `pending_chunk` or setting flags, so the main thread does not use stale or partial chunk data.

**Correctness:**

- Each candidate index is assigned to exactly one chunk and one worker; chunks are disjoint and cover the range up to `TOTAL_CANDIDATES` (last chunk may be shorter). Once a worker finds the password, it reports once and exits; the controller broadcasts `MSG_STOP` so other workers stop and do not overwrite the result. The first (and only) stored result is the one printed by the controller.

---

*End of report*
