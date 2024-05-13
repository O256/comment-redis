/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __SDS_H
#define __SDS_H

#define SDS_MAX_PREALLOC (1024*1024) // 最大预分配长度
extern const char *SDS_NOINIT;

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

// sds是一个字符串结构体，包含了字符串的长度、容量、类型等信息
typedef char *sds;

/* Note: sdshdr5 is never used, we just access the flags byte directly.
 * However is here to document the layout of type 5 SDS strings. */
struct __attribute__ ((__packed__)) sdshdr5 {
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; // 使用的长度
    uint8_t alloc; // 分配的长度
    unsigned char flags; // 3个低位保存类型，5个高位未使用
    char buf[]; // 字符串内容
};
struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7  // 后三位用来保存类型
#define SDS_TYPE_BITS 3
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T))); // 获取sds头部
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T)))) // 获取sds头部
#define SDS_TYPE_5_LEN(f) ((f)>>SDS_TYPE_BITS)  // 高5位保存长度，低3位保存类型

// 返回sds使用的长度
static inline size_t sdslen(const sds s) { // 这里的s指向的是字符串的首地址
    unsigned char flags = s[-1]; // sds结构体的第一个字节保存了flags，所以这里取s[-1]
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags); // 取高5位，即长度
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->len;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->len;
    }
    return 0;
}

// 返回sds可用的长度
static inline size_t sdsavail(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            return 0;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}

// 设置sds的长度
static inline void sdssetlen(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len = newlen;
            break;
    }
}

// 增加sds的长度
static inline void sdsinclen(sds s, size_t inc) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                unsigned char newlen = SDS_TYPE_5_LEN(flags)+inc;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len += inc;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len += inc;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len += inc;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len += inc;
            break;
    }
}

// 返回sds的总长度
/* sdsalloc() = sdsavail() + sdslen() */
static inline size_t sdsalloc(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->alloc;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->alloc;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->alloc;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->alloc;
    }
    return 0;
}

// 设置sds的总长度
static inline void sdssetalloc(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->alloc = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->alloc = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->alloc = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->alloc = newlen;
            break;
    }
}

sds sdsnewlen(const void *init, size_t initlen); // 创建一个sds，长度为initlen，内容为init
sds sdstrynewlen(const void *init, size_t initlen); // 创建一个sds，尝试分配initlen长度的空间
sds sdsnew(const char *init); // 创建一个sds，内容为init
sds sdsempty(void); // 创建一个空的sds
sds sdsdup(const sds s); // 复制一个sds
void sdsfree(sds s); // 释放一个sds
sds sdsgrowzero(sds s, size_t len); // 扩展sds的长度，新的部分用0填充
sds sdscatlen(sds s, const void *t, size_t len); // 将t的内容追加到sds的末尾
sds sdscat(sds s, const char *t); // 将t的内容追加到sds的末尾
sds sdscatsds(sds s, const sds t); // 将t的内容追加到sds的末尾
sds sdscpylen(sds s, const char *t, size_t len); // 将t的内容复制到sds
sds sdscpy(sds s, const char *t); // 将t的内容复制到sds

sds sdscatvprintf(sds s, const char *fmt, va_list ap); // 格式化字符串追加到sds
#ifdef __GNUC__ // gcc特有的属性，用来检查参数是否符合printf的格式
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...); // 格式化字符串追加到sds
#endif

sds sdscatfmt(sds s, char const *fmt, ...); // 格式化字符串追加到sds
sds sdstrim(sds s, const char *cset); // 去除sds中的cset字符
void sdssubstr(sds s, size_t start, size_t len); // 截取sds的一部分
void sdsrange(sds s, ssize_t start, ssize_t end); // 截取sds的一部分
void sdsupdatelen(sds s); // 更新sds的长度
void sdsclear(sds s); // 清空sds
int sdscmp(const sds s1, const sds s2); // 比较两个sds
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count); // 以sep为分隔符，将s分割成多个sds
void sdsfreesplitres(sds *tokens, int count); // 释放sdssplitlen返回的sds数组
void sdstolower(sds s); // 将sds中的字符转换为小写
void sdstoupper(sds s); // 将sds中的字符转换为大写
sds sdsfromlonglong(long long value); // 将long long类型的整数转换为sds
sds sdscatrepr(sds s, const char *p, size_t len); // 将p的内容追加到sds，转义特殊字符
sds *sdssplitargs(const char *line, int *argc); // 将line分割成多个sds
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen); // 将sds中的from字符替换为to字符
sds sdsjoin(char **argv, int argc, char *sep); // 将argv中的字符串用sep连接起来
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen); // 将argv中的sds用sep连接起来
int sdsneedsrepr(const sds s); // 判断sds是否需要转义

/* Callback for sdstemplate. The function gets called by sdstemplate
 * every time a variable needs to be expanded. The variable name is
 * provided as variable, and the callback is expected to return a
 * substitution value. Returning a NULL indicates an error.
 */
typedef sds (*sdstemplate_callback_t)(const sds variable, void *arg);
sds sdstemplate(const char *template, sdstemplate_callback_t cb_func, void *cb_arg);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen); // 为sds扩容
sds sdsMakeRoomForNonGreedy(sds s, size_t addlen); // 为sds扩容，不使用贪心算法
void sdsIncrLen(sds s, ssize_t incr); // 增加sds的长度
sds sdsRemoveFreeSpace(sds s, int would_regrow); // 移除sds的空闲空间
sds sdsResize(sds s, size_t size, int would_regrow); // 调整sds的大小
size_t sdsAllocSize(sds s); // 返回sds的总长度
void *sdsAllocPtr(sds s); // 返回sds的指针

/* Export the allocator used by SDS to the program using SDS.
 * Sometimes the program SDS is linked to, may use a different set of
 * allocators, but may want to allocate or free things that SDS will
 * respectively free or allocate. */
void *sds_malloc(size_t size); // 分配内存
void *sds_realloc(void *ptr, size_t size); // 重新分配内存
void sds_free(void *ptr); // 释放内存

#ifdef REDIS_TEST
int sdsTest(int argc, char *argv[], int flags);
#endif

#endif
