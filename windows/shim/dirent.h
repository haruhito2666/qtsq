/* dirent.h — QTSQ Windows shim (FindFirstFileA/FindNextFileA backed).
 *
 * The only consumer is qtsq_container.c, which uses: opendir, readdir,
 * closedir, `ent->d_name` and skips "." / ".." itself (container.c:300-340).
 * No d_type usage anywhere (verified by census), so a d_name-only
 * `struct dirent` is sufficient. */
#ifndef QTSQ_SHIM_DIRENT_H
#define QTSQ_SHIM_DIRENT_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qtsq_win_DIR DIR;

struct dirent {
    char d_name[1024];
};

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* QTSQ_SHIM_DIRENT_H */
