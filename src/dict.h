/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0
#define DICT_ERR 1

// 字典节点
typedef struct dictEntry dictEntry; /* opaque */ // 字典节点

// 字典类型
typedef struct dict dict;

// 字典类型
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key); // 哈希函数
    void *(*keyDup)(dict *d, const void *key); // 复制键
    void *(*valDup)(dict *d, const void *obj); // 复制值
    int (*keyCompare)(dict *d, const void *key1, const void *key2); // 比较键
    void (*keyDestructor)(dict *d, void *key); // 销毁键
    void (*valDestructor)(dict *d, void *obj); // 销毁值
    int (*expandAllowed)(size_t moreMem, double usedRatio); // 是否允许扩容
    /* Flags */
    /* The 'no_value' flag, if set, indicates that values are not used, i.e. the
     * dict is a set. When this flag is set, it's not possible to access the
     * value of a dictEntry and it's also impossible to use dictSetKey(). Entry
     * metadata can also not be used. */
    unsigned int no_value:1; // 是否不使用值
    /* If no_value = 1 and all keys are odd (LSB=1), setting keys_are_odd = 1
     * enables one more optimization: to store a key without an allocated
     * dictEntry. */
    unsigned int keys_are_odd:1; // 是否键为奇数
    /* TODO: Add a 'keys_are_even' flag and use a similar optimization if that
     * flag is set. */

    /* Allow each dict and dictEntry to carry extra caller-defined metadata. The
     * extra memory is initialized to 0 when allocated. */
    size_t (*dictEntryMetadataBytes)(dict *d); // 字典节点元数据大小
    size_t (*dictMetadataBytes)(void); // 字典元数据大小
    /* Optional callback called after an entry has been reallocated (due to
     * active defrag). Only called if the entry has metadata. */
    void (*afterReplaceEntry)(dict *d, dictEntry *entry); // 替换字典节点后回调
} dictType; // 字典类型

#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1<<(exp))  // 哈希表大小
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp))-1)  // 哈希表掩码

// 字典节点, 用于保存键值对
struct dict {
    dictType *type; // 字典类型

    dictEntry **ht_table[2]; // 哈希表
    unsigned long ht_used[2]; // 哈希表已使用节点数

    long rehashidx; /* rehashing not in progress if rehashidx == -1 */ // 重哈希索引

    /* Keep small vars at end for optimal (minimal) struct padding */ // 保持小变量在最后，以便最小化结构填充
    int16_t pauserehash; /* If >0 rehashing is paused (<0 indicates coding error) */ // 暂停重哈希
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) */ // 哈希表大小指数

    void *metadata[];           /* An arbitrary number of bytes (starting at a
                                 * pointer-aligned address) of size as defined
                                 * by dictType's dictEntryBytes. */
};

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
// 字典迭代器
typedef struct dictIterator {
    dict *d; // 字典
    long index; // 索引
    int table, safe; // 表索引，是否安全
    dictEntry *entry, *nextEntry; // 当前节点，下一个节点
    /* unsafe iterator fingerprint for misuse detection. */
    unsigned long long fingerprint; // 用于检测滥用的指纹
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de); // 字典扫描函数
typedef void *(dictDefragAllocFunction)(void *ptr); // 字典碎片整理分配函数
typedef struct {
    dictDefragAllocFunction *defragAlloc; /* Used for entries etc. */
    dictDefragAllocFunction *defragKey;   /* Defrag-realloc keys (optional) */
    dictDefragAllocFunction *defragVal;   /* Defrag-realloc values (optional) */
} dictDefragFunctions; // 字典碎片整理函数

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP      2 // 初始哈希表大小指数
#define DICT_HT_INITIAL_SIZE     (1<<(DICT_HT_INITIAL_EXP)) // 初始哈希表大小，0x4

/* ------------------------------- Macros ------------------------------------*/
// 释放值
#define dictFreeVal(d, entry) do {                      \
    if ((d)->type->valDestructor)                      \
        (d)->type->valDestructor((d), dictGetVal(entry)); \
   } while(0)

// 释放键
#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d), dictGetKey(entry))

// 比较键
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d), key1, key2) : \
        (key1) == (key2))

#define dictEntryMetadataSize(d) ((d)->type->dictEntryMetadataBytes     \
                                  ? (d)->type->dictEntryMetadataBytes(d) : 0)
#define dictMetadataSize(d) ((d)->type->dictMetadataBytes               \
                             ? (d)->type->dictMetadataBytes() : 0)

#define dictHashKey(d, key) ((d)->type->hashFunction(key)) // 获取键的哈希值
#define dictSlots(d) (DICTHT_SIZE((d)->ht_size_exp[0])+DICTHT_SIZE((d)->ht_size_exp[1])) // 获取哈希表槽位数
#define dictSize(d) ((d)->ht_used[0]+(d)->ht_used[1]) // 获取字典节点数
#define dictIsRehashing(d) ((d)->rehashidx != -1) // 是否正在重哈希
#define dictPauseRehashing(d) ((d)->pauserehash++) // 暂停重哈希
#define dictResumeRehashing(d) ((d)->pauserehash--) // 恢复重哈希

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff // 如果 unsigned long 类型可以存储 64 位数字，则使用 64 位 PRNG
#define randomULong() ((unsigned long) genrand64_int64()) // 生成随机数，返回 64 位整数，采用梅森旋转算法
#else
#define randomULong() random() // 生成随机数，返回 32 位整数，采用标准库函数
#endif

typedef enum {
    DICT_RESIZE_ENABLE, // 允许字典扩容
    DICT_RESIZE_AVOID, // 避免字典扩容
    DICT_RESIZE_FORBID, // 禁止字典扩容
} dictResizeEnable; // 字典扩容选项

/* API */
dict *dictCreate(dictType *type); // 创建字典
int dictExpand(dict *d, unsigned long size); // 扩容字典
int dictTryExpand(dict *d, unsigned long size); // 尝试扩容字典
void *dictMetadata(dict *d); // 获取字典元数据
int dictAdd(dict *d, void *key, void *val); // 添加键值对
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing); // 添加键值对
void *dictFindPositionForInsert(dict *d, const void *key, dictEntry **existing); // 查找插入位置
dictEntry *dictInsertAtPosition(dict *d, void *key, void *position); // 在指定位置插入键值对
dictEntry *dictAddOrFind(dict *d, void *key); // 添加或查找键值对
int dictReplace(dict *d, void *key, void *val); // 替换键值对
int dictDelete(dict *d, const void *key);  // 删除键值对
dictEntry *dictUnlink(dict *d, const void *key); // 解除键值对
void dictFreeUnlinkedEntry(dict *d, dictEntry *he); // 释放解除的键值对
dictEntry *dictTwoPhaseUnlinkFind(dict *d, const void *key, dictEntry ***plink, int *table_index); // 两阶段解除查找
void dictTwoPhaseUnlinkFree(dict *d, dictEntry *he, dictEntry **plink, int table_index); // 两阶段解除释放
void dictRelease(dict *d); // 释放字典
dictEntry * dictFind(dict *d, const void *key); // 查找键值对
void *dictFetchValue(dict *d, const void *key); // 获取值
int dictResize(dict *d); // 重哈希字典
void dictSetKey(dict *d, dictEntry* de, void *key); // 设置键
void dictSetVal(dict *d, dictEntry *de, void *val); // 设置值
void dictSetSignedIntegerVal(dictEntry *de, int64_t val); // 设置有符号整数值
void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val); // 设置无符号整数值
void dictSetDoubleVal(dictEntry *de, double val); // 设置浮点数值
int64_t dictIncrSignedIntegerVal(dictEntry *de, int64_t val); // 有符号整数值增加
uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val); // 无符号整数值增加
double dictIncrDoubleVal(dictEntry *de, double val); // 浮点数值增加
void *dictEntryMetadata(dictEntry *de); // 获取字典节点元数据
void *dictGetKey(const dictEntry *de); // 获取键
void *dictGetVal(const dictEntry *de); // 获取值
int64_t dictGetSignedIntegerVal(const dictEntry *de); // 获取有符号整数值
uint64_t dictGetUnsignedIntegerVal(const dictEntry *de); // 获取无符号整数值
double dictGetDoubleVal(const dictEntry *de); // 获取浮点数值
double *dictGetDoubleValPtr(dictEntry *de); // 获取浮点数值指针
size_t dictMemUsage(const dict *d); // 字典内存使用量
size_t dictEntryMemUsage(void); // 字典节点内存使用量
dictIterator *dictGetIterator(dict *d); // 获取字典迭代器
dictIterator *dictGetSafeIterator(dict *d); // 获取安全字典迭代器
void dictInitIterator(dictIterator *iter, dict *d); // 初始化字典迭代器
void dictInitSafeIterator(dictIterator *iter, dict *d); // 初始化安全字典迭代器
void dictResetIterator(dictIterator *iter); // 重置字典迭代器
dictEntry *dictNext(dictIterator *iter); // 获取下一个字典节点
void dictReleaseIterator(dictIterator *iter); // 释放字典迭代器
dictEntry *dictGetRandomKey(dict *d);  // 获取随机键
dictEntry *dictGetFairRandomKey(dict *d); // 获取公平随机键
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count); // 获取一些键
void dictGetStats(char *buf, size_t bufsize, dict *d, int full); // 获取字典统计信息
uint64_t dictGenHashFunction(const void *key, size_t len); // 生成哈希函数
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len); // 生成大小写敏感哈希函数
void dictEmpty(dict *d, void(callback)(dict*)); // 清空字典
void dictSetResizeEnabled(dictResizeEnable enable); // 设置字典扩容选项
int dictRehash(dict *d, int n); // 重哈希字典
int dictRehashMilliseconds(dict *d, int ms); // 毫秒级重哈希字典
void dictSetHashFunctionSeed(uint8_t *seed); // 设置哈希函数种子
uint8_t *dictGetHashFunctionSeed(void); // 获取哈希函数种子
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata); // 字典扫描
unsigned long dictScanDefrag(dict *d, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata); // 字典碎片整理扫描
uint64_t dictGetHash(dict *d, const void *key); // 获取哈希值
dictEntry *dictFindEntryByPtrAndHash(dict *d, const void *oldptr, uint64_t hash); // 根据指针和哈希值查找字典节点

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);
#endif

#endif /* __DICT_H */
