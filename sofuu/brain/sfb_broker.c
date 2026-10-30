/*
 * sfb_broker.c — single-writer broker for multi-agent use (W2.4)
 *
 * The brain is owned by one process (`sfb serve`). Other processes connect to a
 * UNIX domain socket and issue binary-framed requests. This removes all flock
 * contention between agents: exactly one process ever touches the store.
 *
 * Framing (little-endian on the host):
 *   request  : u32 op | u32 payload_len | payload[payload_len]
 *   response : i32 rc | u32 payload_len | payload[payload_len]
 *
 * Ops and payloads:
 *   SFB_OP_COUNT     (3)  payload: -                      resp: u64 live
 *   SFB_OP_REMEMBER  (1)  payload: u32 dim | f32[dim] | u32 text_len | text
 *   SFB_OP_RECALL    (2)  payload: u32 k | u32 dim | f32[dim]
 *                         resp: u32 n | [u32 id | f32 dist | f32 score | u32 tl | text]*
 *   SFB_OP_FLUSH     (4)  payload: -                      resp: u32 ok | u64 written
 *   SFB_OP_STATS     (5)  payload: -                      resp: json text
 *   SFB_OP_FORGET    (6)  payload: u32 id                 resp: -
 *
 * Payload validation (W2.4). Whatever op the client sends, the server never
 * reads a byte the client did not send: each buffer op declares its fields in a
 * fixed order, so the payload length must be *exactly* the stated size
 * (dim floats and no more, one query vector and no more), every field access is
 * bounds-checked against the declared length, and a payload with the wrong
 * number of floats is handed to the length-checking sfb_*_ex() entry point so
 * it fails with SFB_E_DIM instead of being read past its end. A frame that is
 * short, long or truncated is answered with a typed error and never reaches the
 * brain — the server survives it and keeps serving.
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"
#include "sfb_broker.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

#define SFB_OP_REMEMBER 1u
#define SFB_OP_RECALL   2u
#define SFB_OP_COUNT    3u
#define SFB_OP_FLUSH    4u
#define SFB_OP_STATS    5u
#define SFB_OP_FORGET   6u

#define SFB_BROKER_MAX_PAYLOAD (16u * 1024u * 1024u)
/* Upper bound on SFB_OP_RECALL's k. The server sizes its hit array from the
 * request, so this is a memory cap, not an array bound. */
#define SFB_BROKER_MAX_K       1024u

/* accept() waits at most this long before the stop flag is re-checked, so a
 * SIGINT/SIGTERM that lands between the loop test and accept() cannot strand
 * the process (signals alone are racy here). */
#define SFB_BROKER_ACCEPT_POLL_MS 200

/* Set while sfb_broker_run_ready() is accepting, so a read on a stalled or
 * half-open client cannot outlive a SIGINT/SIGTERM either. */
static volatile sig_atomic_t *broker_stop_flag;

static int read_all_fd(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        struct pollfd pfd;
        if (broker_stop_flag != NULL && *broker_stop_flag) return SFB_E_IO;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, SFB_BROKER_ACCEPT_POLL_MS);
        if (pr < 0) { if (errno == EINTR) continue; return SFB_E_IO; }
        if (pr == 0) continue;          /* stop flag gets another look       */
        ssize_t r = read(fd, (char *)buf + got, len - got);
        if (r == 0) return SFB_E_IO;
        if (r < 0) { if (errno == EINTR) continue; return SFB_E_IO; }
        got += (size_t)r;
    }
    return SFB_OK;
}

/* ── framing helpers ───────────────────────────────────────────────────── */
/*
 * W2.4: every byte a client sends is accounted for before any of it is read,
 * and every field is bounds-checked against the length the client declared.
 * A frame that is short, long or truncated is answered with a typed error and
 * dropped — it never reaches the brain.
 */
static int broker_reply(int fd, int32_t status, const void *payload, uint32_t rlen) {
    uint32_t hdr[2];
    if (rlen != 0 && payload == NULL) return SFB_E_NOMEM;   /* never half a frame */
    hdr[0] = (uint32_t)status;
    hdr[1] = rlen;
    if (sfb_write_all(fd, hdr, sizeof hdr) != SFB_OK) return SFB_E_IO;
    if (rlen && sfb_write_all(fd, payload, rlen) != SFB_OK) return SFB_E_IO;
    return SFB_OK;
}

/* Answer a frame that never arrived in full, then let the caller drop it: a
 * client that half-closed mid-frame gets a typed error instead of a bare EOF. */
static int broker_refuse(int fd, int32_t status) {
    (void)broker_reply(fd, status, NULL, 0u);
    return (int)status;
}

/* Read one u32 field at `off`. The subtraction is what makes this safe: an
 * offset and a length that would overflow 32 bits simply fail the check. */
static int broker_get_u32(const unsigned char *payload, uint32_t plen,
                          uint32_t off, uint32_t *out) {
    if (payload == NULL || off > plen || plen - off < 4u) return SFB_E_INVALID;
    memcpy(out, payload + off, 4);
    return SFB_OK;
}

/* handle one connection; returns when the client closes or one request is served */
static int serve_one(sfb_brain_t *b, int fd) {
    uint32_t hdr[2] = {0, 0};
    int rc = read_all_fd(fd, hdr, sizeof hdr);
    if (rc != SFB_OK) return broker_refuse(fd, SFB_E_INVALID);   /* truncated header */
    uint32_t op = hdr[0], plen = hdr[1];
    if (plen > SFB_BROKER_MAX_PAYLOAD) return broker_refuse(fd, SFB_E_INVALID);

    unsigned char *payload = plen ? malloc(plen) : NULL;
    if (plen && !payload) return broker_refuse(fd, SFB_E_NOMEM);
    if (plen) {
        rc = read_all_fd(fd, payload, plen);
        if (rc != SFB_OK) {                     /* truncated payload            */
            free(payload);
            return broker_refuse(fd, SFB_E_INVALID);
        }
    }

    unsigned char *resp = NULL;
    uint32_t rlen = 0;
    int32_t status = SFB_OK;

    switch (op) {
    case SFB_OP_COUNT: {
        uint64_t live = 0;
        status = sfb_count(b, &live);
        resp = malloc(sizeof live); if (resp) { memcpy(resp, &live, sizeof live); rlen = sizeof live; }
        break;
    }
    case SFB_OP_REMEMBER: {
        /* u32 dim | f32[dim] | u32 text_len | text  (exactly, nothing more) */
        uint32_t dim = 0, tl = 0;
        uint64_t vec_bytes, text_off;

        status = broker_get_u32(payload, plen, 0u, &dim);
        if (status != SFB_OK) break;            /* plen < 4                     */
        if (dim < SFB_MIN_DIM || dim > SFB_MAX_DIM) { status = SFB_E_INVALID; break; }
        vec_bytes = (uint64_t)dim * 4u;
        if ((uint64_t)plen < 4u + vec_bytes + 4u) { status = SFB_E_INVALID; break; }
        status = broker_get_u32(payload, plen, 4u + (uint32_t)vec_bytes, &tl);
        if (status != SFB_OK) break;
        text_off = 8u + vec_bytes;
        if ((uint64_t)plen != text_off + (uint64_t)tl) { status = SFB_E_INVALID; break; }
        char *text = malloc((size_t)tl + 1u);
        if (!text) { status = SFB_E_NOMEM; break; }
        memcpy(text, payload + text_off, tl); text[tl] = 0;
        sfb_remember_result_t on = {0, 0, 0};
        /* sfb_remember_ex(), never the sfb_remember() shim: the shim assumes the
         * caller's buffer holds the *brain's* dim floats, so a payload carrying
         * a different number of floats would be read past its end here. _ex
         * compares vec_len with cfg.dim and returns SFB_E_DIM before it touches
         * a single element. */
        status = sfb_remember_ex(b, (const float *)(payload + 4), (size_t)dim,
                                 text, "agent", &on);
        free(text);
        if (status == SFB_OK) {
            resp = malloc(sizeof on); if (resp) { memcpy(resp, &on, sizeof on); rlen = sizeof on; }
        }
        break;
    }
    case SFB_OP_RECALL: {
        /* u32 k | u32 dim | f32[dim]  (exactly one query vector) */
        uint32_t k = 0, dim = 0;
        status = broker_get_u32(payload, plen, 0u, &k);
        if (status != SFB_OK) break;
        status = broker_get_u32(payload, plen, 4u, &dim);
        if (status != SFB_OK) break;
        if (k == 0 || k > SFB_BROKER_MAX_K) { status = SFB_E_INVALID; break; }
        if (dim < SFB_MIN_DIM || dim > SFB_MAX_DIM) { status = SFB_E_INVALID; break; }
        if ((uint64_t)plen != 8u + (uint64_t)dim * 4u) { status = SFB_E_INVALID; break; }
        /* hits[] is sized by the request instead of a fixed 128: sfb_recall_ex
         * writes at most k hits, and k up to 1024 used to overrun the stack. */
        sfb_hit_t *hits = (sfb_hit_t *)calloc(k, sizeof *hits);
        if (hits == NULL) { status = SFB_E_NOMEM; break; }
        uint32_t n = 0;
        status = sfb_recall_ex(b, (const float *)(payload + 8), (size_t)dim, k, hits, &n);
        if (status != SFB_OK) { free(hits); break; }
        if (n > k) n = k;
        size_t need = 4;
        for (uint32_t i = 0; i < n; i++) need += 16 + (hits[i].text ? strlen(hits[i].text) : 0);
        resp = malloc(need);
        if (resp) {
            unsigned char *w = resp; memcpy(w, &n, 4); w += 4;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t tl = hits[i].text ? (uint32_t)strlen(hits[i].text) : 0;
                memcpy(w, &hits[i].id, 4); w += 4;
                memcpy(w, &hits[i].distance, 4); w += 4;
                memcpy(w, &hits[i].score, 4); w += 4;
                memcpy(w, &tl, 4); w += 4;
                if (tl) { memcpy(w, hits[i].text, tl); w += tl; }
            }
            rlen = (uint32_t)(w - resp);
        }
        free(hits);
        break;
    }
    case SFB_OP_FLUSH: {
        sfb_flush_result_t fr; memset(&fr, 0, sizeof fr);
        status = sfb_flush(b, &fr);
        resp = malloc(12);
        if (resp) { uint32_t okk = fr.ok; uint64_t wr = fr.records_written;
                    memcpy(resp, &okk, 4); memcpy(resp + 4, &wr, 8); rlen = 12; }
        break;
    }
    case SFB_OP_STATS: {
        sfb_stats_t st; status = sfb_stats_get(b, &st);
        char buf[512];
        int n = snprintf(buf, sizeof buf,
            "{\"records\":%llu,\"live\":%llu,\"segments\":%llu,\"bytes_on_disk\":%llu}",
            (unsigned long long)st.records, (unsigned long long)st.live,
            (unsigned long long)st.segments, (unsigned long long)st.bytes_on_disk);
        if (n > 0) { resp = malloc((size_t)n); if (resp) { memcpy(resp, buf, (size_t)n); rlen = (uint32_t)n; } }
        break;
    }
    case SFB_OP_FORGET: {
        uint32_t id = 0;
        status = broker_get_u32(payload, plen, 0u, &id);
        if (status != SFB_OK) break;
        if (plen != 4u) { status = SFB_E_INVALID; break; }   /* exact frame */
        status = sfb_forget(b, id);
        break;
    }
    default:
        status = SFB_E_INVALID;
    }

    int werr = broker_reply(fd, status, resp, rlen);
    free(resp); free(payload);
    return werr;
}

/* ── listener lifecycle ───────────────────────────────────────────────── */
static int broker_fill_addr(const char *sock_path, struct sockaddr_un *addr) {
    size_t n;
    if (sock_path == NULL || sock_path[0] == '\0') return SFB_E_INVALID;
    n = strlen(sock_path);
    if (n >= sizeof addr->sun_path) return SFB_E_INVALID;
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, sock_path, n + 1);
    return SFB_OK;
}

/* 1 = a listener answers, 0 = stale socket / no path, -1 = undecidable. */
static int broker_socket_live(const char *sock_path) {
    struct sockaddr_un addr;
    if (broker_fill_addr(sock_path, &addr) != SFB_OK) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    int err = errno;
    (void)close(fd);
    if (rc == 0) return 1;
    if (err == ECONNREFUSED || err == ENOENT) return 0;
    return -1;                      /* EACCES/ENOTSOCK/…: do not guess        */
}

int sfb_broker_probe_path(const char *sock_path) {
    struct sockaddr_un addr;
    struct stat st;
    int live;

    if (broker_fill_addr(sock_path, &addr) != SFB_OK) return SFB_E_INVALID;
    if (lstat(sock_path, &st) != 0)
        return (errno == ENOENT) ? SFB_OK : SFB_E_IO;
    if (!S_ISSOCK(st.st_mode))
        return SFB_E_INVALID;       /* someone else's file: never unlink it    */
    live = broker_socket_live(sock_path);
    if (live == 1) return SFB_E_STATE;   /* a live broker owns it — do not steal */
    if (live < 0) return SFB_E_IO;       /* cannot tell — do not steal either    */
    return SFB_OK;                  /* stale; broker_listen() reclaims it      */
}

/* Bind + listen. Returns SFB_OK with *out_fd set, or a typed error.
 *
 * Stale sockets are handled *after* a failed bind, never by an unconditional
 * unlink: only a path that refuses connections is removed, and only then is
 * bind() retried. A live listener is therefore never stolen. (fstat() cannot
 * be used to prove the path is ours: on Darwin a socket fd's st_dev/st_ino
 * live in a different namespace from the filesystem node.) */
static int broker_listen(const char *sock_path, int *out_fd) {
    struct sockaddr_un addr;
    struct stat st;
    int rc = broker_fill_addr(sock_path, &addr);
    if (rc != SFB_OK) return rc;

    if (lstat(sock_path, &st) == 0 && !S_ISSOCK(st.st_mode))
        return SFB_E_INVALID;       /* never clobber a regular file            */

    int lfd = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lfd < 0) return SFB_E_IO;
        if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0) break;
        int err = errno;
        (void)close(lfd);
        lfd = -1;
        if (err != EADDRINUSE) return SFB_E_IO;
        if (attempt == 1) return SFB_E_STATE;

        rc = broker_socket_live(sock_path);
        if (rc == 0) {
            /* Maybe a peer is between bind() and listen(); give it a moment. */
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 20L * 1000L * 1000L;
            (void)nanosleep(&ts, NULL);
            rc = broker_socket_live(sock_path);
        }
        if (rc > 0) return SFB_E_STATE;
        if (rc < 0) return SFB_E_IO;
        if (unlink(sock_path) != 0 && errno != ENOENT) return SFB_E_IO;
    }
    if (lfd < 0) return SFB_E_STATE;

    if (listen(lfd, 64) != 0) { (void)close(lfd); (void)unlink(sock_path); return SFB_E_IO; }
    (void)chmod(sock_path, 0600);
    *out_fd = lfd;
    return SFB_OK;
}

static void broker_accept_loop(sfb_brain_t *b, int lfd, volatile sig_atomic_t *stop) {
    while (!(stop && *stop)) {
        struct pollfd pfd;
        pfd.fd = lfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, SFB_BROKER_ACCEPT_POLL_MS);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;             /* stop flag gets another look     */
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        (void)serve_one(b, cfd);
        (void)close(cfd);
    }
}

int sfb_broker_run_ready(sfb_brain_t *b, const char *sock_path,
                         volatile sig_atomic_t *stop,
                         sfb_broker_ready_cb on_ready, void *ud) {
    struct sigaction ign, old_pipe;
    int restore_pipe = 0;
    if (b == NULL) return SFB_E_INVALID;

    /* A client that disconnects before reading its response must not kill the
     * server with SIGPIPE: the broker has to survive its clients. */
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGPIPE, &ign, &old_pipe) == 0) restore_pipe = 1;

    int lfd = -1;
    int rc = broker_listen(sock_path, &lfd);
    if (rc != SFB_OK) {
        if (restore_pipe) (void)sigaction(SIGPIPE, &old_pipe, NULL);
        return rc;
    }
    broker_stop_flag = stop;
    if (on_ready != NULL) on_ready(sock_path, ud);
    broker_accept_loop(b, lfd, stop);
    broker_stop_flag = NULL;
    (void)close(lfd);
    (void)unlink(sock_path);
    if (restore_pipe) (void)sigaction(SIGPIPE, &old_pipe, NULL);
    return SFB_OK;
}

/* Run the broker until `stop` is non-zero (checked between connections). */
int sfb_broker_run(sfb_brain_t *b, const char *sock_path, volatile sig_atomic_t *stop) {
    return sfb_broker_run_ready(b, sock_path, stop, NULL, NULL);
}

/* ── client side ──────────────────────────────────────────────────────── */
static int broker_call_raw(const char *sock_path, uint32_t op,
                           const void *payload, uint32_t plen,
                           void **resp_out, uint32_t *rlen_out) {
    struct sockaddr_un addr;
    if (strlen(sock_path) >= sizeof addr.sun_path) return SFB_E_INVALID;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return SFB_E_IO;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) { close(fd); return SFB_E_IO; }

    uint32_t hdr[2] = { op, plen };
    if (sfb_write_all(fd, hdr, sizeof hdr) != SFB_OK ||
        (plen && sfb_write_all(fd, payload, plen) != SFB_OK)) { close(fd); return SFB_E_IO; }

    uint32_t rhdr[2] = {0, 0};
    if (read_all_fd(fd, rhdr, sizeof rhdr) != SFB_OK) { close(fd); return SFB_E_IO; }
    int32_t rc = (int32_t)rhdr[0];
    uint32_t rlen = rhdr[1];
    void *rp = NULL;
    if (rlen) {
        rp = malloc(rlen);
        if (!rp) { close(fd); return SFB_E_NOMEM; }
        if (read_all_fd(fd, rp, rlen) != SFB_OK) { free(rp); close(fd); return SFB_E_IO; }
    }
    close(fd);
    if (resp_out) *resp_out = rp; else free(rp);
    if (rlen_out) *rlen_out = rlen;
    return rc;
}

int sfb_broker_count(const char *sock, uint64_t *live) {
    void *r = NULL; uint32_t n = 0;
    int rc = broker_call_raw(sock, SFB_OP_COUNT, NULL, 0, &r, &n);
    if (rc == SFB_OK && live && n >= 8) memcpy(live, r, 8);
    free(r); return rc;
}

int sfb_broker_remember(const char *sock, const float *vec, uint32_t dim,
                        const char *text, sfb_remember_result_t *out) {
    uint32_t tl = (uint32_t)strlen(text);
    uint32_t plen = 4 + dim * 4 + 4 + tl;
    unsigned char *p = malloc(plen);
    if (!p) return SFB_E_NOMEM;
    unsigned char *w = p;
    memcpy(w, &dim, 4); w += 4;
    memcpy(w, vec, dim * 4); w += dim * 4;
    memcpy(w, &tl, 4); w += 4;
    memcpy(w, text, tl);
    void *r = NULL; uint32_t n = 0;
    int rc = broker_call_raw(sock, SFB_OP_REMEMBER, p, plen, &r, &n);
    free(p);
    if (rc == SFB_OK && out && n >= sizeof *out) memcpy(out, r, sizeof *out);
    free(r); return rc;
}

int sfb_broker_flush(const char *sock, uint64_t *written) {
    void *r = NULL; uint32_t n = 0;
    int rc = broker_call_raw(sock, SFB_OP_FLUSH, NULL, 0, &r, &n);
    if (rc == SFB_OK && written && n >= 12) memcpy(written, (char *)r + 4, 8);
    free(r); return rc;
}
