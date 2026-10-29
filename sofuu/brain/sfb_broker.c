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

/* handle one connection; returns when the client closes or one request is served */
static int serve_one(sfb_brain_t *b, int fd) {
    uint32_t hdr[2] = {0, 0};
    int rc = read_all_fd(fd, hdr, sizeof hdr);
    if (rc != SFB_OK) return rc;
    uint32_t op = hdr[0], plen = hdr[1];
    if (plen > SFB_BROKER_MAX_PAYLOAD) return SFB_E_INVALID;

    unsigned char *payload = plen ? malloc(plen) : NULL;
    if (plen && !payload) return SFB_E_NOMEM;
    if (plen) { rc = read_all_fd(fd, payload, plen); if (rc != SFB_OK) { free(payload); return rc; } }

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
        if (plen < 4) { status = SFB_E_INVALID; break; }
        uint32_t dim; memcpy(&dim, payload, 4);
        if (dim < SFB_MIN_DIM || dim > SFB_MAX_DIM || plen < 4 + dim * 4 + 4) { status = SFB_E_INVALID; break; }
        const float *vec = (const float *)(payload + 4);
        uint32_t tl; memcpy(&tl, payload + 4 + dim * 4, 4);
        if (plen < 4 + dim * 4 + 4 + tl) { status = SFB_E_INVALID; break; }
        char *text = malloc(tl + 1);
        if (!text) { status = SFB_E_NOMEM; break; }
        memcpy(text, payload + 4 + dim * 4 + 4, tl); text[tl] = 0;
        sfb_remember_result_t on = {0, 0, 0};
        status = sfb_remember(b, vec, text, "agent", &on);
        free(text);
        if (status == SFB_OK) {
            resp = malloc(sizeof on); if (resp) { memcpy(resp, &on, sizeof on); rlen = sizeof on; }
        }
        break;
    }
    case SFB_OP_RECALL: {
        if (plen < 8) { status = SFB_E_INVALID; break; }
        uint32_t k, dim; memcpy(&k, payload, 4); memcpy(&dim, payload + 4, 4);
        if (k == 0 || k > 1024 || plen < 8 + dim * 4) { status = SFB_E_INVALID; break; }
        sfb_hit_t hits[128];
        uint32_t n = 0;
        status = sfb_recall(b, (const float *)(payload + 8), k, hits, &n);
        if (status != SFB_OK) break;
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
        if (plen < 4) { status = SFB_E_INVALID; break; }
        uint32_t id; memcpy(&id, payload, 4);
        status = sfb_forget(b, id);
        break;
    }
    default:
        status = SFB_E_INVALID;
    }

    int werr = SFB_OK;
    uint32_t rhdr[2] = { (uint32_t)status, rlen };
    if (sfb_write_all(fd, rhdr, sizeof rhdr) != SFB_OK) werr = SFB_E_IO;
    else if (rlen && sfb_write_all(fd, resp, rlen) != SFB_OK) werr = SFB_E_IO;

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
