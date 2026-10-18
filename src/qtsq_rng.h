/*
 * QTSQ — checked CSPRNG wrapper (M-5)
 *
 * randombytes() returns 0 on success and non-zero on failure, but every
 * call site discarded that status: a broken RNG silently produced zeroed
 * salts, IVs and Shamir coefficients. Use qtsq_random_bytes() at every
 * security-relevant draw and fail closed when it returns non-zero.
 */
#ifndef QTSQ_RNG_H
#define QTSQ_RNG_H

#include <stdint.h>
#include <stddef.h>
#include "../crypto/common/randombytes.h"

static inline int qtsq_random_bytes(uint8_t *buf, size_t len) {
    return randombytes(buf, len) == 0 ? 0 : -1;
}

#endif /* QTSQ_RNG_H */
