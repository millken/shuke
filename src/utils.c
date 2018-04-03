//
// Created by yangyu on 17-3-29.
//

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pwd.h>

#include <rte_byteorder.h>
#include <sys/time.h>
#include <rte_cycles.h>

#include "log.h"
#include "utils.h"
#include "zmalloc.h"

DEF_LOG_MODULE(RTE_LOGTYPE_USER1, "UTILS");

void freev(void **pp) {
    if (!pp) return;
    for (int i = 0; pp[i] != NULL; ++i) {
        free(pp[i]);
    }
    free(pp);
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
long long mstime(void) {
    return ustime()/1000;
}

static int intCmpFunc(const void *a, const void *b) {
    return (*(int*)a - *(int*)b);
}

void sortIntArray(int arr[], size_t nitems) {
    qsort(arr, nitems, sizeof(int), intCmpFunc);
}

int intArrayToStr(int arr[], int nitems, char *seps, char *buf, size_t size) {
    size_t offset = 0;
    int n = 0;

    buf[0] = '\0';
    if (nitems < 1) return -1;
    n = snprintf(buf, size, "%d", arr[0]);
    offset = (size_t)n;
    for (int i = 1; i < nitems; ++i) {
        if (offset >= (size-1)) return -1;
        n = snprintf(buf+offset, size-offset, "%s%d", seps, arr[i]);
        offset += n;
    }
    buf[offset] = '\0';
    return (int)offset;
}

int str2long(char *ss, long *v) {
    char *end = NULL;
    long lval;
    int base = 10;
    if (ss[0] == '0') base = 16;
    lval = strtol(ss, &end, base);
    if (*end == '\0') {
        *v = lval;
        return 0;
    } else {
        return -1;
    }
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lluB",n);
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    } else if (n < (1024LL*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024);
        sprintf(s,"%.2fT",d);
    } else if (n < (1024LL*1024*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024*1024);
        sprintf(s,"%.2fP",d);
    } else {
        /* Let's hope we never need this */
        sprintf(s,"%lluB",n);
    }
}

void numberToHuman(char *s, unsigned long long n) {
    double d;
    unsigned long long K = 1000;
    unsigned long long M = K * K;
    unsigned long long B = K * K * K;
    unsigned long long T = K * K * K * K;
    unsigned long long P = K * K * K * K * K;

    if (n < K) {
        /* Bytes */
        sprintf(s,"%llu",n);
    } else if (n < (1000 * K)) {
        d = (double)n/K;
        sprintf(s,"%.2fK",d);
    } else if (n < (1000 * M)) {
        d = (double)n/M;
        sprintf(s,"%.2fM",d);
    } else if (n < (1000 * B)) {
        d = (double)n/B;
        sprintf(s,"%.2fB",d);
    } else if (n < (1000 * T)) {
        d = (double)n/T;
        sprintf(s,"%.2fT",d);
    } else if (n < (1000LL* P)) {
        d = (double)n/P;
        sprintf(s,"%.2fP",d);
    } else {
        /* Let's hope we never need this */
        sprintf(s,"%llu",n);
    }
}

static long getFileSize(FILE *fp) {
    long size;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);
    return size;
}

char* readFile(const char *filename) {
    char *buf = NULL;
    long totallen, read_size;
    FILE *fp = fopen(filename, "r");

    if (fp) {
        totallen = getFileSize(fp);
        buf = (char *) malloc(sizeof(char) * (totallen + 1));
        read_size = fread(buf, sizeof(char), totallen, fp);
        buf[totallen] = '\0';
        if (totallen != read_size) {
            free(buf);
            buf = NULL;
        }
        fclose(fp);
    }
    return buf;
}

char *zreadFile(const char *filename) {
    char *buf = NULL;
    long totallen, read_size;
    FILE *fp = fopen(filename, "r");

    if (fp) {
        totallen = getFileSize(fp);
        buf = (char *) zmalloc(sizeof(char) * (totallen + 1));
        read_size = fread(buf, sizeof(char), totallen, fp);
        buf[totallen] = '\0';
        if (totallen != read_size) {
            zfree(buf);
            buf = NULL;
        }
        fclose(fp);
    }
    return buf;
}

char* getHomePath(void)
{
    char *home = getenv("HOME");
    if (home) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw->pw_dir;
}

char *toAbsPath(char *p, char *rootp) {
    char buf[4096] = "";
    char root[1024];
    if (rootp == NULL) {
        if (getcwd(root, 1024) == NULL) return NULL;
    } else {
        snprintf(root, 1024, "%s", rootp);
    }

    char *end = p + strlen(p) - 1;
    char *ptr = p;
    if (*p == '/') return strdup(p);
    if (*p == '~') {
        char *home = getHomePath();
        p += 2;
        snprintf(buf, 4096, "%s/%s", home, p);
        return strdup(buf);
    }

    if (*end == '/') *end = 0;
    if (*ptr == '.') {
        while (*ptr == '.') {
            if (*ptr == '.' && *(ptr+1) == '.') {
                end = strrchr(root, '/');
                *end = 0;
                if (*(ptr+2) == '/') {
                    ptr += 3;
                }
            } else {
                if (*(ptr+1) == '/') {
                    ptr += 2;
                }
            }
            snprintf(buf, 4096, "%s/%s", root, ptr);
        }
    } else {
        snprintf(buf, 4096, "%s/%s", root, p);
    }
    return strdup(buf);
}

/*!
 * get length of the domain in <len label> format(excluding the terminating null byte)
 * @param domain : the domain name in <len label> format.
 * @return the length of the domain
 */
size_t lenlabellen(char *domain) {
    char *ptr;
    for (ptr = domain; *ptr != 0; ptr += (*ptr+1)) ;
    return ptr - domain;
}

/*!
 * this function will dump all the arguments to buf, the format is specified by fmt.
 * it is similar to the struct package in python.
 *
 * @param buf: buffer used to store bytes
 * @param offset: the start position where the new data should stays.
 * @param size: the total size of buffer
 * @param fmt: the format of the arguments, format used to specify the byte order and data size
 *        byte order:
 *             1. =(native endian)
 *             2. >(big endian)
 *             3. <(little endian)
 *        data size:
 *             1. b(byte)      1 byte
 *             2. h(short)     2 bytes
 *             3. i(int)       4 bytes
 *             4. q(long long) 8 bytes
 *             5. s(string)    using strlen(s)+1 to get length
 *             6. m(memory)    an extra argument is needed to provide the length
 *       pls note if byte order is ignored, then it will use the previous byte order.
 * @param ...
 * @return -1 when the buffer size is not enough, otherwise return the new offset.
 */
int snpack(char *buf, int offset, size_t size, char const *fmt, ...) {
    char *ptr = buf + offset;
    const char *f = fmt;
    size_t remain = size - offset;
    int result;

    uint8_t  u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    char *ss;
    size_t mem_len;

    va_list ap;
    va_start(ap, fmt);

    bool need_rev = false;
    while(*f) {
        switch(*f) {
            case '=':
                need_rev = false;
                f++;
                break;
            case '<':  // little endian
#if (BYTE_ORDER == BIG_ENDIAN)
                need_rev = true;
#else
                need_rev = false;
#endif
                f++;
                break;
            case '>':  // big endian
            case '!':
#if (BYTE_ORDER == LITTLE_ENDIAN)
                need_rev = true;
#else
                need_rev = false;
#endif
                f++;
                break;
            default:
                break;
        }
        switch(*f) {
            case 'b':
            case 'B':
                if (unlikely(remain < 1)) goto error;
                u8 = (uint8_t)va_arg(ap, int);
                *ptr++ = u8;
                remain--;
                break;
            case 'h':  //signed short
            case 'H':
                if (unlikely(remain < 2)) goto error;
                u16 = (uint16_t )va_arg(ap, int);
                if (need_rev) u16 = rte_bswap16(u16);
                *((uint16_t*)ptr) = u16;
                ptr += 2;
                remain -= 2;
                break;
            case 'i':
            case 'I':
                if (unlikely(remain < 4)) goto error;
                u32 = (uint32_t )va_arg(ap, int);
                if (need_rev) u32 = rte_bswap32(u32);
                *((uint32_t*)ptr) = u32;
                ptr += 4;
                remain -= 4;
                break;
            case 'q':
            case 'Q':
                if (unlikely(remain < 8)) goto error;
                u64 = (uint64_t )va_arg(ap, long long);
                if (need_rev) u64 = rte_bswap64(u64);
                *((uint64_t*)ptr) = u64;
                ptr += 8;
                remain -= 4;
                break;
            case 's':
            case 'S':
                ss = va_arg(ap, char *);
                size_t ss_len = strlen(ss) + 1;
                if (unlikely(remain < ss_len)) goto error;
                rte_memcpy(ptr, ss, ss_len);
                ptr += ss_len;
                remain -= ss_len;
                break;
            case 'm':
            case 'M':
                ss = va_arg(ap, char *);
                mem_len = va_arg(ap, size_t);
                if (unlikely(remain < mem_len)) goto error;
                if (unlikely(mem_len == 0)) break;
                rte_memcpy(ptr, ss, mem_len);
                ptr += mem_len;
                remain -= mem_len;
                break;
            default:
                LOG_FATAL("BUG: unknown format %s", fmt);
        }
        f++;
    }
    result = (int)(size-remain);
    goto ok;
error:
    result = -1;
ok:
    va_end(ap);
    return result;
}
