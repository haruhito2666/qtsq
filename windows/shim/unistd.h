/* unistd.h — QTSQ Windows shim.
 * The POSIX function surface (access, chmod, unlink, fileno, fsync→_commit,
 * mkdir, sysconf, R_OK/W_OK/F_OK) is mapped by qtsq_win_compat.h, which is
 * force-included before any TU reaches here. */
#ifndef QTSQ_SHIM_UNISTD_H
#define QTSQ_SHIM_UNISTD_H
#include "qtsq_win_compat.h"
#endif /* QTSQ_SHIM_UNISTD_H */
