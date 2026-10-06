/*
 * Probe for the real SDK's GetMediaData() resume token (outindexbuf), driven by
 * tests/indexbuf-probe.php. Calls the SDK directly, without the extension, so that a token
 * from one process can be handed to another.
 *
 *   gcc -O1 -o indexbuf-probe tests/indexbuf-probe.c -ldl
 *   WECOM_CORPID=... WECOM_SECRET=... ./indexbuf-probe full <sdkfileid> <outfile>
 *       loop from "" until is_finish; one JSON line per call on stderr
 *   WECOM_CORPID=... WECOM_SECRET=... ./indexbuf-probe one <sdkfileid> <indexbuf> <outfile>
 *       a single call with the given indexbuf
 *
 * WECOM_SDK_LIB overrides the library path. stdout is left to the SDK, which prints the
 * gettoken response there: discard it.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct WeWorkFinanceSdk_t WeWorkFinanceSdk_t;
typedef struct { char *outindexbuf; int out_len; char *data; int data_len; int is_finish; } MediaData_t;
typedef WeWorkFinanceSdk_t* (*NewSdk_t)(void);
typedef int (*Init_t)(WeWorkFinanceSdk_t*, const char*, const char*);
typedef void (*DestroySdk_t)(WeWorkFinanceSdk_t*);
typedef int (*GetMediaData_t)(WeWorkFinanceSdk_t*, const char*, const char*, const char*, const char*, int, MediaData_t*);
typedef MediaData_t* (*NewMediaData_t)(void);
typedef void (*FreeMediaData_t)(MediaData_t*);

static void json_str(const char *s, int len) {
    fputc('"', stderr);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { fputc('\\', stderr); fputc(c, stderr); }
        else if (c < 0x20 || c >= 0x7f) fprintf(stderr, "\\u%04x", c);
        else fputc(c, stderr);
    }
    fputc('"', stderr);
}

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage\n"); return 2; }
    const char *corpid = getenv("WECOM_CORPID"), *secret = getenv("WECOM_SECRET");
    const char *lib = getenv("WECOM_SDK_LIB") ? getenv("WECOM_SDK_LIB") : "/usr/local/lib/libWeWorkFinanceSdk_C.so";
    if (!corpid || !secret) { fprintf(stderr, "no credentials\n"); return 2; }
    void *h = dlopen(lib, RTLD_LAZY);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    NewSdk_t NewSdk = dlsym(h, "NewSdk"); Init_t Init = dlsym(h, "Init"); DestroySdk_t DestroySdk = dlsym(h, "DestroySdk");
    GetMediaData_t GetMediaData = dlsym(h, "GetMediaData"); NewMediaData_t NewMediaData = dlsym(h, "NewMediaData");
    FreeMediaData_t FreeMediaData = dlsym(h, "FreeMediaData");
    WeWorkFinanceSdk_t *sdk = NewSdk();
    int ret = Init(sdk, corpid, secret);
    if (ret != 0) { fprintf(stderr, "{\"init_error\":%d}\n", ret); return 1; }

    const char *mode = argv[1], *fileid = argv[2];
    const char *indexbuf = strcmp(mode, "one") == 0 ? argv[3] : "";
    const char *outpath = strcmp(mode, "one") == 0 ? argv[4] : argv[3];
    FILE *out = fopen(outpath, "wb");
    if (!out) { fprintf(stderr, "fopen failed\n"); return 2; }
    char *cur = strdup(indexbuf);
    int call = 0;
    for (;;) {
        MediaData_t *m = NewMediaData();
        double t0 = now_s();
        ret = GetMediaData(sdk, cur, fileid, "", "", 30, m);
        double dt = now_s() - t0;
        fprintf(stderr, "{\"call\":%d,\"indexbuf\":", call++);
        json_str(cur, (int)strlen(cur));
        fprintf(stderr, ",\"ret\":%d,\"seconds\":%.3f", ret, dt);
        if (ret == 0) {
            fprintf(stderr, ",\"data_len\":%d,\"is_finish\":%d,\"out_len\":%d,\"outindexbuf\":", m->data_len, m->is_finish, m->out_len);
            if (m->outindexbuf) json_str(m->outindexbuf, m->out_len > 0 ? m->out_len : (int)strlen(m->outindexbuf)); else fprintf(stderr, "null");
            fprintf(stderr, ",\"outindexbuf_strlen\":%d", m->outindexbuf ? (int)strlen(m->outindexbuf) : -1);
            if (m->data_len > 0) fwrite(m->data, 1, (size_t)m->data_len, out);
        }
        fprintf(stderr, "}\n");
        int finish = (ret != 0) || m->is_finish || strcmp(mode, "one") == 0;
        if (!finish) { free(cur); cur = strdup(m->outindexbuf ? m->outindexbuf : ""); }
        FreeMediaData(m);
        if (finish) break;
    }
    fclose(out);
    free(cur);
    DestroySdk(sdk);
    return ret == 0 ? 0 : 1;
}
