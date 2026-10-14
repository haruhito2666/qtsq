/* strings.h — QTSQ Windows shim.
 * strcasecmp/strncasecmp map to _stricmp/_strnicmp via qtsq_win_compat.h.
 * (Keep the non-underscore names available on MinGW too by not assuming the
 * CRT exports them as functions — the macros in compat.h always win.) */
#ifndef QTSQ_SHIM_STRINGS_H
#define QTSQ_SHIM_STRINGS_H
#include "qtsq_win_compat.h"
#endif /* QTSQ_SHIM_STRINGS_H */
