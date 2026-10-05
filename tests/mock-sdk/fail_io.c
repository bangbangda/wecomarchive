/*
 * LD_PRELOAD shim for fault injection in tests/test_mock.php.
 *
 * WECOM_FAIL_IO=fsync  fsync() fails with EIO
 * WECOM_FAIL_IO=close  close() / fclose() close the file, then report EIO (as NFS may on a
 *                      deferred write error). fclose() is covered because php_stream_sync()
 *                      (PHP >= 8.1) turns the stream into a FILE*, which glibc closes internally.
 *
 * Only descriptors of saveMediaData() temporary files ("*.part-*") are affected.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int is_target(const char *mode, int fd) {
    const char *want = getenv("WECOM_FAIL_IO");
    char link[64], path[4096];
    ssize_t n;

    if (!want || strcmp(want, mode) != 0) {
        return 0;
    }
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0) {
        return 0;
    }
    path[n] = '\0';
    return strstr(path, ".part-") != NULL;
}

int fsync(int fd) {
    static int (*real_fsync)(int) = NULL;
    if (!real_fsync) {
        real_fsync = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
    }
    if (is_target("fsync", fd)) {
        errno = EIO;
        return -1;
    }
    return real_fsync(fd);
}

int close(int fd) {
    static int (*real_close)(int) = NULL;
    if (!real_close) {
        real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
    }
    if (is_target("close", fd)) {
        real_close(fd);
        errno = EIO;
        return -1;
    }
    return real_close(fd);
}

int fclose(FILE *fp) {
    static int (*real_fclose)(FILE *) = NULL;
    if (!real_fclose) {
        real_fclose = (int (*)(FILE *))dlsym(RTLD_NEXT, "fclose");
    }
    /* Check before closing: the descriptor is gone afterwards */
    int fail = fp && is_target("close", fileno(fp));
    int ret = real_fclose(fp);
    if (fail) {
        errno = EIO;
        return EOF;
    }
    return ret;
}
