/* sfb_broker.h — multi-agent broker (W2.4). See sfb_broker.c for the framing. */
#ifndef SFB_BROKER_H
#define SFB_BROKER_H

#include "sfb.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Serve `b` on a UNIX socket until *stop becomes non-zero (may be NULL). */
int sfb_broker_run(sfb_brain_t *b, const char *sock_path, volatile sig_atomic_t *stop);

/* Readiness hook for `sfb serve`: invoked once the socket is bound, listening
 * and reachable, before the first accept(). Used to print the ready line. */
typedef void (*sfb_broker_ready_cb)(const char *sock_path, void *ud);

/* sfb_broker_run() plus the readiness callback. Same stop semantics. */
int sfb_broker_run_ready(sfb_brain_t *b, const char *sock_path,
                         volatile sig_atomic_t *stop,
                         sfb_broker_ready_cb on_ready, void *ud);

/* Pre-flight for a second server on `sock_path`:
 *   SFB_OK        no live listener (a stale socket file is reclaimed later,
 *                 safely, by sfb_broker_run*() at bind time)
 *   SFB_E_STATE   something is listening there — never unlinked
 *   SFB_E_INVALID the path exists and is not a UNIX socket, or is too long
 *   SFB_E_IO      the path cannot be inspected (errno on the caller's side)  */
int sfb_broker_probe_path(const char *sock_path);

/* Client calls. Return SFB_OK or a typed sfb error. */
int sfb_broker_count(const char *sock, uint64_t *live);
int sfb_broker_remember(const char *sock, const float *vec, uint32_t dim,
                        const char *text, sfb_remember_result_t *out);
int sfb_broker_flush(const char *sock, uint64_t *written);

#ifdef __cplusplus
}
#endif
#endif
