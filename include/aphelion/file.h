#ifndef APHELION_FILE_H
#define APHELION_FILE_H

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Filenames are UTF-8 on Windows and native byte strings on POSIX. Modes are
 * passed through without changing text/binary behavior. Failures preserve the
 * filesystem errno; Windows conversion failures use EILSEQ (invalid UTF-8)
 * or ENOMEM. NULL arguments fail with EINVAL. */
FILE *aphelion_file_open(const char *utf8_path, const char *mode);
bool aphelion_file_is_regular(const char *utf8_path);

#ifdef __cplusplus
}
#endif

#endif /* APHELION_FILE_H */
