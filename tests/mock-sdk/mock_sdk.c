/*
 * Test double for libWeWorkFinanceSdk_C.so.
 *
 * Exports the same C ABI as the official SDK, so the extension can load it through the
 * "lib_path" constructor option. GetMediaData serves a local file in chunks and can inject
 * error codes and delays, which lets the tests exercise multi-chunk downloads, zero-byte
 * files, retries, time limits and resuming without WeCom credentials.
 *
 * The sdkfileid is a ';'-separated list of key=value pairs:
 *   file=/path      file to serve (required)
 *   chunk=N         bytes per chunk (default 524288, the SDK's maximum)
 *   fail_at=K       chunk index that fails (default: none)
 *   fail_code=C     error code returned for that chunk (default 10001)
 *   fail_times=T    how many consecutive calls for that chunk fail (default 1)
 *   error=C         fail every call with C
 *   delay_ms=D      sleep D ms in every call before answering (a slow network: blocks
 *                   without using CPU, so max_execution_time does not notice it)
 *   no_outindex=K   chunk index K is returned unfinished but without outindexbuf
 *   log=/path       append one line per call: "<monotonic ms> <timeout> <indexbuf>"
 *
 * indexbuf/outindexbuf have the official SDK's form "Range:bytes=<start>-<end>" (the SDK
 * v3_20250205 starts with "Range:bytes=0-524287" and formats the next token with
 * "Range:bytes=%llu-%llu"); any other non-empty indexbuf returns 10000, which catches an
 * extension that does not pass outindexbuf back verbatim.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct WeWorkFinanceSdk_t { int unused; } WeWorkFinanceSdk_t;

typedef struct Slice_t {
    char *buf;
    int len;
} Slice_t;

typedef struct MediaData_t {
    char *outindexbuf;
    int out_len;
    char *data;
    int data_len;
    int is_finish;
} MediaData_t;

typedef struct {
    char file[1024];
    char log[1024];
    long chunk;
    long fail_at;
    int fail_code;
    int fail_times;
    int error;
    long delay_ms;
    long no_outindex;
} mock_spec;

/* Consecutive failures served for the current (sdkfileid, indexbuf) pair */
static char last_call[2048];
static int failures_served;

static int parse_spec(const char *sdkfileid, mock_spec *spec) {
    char buf[2048];
    char *save = NULL;

    memset(spec, 0, sizeof(*spec));
    spec->chunk = 524288;
    spec->fail_at = -1;
    spec->fail_code = 10001;
    spec->fail_times = 1;
    spec->no_outindex = -1;

    if (strlen(sdkfileid) >= sizeof(buf)) {
        return -1;
    }
    strcpy(buf, sdkfileid);

    for (char *tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            return -1;
        }
        *eq = '\0';
        const char *key = tok, *val = eq + 1;

        if (strcmp(key, "file") == 0) {
            snprintf(spec->file, sizeof(spec->file), "%s", val);
        } else if (strcmp(key, "log") == 0) {
            snprintf(spec->log, sizeof(spec->log), "%s", val);
        } else if (strcmp(key, "chunk") == 0) {
            spec->chunk = atol(val);
        } else if (strcmp(key, "fail_at") == 0) {
            spec->fail_at = atol(val);
        } else if (strcmp(key, "fail_code") == 0) {
            spec->fail_code = atoi(val);
        } else if (strcmp(key, "fail_times") == 0) {
            spec->fail_times = atoi(val);
        } else if (strcmp(key, "error") == 0) {
            spec->error = atoi(val);
        } else if (strcmp(key, "delay_ms") == 0) {
            spec->delay_ms = atol(val);
        } else if (strcmp(key, "no_outindex") == 0) {
            spec->no_outindex = atol(val);
        }
    }

    return (spec->file[0] && spec->chunk > 0) ? 0 : -1;
}

static void log_call(const mock_spec *spec, const char *indexbuf, int timeout) {
    if (!spec->log[0]) {
        return;
    }
    FILE *fp = fopen(spec->log, "a");
    if (!fp) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(fp, "%lld %d %s\n", (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, timeout, indexbuf);
    fclose(fp);
}

WeWorkFinanceSdk_t *NewSdk(void) {
    return calloc(1, sizeof(WeWorkFinanceSdk_t));
}

int Init(WeWorkFinanceSdk_t *sdk, const char *corpid, const char *secret) {
    (void)sdk; (void)corpid; (void)secret;
    return 0;
}

void DestroySdk(WeWorkFinanceSdk_t *sdk) {
    free(sdk);
}

int GetChatData(WeWorkFinanceSdk_t *sdk, unsigned long long seq, unsigned int limit,
                const char *proxy, const char *passwd, int timeout, Slice_t *chat) {
    (void)sdk; (void)seq; (void)limit; (void)proxy; (void)passwd; (void)timeout; (void)chat;
    return 10000;
}

int DecryptData(const char *key, const char *msg, Slice_t *out) {
    (void)key; (void)msg; (void)out;
    return 10000;
}

Slice_t *NewSlice(void) {
    return calloc(1, sizeof(Slice_t));
}

void FreeSlice(Slice_t *slice) {
    if (slice) {
        free(slice->buf);
        free(slice);
    }
}

MediaData_t *NewMediaData(void) {
    return calloc(1, sizeof(MediaData_t));
}

void FreeMediaData(MediaData_t *media) {
    if (media) {
        free(media->outindexbuf);
        free(media->data);
        free(media);
    }
}

int GetMediaData(WeWorkFinanceSdk_t *sdk, const char *indexbuf, const char *sdkfileid,
                 const char *proxy, const char *passwd, int timeout, MediaData_t *media) {
    (void)sdk; (void)proxy; (void)passwd;
    mock_spec spec;
    long offset = 0, range_end = -1;

    if (!indexbuf || !sdkfileid || !media || parse_spec(sdkfileid, &spec) != 0) {
        return 10000;
    }
    log_call(&spec, indexbuf, timeout);
    if (spec.delay_ms > 0) {
        usleep((useconds_t)(spec.delay_ms * 1000));
    }
    if (spec.error) {
        return spec.error;
    }
    if (indexbuf[0] != '\0') {
        if (sscanf(indexbuf, "Range:bytes=%ld-%ld", &offset, &range_end) != 2 || offset < 0 || range_end < offset) {
            return 10000;
        }
    }

    char call[sizeof(last_call)];
    snprintf(call, sizeof(call), "%s|%s", sdkfileid, indexbuf);
    if (strcmp(call, last_call) != 0) {
        snprintf(last_call, sizeof(last_call), "%s", call);
        failures_served = 0;
    }
    if (spec.fail_at >= 0 && offset == spec.fail_at * spec.chunk && failures_served < spec.fail_times) {
        failures_served++;
        return spec.fail_code;
    }

    int fd = open(spec.file, O_RDONLY);
    if (fd < 0) {
        return 10005;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return 10003;
    }

    long remaining = (long)st.st_size - offset;
    long len = remaining < spec.chunk ? remaining : spec.chunk;
    if (range_end >= 0 && range_end - offset + 1 < len) {
        len = range_end - offset + 1;
    }
    if (len < 0) {
        close(fd);
        return 10000;
    }

    if (len > 0) {
        media->data = malloc((size_t)len);
        if (!media->data || pread(fd, media->data, (size_t)len, offset) != len) {
            close(fd);
            return 10003;
        }
    }
    close(fd);

    media->data_len = (int)len;
    media->is_finish = (offset + len >= (long)st.st_size);
    if (!media->is_finish && spec.no_outindex >= 0 && offset == spec.no_outindex * spec.chunk) {
        return 0;
    }
    if (!media->is_finish) {
        char next[64];
        int n = snprintf(next, sizeof(next), "Range:bytes=%ld-%ld", offset + len, offset + len + spec.chunk - 1);
        media->outindexbuf = strdup(next);
        media->out_len = n;
    }

    return 0;
}
