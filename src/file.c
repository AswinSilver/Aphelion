#include "aphelion/file.h"

#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

static wchar_t *file_widen(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  text, -1, NULL, 0);
    wchar_t *wide;
    if (count == 0) {
        errno = GetLastError() == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EILSEQ;
        return NULL;
    }
    if ((size_t)count > SIZE_MAX / sizeof(*wide)) {
        errno = ENOMEM;
        return NULL;
    }
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            text, -1, wide, count) == 0) {
        int error = GetLastError() == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EILSEQ;
        free(wide);
        errno = error;
        return NULL;
    }
    return wide;
}
#endif

FILE *aphelion_file_open(const char *utf8_path, const char *mode) {
    if (utf8_path == NULL || mode == NULL) {
        errno = EINVAL;
        return NULL;
    }
#ifdef _WIN32
    {
        wchar_t *path = file_widen(utf8_path);
        wchar_t *wide_mode;
        FILE *file;
        int error;
        if (path == NULL) return NULL;
        wide_mode = file_widen(mode);
        if (wide_mode == NULL) {
            error = errno;
            free(path);
            errno = error;
            return NULL;
        }
        file = _wfopen(path, wide_mode);
        error = errno;
        free(wide_mode);
        free(path);
        errno = error;
        return file;
    }
#else
    return fopen(utf8_path, mode);
#endif
}

bool aphelion_file_is_regular(const char *utf8_path) {
    if (utf8_path == NULL) {
        errno = EINVAL;
        return false;
    }
#ifdef _WIN32
    {
        wchar_t *path = file_widen(utf8_path);
        struct _stat64 info;
        int status;
        int error;
        if (path == NULL) return false;
        status = _wstat64(path, &info);
        error = errno;
        free(path);
        errno = error;
        return status == 0 && (info.st_mode & _S_IFMT) == _S_IFREG;
    }
#else
    {
        struct stat info;
        return stat(utf8_path, &info) == 0 && S_ISREG(info.st_mode);
    }
#endif
}
