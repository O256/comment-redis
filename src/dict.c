/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
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

// 翻译上面的注释：哈希表实现。这个文件实现了内存中的哈希表，包括插入、删除、替换、查找、获取随机元素操作。哈希表会自动调整大小，如果需要，将使用大小为2的幂的表，冲突将通过链接来处理。有关更多信息，请参阅源代码... :)

#include "fmacros.h" // 包含

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to disable
 * resizing and rehashing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to DICT_RESIZE_AVOID, not all
 * resizes are prevented: a hash table is still allowed to grow if the ratio
 * between the number of elements and the buckets > dict_force_resize_ratio. */
// 翻译：使用dictEnableResize() / dictDisableResize()，我们可以根据需要禁用哈希表的调整大小和rehash。这对于Redis非常重要，因为我们使用写时复制，并且在有子进程执行保存操作时不希望移动太多内存。请注意，即使将dict_can_resize设置为DICT_RESIZE_AVOID，也不是所有调整大小都被阻止：如果元素数量和桶之间的比率> dict_force_resize_ratio，则仍然允许哈希表增长。

static dictResizeEnable dict_can_resize = DICT_RESIZE_ENABLE; // 是否可以resize
static unsigned int dict_force_resize_ratio = 5;              // resize的比率

/* -------------------------- types ----------------------------------------- */

// 该结构体定义了字典的操作函数
/*
dict的内存布局如下：
+-----------------+
| dictType *type  |  dict的类型
+-----------------+
| dictht ht[2]    |  两个hash table
+-----------------+
| int rehashidx   |  rehash的索引
+-----------------+
| int iterators   |  迭代器的数量
+-----------------+

*/

// 字典结构
struct dictEntry
{
    void *key; // key，可以是任意类型，根据dictType的keyDup函数复制，根据dictType的keyCompare函数比较，根据dictType的keyHash函数计算hash值，根据dictType的keyDestructor函数释放
    union
    {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;                    // value，可以是任意类型，根据dictType的valDup函数复制，根据dictType的valCompare函数比较，根据dictType的valDestructor函数释放
    struct dictEntry *next; /* Next entry in the same hash bucket. */
    void *metadata[];       /* An arbitrary number of bytes (starting at a
                             * pointer-aligned address) of size as returned
                             * by dictType's dictEntryMetadataBytes(). */
                            // 翻译：任意数量的字节（从指针对齐的地址开始）的大小，由dictType的dictEntryMetadataBytes()返回
};

// 字典类型，定义了字典的操作函数，比如key的复制、key的比较、key的hash值计算、key的释放、value的复制、value的比较、value的释放
// 为什么需要这个结构？因为字典的key和value可以是任意类型，所以需要这个结构来定义这些操作
typedef struct
{
    void *key;
    dictEntry *next;
} dictEntryNoValue;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *d);                 // 扩容
static signed char _dictNextExp(unsigned long size);     // 获取下一个扩容的大小
static int _dictInit(dict *d, dictType *type);           // 初始化
static dictEntry *dictGetNext(const dictEntry *de);      // 获取下一个节点
static dictEntry **dictGetNextRef(dictEntry *de);        // 获取下一个节点的引用
static void dictSetNext(dictEntry *de, dictEntry *next); // 设置下一个节点

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16]; // hash 函数的种子

void dictSetHashFunctionSeed(uint8_t *seed)
{ // 设置 hash 函数的种子
    memcpy(dict_hash_function_seed, seed, sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void)
{ // 获取 hash 函数的种子
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */
// 使用 siphash 算法计算 hash 值
uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);

// 使用 siphash 算法计算 hash 值
// 和siphash的区别是，这个函数会将key转换为小写
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

// 使用 siphash 算法计算 hash 值
uint64_t dictGenHashFunction(const void *key, size_t len)
{
    return siphash(key, len, dict_hash_function_seed);
}

// 使用 siphash 算法计算 hash 值, 不区分大小写
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len)
{
    return siphash_nocase(buf, len, dict_hash_function_seed);
}

/* --------------------- dictEntry pointer bit tricks ----------------------  */

/* The 3 least significant bits in a pointer to a dictEntry determines what the
 * pointer actually points to. If the least bit is set, it's a key. Otherwise,
 * the bit pattern of the least 3 significant bits mark the kind of entry. */

#define ENTRY_PTR_MASK 7 /* 111 */     // 低3位的掩码
#define ENTRY_PTR_NORMAL 0 /* 000 */   // 普通节点
#define ENTRY_PTR_NO_VALUE 2 /* 010 */ // 没有 value 的节点

/* Returns 1 if the entry pointer is a pointer to a key, rather than to an
 * allocated entry. Returns 0 otherwise. */
// 判断是否是 key
static inline int entryIsKey(const dictEntry *de)
{
    return (uintptr_t)(void *)de & 1; // 低1位为 1，表示是 key
}

/* Returns 1 if the pointer is actually a pointer to a dictEntry struct. Returns
 * 0 otherwise. */
// 判断是否是普通的节点
static inline int entryIsNormal(const dictEntry *de)
{
    return ((uintptr_t)(void *)de & ENTRY_PTR_MASK) == ENTRY_PTR_NORMAL; // 低3位为 000，表示是普通节点
}

/* Returns 1 if the entry is a special entry with key and next, but without
 * value. Returns 0 otherwise. */
static inline int entryIsNoValue(const dictEntry *de)
{
    return ((uintptr_t)(void *)de & ENTRY_PTR_MASK) == ENTRY_PTR_NO_VALUE; // 低3位为 010，表示是没有 value 的节点
}

/* Creates an entry without a value field. */
// 创建一个没有 value 的节点, 低3位为 010, 表示没有 value
static inline dictEntry *createEntryNoValue(void *key, dictEntry *next)
{
    dictEntryNoValue *entry = zmalloc(sizeof(*entry));
    entry->key = key;                                                            // 设置 key
    entry->next = next;                                                          // 设置 next, 这里是一个链表，用于
    return (dictEntry *)(void *)((uintptr_t)(void *)entry | ENTRY_PTR_NO_VALUE); // 设置低3位为 010
}

// 编码标志位
static inline dictEntry *encodeMaskedPtr(const void *ptr, unsigned int bits)
{
    assert(((uintptr_t)ptr & ENTRY_PTR_MASK) == 0);
    return (dictEntry *)(void *)((uintptr_t)ptr | bits);
}

// 解码标志位
static inline void *decodeMaskedPtr(const dictEntry *de)
{
    assert(!entryIsKey(de));                                  // 传入的一定是key，key的低3位为标志位
    return (void *)((uintptr_t)(void *)de & ~ENTRY_PTR_MASK); // 后三位为标志位
}

/* Decodes the pointer to an entry without value, when you know it is an entry
 * without value. Hint: Use entryIsNoValue to check. */
// 解码noValue的entity
static inline dictEntryNoValue *decodeEntryNoValue(const dictEntry *de)
{
    return decodeMaskedPtr(de);
}

/* Returns 1 if the entry has a value field and 0 otherwise. */
// 检查en是否有value
static inline int entryHasValue(const dictEntry *de)
{
    return entryIsNormal(de); // value为普通节点，key为没有value的节点
}

/* ----------------------------- API implementation ------------------------- */

/* Reset hash table parameters already initialized with _dictInit()*/
// 重置字典
static void _dictReset(dict *d, int htidx)
{
    d->ht_table[htidx] = NULL;
    d->ht_size_exp[htidx] = -1;
    d->ht_used[htidx] = 0;
}

/* Create a new hash table */
// 创建一个新的hash table
dict *dictCreate(dictType *type)
{
    size_t metasize = type->dictMetadataBytes ? type->dictMetadataBytes() : 0;
    dict *d = zmalloc(sizeof(*d) + metasize);
    if (metasize)
    {
        memset(dictMetadata(d), 0, metasize);
    }

    _dictInit(d, type);
    return d;
}

/* Initialize the hash table */
// 初始化hash table
int _dictInit(dict *d, dictType *type)
{
    _dictReset(d, 0); // 初始化0、1的数据
    _dictReset(d, 1);
    d->type = type;
    d->rehashidx = -1;  //  rehash的index
    d->pauserehash = 0; // 是否暂定rehash
    return DICT_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
// 重新设置dict的大小
int dictResize(dict *d)
{
    unsigned long minimal;

    if (dict_can_resize != DICT_RESIZE_ENABLE || dictIsRehashing(d))
        return DICT_ERR;     // 如果resize标志位设置为不resize
    minimal = d->ht_used[0]; // 0和1桶放置其他信息
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE; // 最小尺寸为4
    return dictExpand(d, minimal);      // 扩容
}

/* Expand or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if expand was performed, and DICT_ERR if skipped. */
// 实际扩展字典函数
int _dictExpand(dict *d, unsigned long size, int *malloc_failed)
{
    if (malloc_failed)
        *malloc_failed = 0;

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size) // 0位置放置的是hash的大小
        return DICT_ERR;

    /* the new hash table */
    dictEntry **new_ht_table; // 新的hash table的2维指针
    unsigned long new_ht_used;
    signed char new_ht_size_exp = _dictNextExp(size); // 字典的下一个尺寸

    /* Detect overflows */
    size_t newsize = 1ul << new_ht_size_exp;
    if (newsize < size || newsize * sizeof(dictEntry *) < newsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    if (new_ht_size_exp == d->ht_size_exp[0])
        return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
    if (malloc_failed)
    {
        new_ht_table = ztrycalloc(newsize * sizeof(dictEntry *));
        *malloc_failed = new_ht_table == NULL;
        if (*malloc_failed)
            return DICT_ERR;
    }
    else
        new_ht_table = zcalloc(newsize * sizeof(dictEntry *));

    new_ht_used = 0;

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    if (d->ht_table[0] == NULL)
    {
        d->ht_size_exp[0] = new_ht_size_exp;
        d->ht_used[0] = new_ht_used;
        d->ht_table[0] = new_ht_table;
        return DICT_OK;
    }

    // 第1个hash为默认的hash
    // 第2个为rehash
    /* Prepare a second hash table for incremental rehashing */
    d->ht_size_exp[1] = new_ht_size_exp;
    d->ht_used[1] = new_ht_used;
    d->ht_table[1] = new_ht_table;
    d->rehashidx = 0;
    return DICT_OK;
}

/* return DICT_ERR if expand was not performed */
int dictExpand(dict *d, unsigned long size)
{
    return _dictExpand(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
int dictTryExpand(dict *d, unsigned long size)
{
    int malloc_failed;
    _dictExpand(d, size, &malloc_failed);
    return malloc_failed ? DICT_ERR : DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
// 重新hash，每次移动n个元素
// 移动案例说明：假设有两个桶，第一个桶有10个元素，第二个桶有5个元素，每次移动n个元素，那么第一次移动5个元素，第二次移动5个元素
int dictRehash(dict *d, int n)
{
    int empty_visits = n * 10; /* Max number of empty buckets to visit. */ // 最大访问的空桶数，q：空桶数是什么？a：空桶数是指没有元素的桶
    unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);                     // 第1个桶的大小
    unsigned long s1 = DICTHT_SIZE(d->ht_size_exp[1]);                     // 第2个桶的大小
    if (dict_can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d))
        return 0; // 选项为禁止rehash
    if (dict_can_resize == DICT_RESIZE_AVOID &&
        ((s1 > s0 && s1 / s0 < dict_force_resize_ratio) ||
         (s1 < s0 && s0 / s1 < dict_force_resize_ratio))) // 避免rehash
    {
        return 0;
    }

    // 从第一个桶中移动元素到第二个桶中， 每次移动多少个元素由n决定
    while (n-- && d->ht_used[0] != 0)
    {                           // 0位置放置的是hash的大小
        dictEntry *de, *nextde; // 当前节点和下一个节点

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        // rehashidx是第一个桶的索引
        assert(DICTHT_SIZE(d->ht_size_exp[0]) > (unsigned long)d->rehashidx); // 第一个桶的大小大于rehashidx, q：rehashindex存放的是什么？a：存放的是第一个桶的索引
        while (d->ht_table[0][d->rehashidx] == NULL)
        {                   // 如果第一个桶的rehashidx位置为空，rehashindex位置为null表示没有元素
            d->rehashidx++; // rehashidx加1
            if (--empty_visits == 0)
                return 1; // 如果访问的空桶数为0，返回1，表示还有元素需要移动，但是没有空桶了
        }
        // 从第一个桶中取出元素
        de = d->ht_table[0][d->rehashidx]; // 取出元素
        /* Move all the keys in this bucket from the old to the new hash HT */
        while (de)
        {               // 遍历链表
            uint64_t h; // hash值

            nextde = dictGetNext(de);   // 获取下一个节点
            void *key = dictGetKey(de); // 获取key
            /* Get the index in the new hash table */
            if (d->ht_size_exp[1] > d->ht_size_exp[0])
            {                                                                  // 第二个桶的大小大于第一个桶的大小，则直接复用第一个桶的hash值
                h = dictHashKey(d, key) & DICTHT_SIZE_MASK(d->ht_size_exp[1]); // 获取hash值
            }
            else
            { // 第二个桶的大小小于第一个桶的大小, 生成新的hash值
                /* We're shrinking the table. The tables sizes are powers of
                 * two, so we simply mask the bucket index in the larger table
                 * to get the bucket index in the smaller table. */
                h = d->rehashidx & DICTHT_SIZE_MASK(d->ht_size_exp[1]); // 获取hash值 & 上size相当于取余
            }
            if (d->type->no_value)
            { // 如果没有value
                if (d->type->keys_are_odd && !d->ht_table[1][h])
                { // 如果key是奇数，并且第二个桶的h位置为空
                    /* Destination bucket is empty and we can store the key
                     * directly without an allocated entry. Free the old entry
                     * if it's an allocated entry.
                     *
                     * TODO: Add a flag 'keys_are_even' and if set, we can use
                     * this optimization for these dicts too. We can set the LSB
                     * bit when stored as a dict entry and clear it again when
                     * we need the key back. */
                    assert(entryIsKey(key)); // key是一个key
                    if (!entryIsKey(de))
                        zfree(decodeMaskedPtr(de)); // 如果de不是key，释放de
                    de = key;                       // de设置为key
                }
                else if (entryIsKey(de))
                { // 如果de是key
                    /* We don't have an allocated entry but we need one. */
                    de = createEntryNoValue(key, d->ht_table[1][h]); // 创建一个没有value的节点
                }
                else
                {
                    /* Just move the existing entry to the destination table and
                     * update the 'next' field. */
                    assert(entryIsNoValue(de));
                    dictSetNext(de, d->ht_table[1][h]); // 设置下一个节点
                }
            }
            else
            {
                dictSetNext(de, d->ht_table[1][h]); // 设置下一个节点
            }
            d->ht_table[1][h] = de; // 设置第二个桶的h位置为de
            d->ht_used[0]--;        // 第一个桶的大小减1
            d->ht_used[1]++;        // 第二个桶的大小加1
            de = nextde;            // de设置为下一个节点
        }
        d->ht_table[0][d->rehashidx] = NULL; // 第一个桶的rehashidx位置设置为null
        d->rehashidx++;                      // rehashidx加1
    }

    /* Check if we already rehashed the whole table... */
    if (d->ht_used[0] == 0)
    {                          // 如果第一个桶的大小为0
        zfree(d->ht_table[0]); // 释放第一个桶
        /* Copy the new ht onto the old one */
        d->ht_table[0] = d->ht_table[1];       // 第一个桶设置为第二个桶
        d->ht_used[0] = d->ht_used[1];         // 第一个桶的大小设置为第二个桶的大小
        d->ht_size_exp[0] = d->ht_size_exp[1]; // 第一个桶的大小设置为第二个桶的大小
        _dictReset(d, 1);                      // 重置第二个桶
        d->rehashidx = -1;                     // rehashidx设置为-1
        return 0;
    }

    /* More to rehash... */
    return 1;
}

// 获取毫秒级的当前时间
long long timeInMilliseconds(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

/* Rehash in ms+"delta" milliseconds. The value of "delta" is larger
 * than 0, and is smaller than 1 in most cases. The exact upper bound
 * depends on the running time of dictRehash(d,100).*/
// 重新hash，直到时间超过ms
int dictRehashMilliseconds(dict *d, int ms)
{
    if (d->pauserehash > 0)
        return 0;

    long long start = timeInMilliseconds();
    int rehashes = 0;

    while (dictRehash(d, 100))
    {
        rehashes += 100;
        if (timeInMilliseconds() - start > ms)
            break; // 如果时间超过ms，退出
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some elements can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
// 翻译上上面的注释：这个函数只执行一步rehash，只有在我们的哈希表没有暂停的情况下才会执行。当我们的迭代器在rehash的中间时，我们不能干扰两个哈希表，否则一些元素可能会丢失或重复。
static void _dictRehashStep(dict *d)
{
    if (d->pauserehash == 0)
        dictRehash(d, 1); // 如果没有暂停rehash，执行一步rehash
}

/* Return a pointer to the metadata section within the dict. */
// 返回字典中元数据的指针，元数据中存放的是一些额外的信息，比如字典的大小
void *dictMetadata(dict *d)
{
    return &d->metadata;
}

/* Add an element to the target hash table */
// 添加一个元素到字典中
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d, key, NULL); // 添加一个元素

    if (!entry)
        return DICT_ERR;
    if (!d->type->no_value)
        dictSetVal(d, entry, val);
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */

/*
翻译：低级添加或查找：这个函数添加条目，但不设置值，而是将dictEntry结构返回给用户，用户将确保根据需要填充值字段。这个函数也直接暴露给用户API，主要是为了在哈希值中存储非指针，例如：entry = dictAddRaw(dict,mykey,NULL); if (entry != NULL) dictSetSignedIntegerVal(entry,1000); 返回值：如果键已经存在，则返回NULL，并且如果existing不为NULL，则将现有条目填充到“*existing”中。如果添加了键，则返回哈希条目以供调用者操作。
*/
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    /* Get the position for the new key or NULL if the key already exists. */
    void *position = dictFindPositionForInsert(d, key, existing); // 查找插入的位置
    if (!position)
        return NULL;

    /* Dup the key if necessary. */
    if (d->type->keyDup)
        key = d->type->keyDup(d, key);

    return dictInsertAtPosition(d, key, position);
}

/* Adds a key in the dict's hashtable at the position returned by a preceding
 * call to dictFindPositionForInsert. This is a low level function which allows
 * splitting dictAddRaw in two parts. Normally, dictAddRaw or dictAdd should be
 * used instead. */
// 翻一下上面的注释：在前面调用dictFindPositionForInsert返回的位置上添加一个key。这是一个低级函数，允许将dictAddRaw分为两部分。通常情况下，应该使用dictAddRaw或dictAdd。
dictEntry *dictInsertAtPosition(dict *d, void *key, void *position)
{
    dictEntry **bucket = position; /* It's a bucket, but the API hides that. */
    dictEntry *entry;
    /* If rehashing is ongoing, we insert in table 1, otherwise in table 0.
     * Assert that the provided bucket is the right table. */
    int htidx = dictIsRehashing(d) ? 1 : 0;
    assert(bucket >= &d->ht_table[htidx][0] &&
           bucket <= &d->ht_table[htidx][DICTHT_SIZE_MASK(d->ht_size_exp[htidx])]);
    size_t metasize = dictEntryMetadataSize(d);
    if (d->type->no_value)
    {
        assert(!metasize); /* Entry metadata + no value not supported. */
        if (d->type->keys_are_odd && !*bucket)
        {
            /* We can store the key directly in the destination bucket without the
             * allocated entry.
             *
             * TODO: Add a flag 'keys_are_even' and if set, we can use this
             * optimization for these dicts too. We can set the LSB bit when
             * stored as a dict entry and clear it again when we need the key
             * back. */
            entry = key;
            assert(entryIsKey(entry));
        }
        else
        {
            /* Allocate an entry without value. */
            entry = createEntryNoValue(key, *bucket);
        }
    }
    else
    {
        /* Allocate the memory and store the new entry.
         * Insert the element in top, with the assumption that in a database
         * system it is more likely that recently added entries are accessed
         * more frequently. */
        entry = zmalloc(sizeof(*entry) + metasize);
        assert(entryIsNormal(entry)); /* Check alignment of allocation */
        if (metasize > 0)
        {
            memset(dictEntryMetadata(entry), 0, metasize);
        }
        entry->key = key;
        entry->next = *bucket;
    }
    *bucket = entry;
    d->ht_used[htidx]++;

    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
// 翻译：添加或替换：添加一个元素，如果键已经存在，则丢弃旧值。如果键是从头添加的，则返回1，如果已经有这样的键，则返回0，并且dictReplace()只是执行了值更新操作。
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing; // 当前节点和已存在的节点

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    entry = dictAddRaw(d, key, &existing); // 添加一个元素
    if (entry)
    {
        dictSetVal(d, entry, val); // 设置value
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    // 翻译：设置新值并释放旧值。请注意，按照这个顺序执行是很重要的，因为值可能正好与以前的值完全相同。在这种情况下，考虑引用计数，您希望增加（设置），然后减少（释放），而不是相反。
    void *oldval = dictGetVal(existing);
    dictSetVal(d, existing, val);
    if (d->type->valDestructor)
        d->type->valDestructor(d, oldval); // 释放旧值
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
// 翻译：添加或查找：dictAddOrFind()只是dictAddRaw()的一个版本，它始终返回指定键的哈希条目，即使键已经存在并且无法添加（在这种情况下，将返回已经存在的键的条目）。有关更多信息，请参见dictAddRaw()。
dictEntry *dictAddOrFind(dict *d, void *key)
{
    dictEntry *entry, *existing;
    entry = dictAddRaw(d, key, &existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is a helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
// 翻译：搜索并删除一个元素。这是dictDelete()和dictUnlink()的一个辅助函数，请检查这些函数的顶部注释。
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree)
{
    uint64_t h, idx; // hash值和索引
    dictEntry *he, *prevHe; // 当前节点和前一个节点
    int table;             // 表

    /* dict is empty */
    if (dictSize(d) == 0)
        return NULL;

    if (dictIsRehashing(d))
        _dictRehashStep(d); // 重新hash
    h = dictHashKey(d, key); // 获取hash值

    for (table = 0; table <= 1; table++) // 遍历两个桶
    {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]); // 获取索引
        he = d->ht_table[table][idx];                     // 获取当前节点
        prevHe = NULL;
        while (he)
        {
            void *he_key = dictGetKey(he);
            if (key == he_key || dictCompareKeys(d, key, he_key))
            {
                /* Unlink the element from the list */
                if (prevHe)
                    dictSetNext(prevHe, dictGetNext(he));
                else
                    d->ht_table[table][idx] = dictGetNext(he);
                if (!nofree)
                {
                    dictFreeUnlinkedEntry(d, he);
                }
                d->ht_used[table]--;
                return he;
            }
            prevHe = he;
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d))
            break;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
int dictDelete(dict *ht, const void *key)
{
    return dictGenericDelete(ht, key, 0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
dictEntry *dictUnlink(dict *d, const void *key)
{
    return dictGenericDelete(d, key, 1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he)
{
    if (he == NULL)
        return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    if (!entryIsKey(he))
        zfree(decodeMaskedPtr(he));
}

/* Destroy an entire dictionary */
int _dictClear(dict *d, int htidx, void(callback)(dict *))
{
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]) && d->ht_used[htidx] > 0; i++)
    {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0)
            callback(d);

        if ((he = d->ht_table[htidx][i]) == NULL)
            continue;
        while (he)
        {
            nextHe = dictGetNext(he);
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            if (!entryIsKey(he))
                zfree(decodeMaskedPtr(he));
            d->ht_used[htidx]--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    zfree(d->ht_table[htidx]);
    /* Re-initialize the table */
    _dictReset(d, htidx);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *d)
{
    _dictClear(d, 0, NULL);
    _dictClear(d, 1, NULL);
    zfree(d);
}

dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;

    if (dictSize(d) == 0)
        return NULL; /* dict is empty */
    if (dictIsRehashing(d))
        _dictRehashStep(d);
    h = dictHashKey(d, key);
    for (table = 0; table <= 1; table++)
    {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        he = d->ht_table[table][idx];
        while (he)
        {
            void *he_key = dictGetKey(he);
            if (key == he_key || dictCompareKeys(d, key, he_key))
                return he;
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d))
            return NULL;
    }
    return NULL;
}

void *dictFetchValue(dict *d, const void *key)
{
    dictEntry *he;

    he = dictFind(d, key);
    return he ? dictGetVal(he) : NULL;
}

/* Find an element from the table, also get the plink of the entry. The entry
 * is returned if the element is found, and the user should later call
 * `dictTwoPhaseUnlinkFree` with it in order to unlink and release it. Otherwise if
 * the key is not found, NULL is returned. These two functions should be used in pair.
 * `dictTwoPhaseUnlinkFind` pauses rehash and `dictTwoPhaseUnlinkFree` resumes rehash.
 *
 * We can use like this:
 *
 * dictEntry *de = dictTwoPhaseUnlinkFind(db->dict,key->ptr,&plink, &table);
 * // Do something, but we can't modify the dict
 * dictTwoPhaseUnlinkFree(db->dict,de,plink,table); // We don't need to lookup again
 *
 * If we want to find an entry before delete this entry, this an optimization to avoid
 * dictFind followed by dictDelete. i.e. the first API is a find, and it gives some info
 * to the second one to avoid repeating the lookup
 */
dictEntry *dictTwoPhaseUnlinkFind(dict *d, const void *key, dictEntry ***plink, int *table_index)
{
    uint64_t h, idx, table;

    if (dictSize(d) == 0)
        return NULL; /* dict is empty */
    if (dictIsRehashing(d))
        _dictRehashStep(d);
    h = dictHashKey(d, key);

    for (table = 0; table <= 1; table++)
    {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        dictEntry **ref = &d->ht_table[table][idx];
        while (ref && *ref)
        {
            void *de_key = dictGetKey(*ref);
            if (key == de_key || dictCompareKeys(d, key, de_key))
            {
                *table_index = table;
                *plink = ref;
                dictPauseRehashing(d);
                return *ref;
            }
            ref = dictGetNextRef(*ref);
        }
        if (!dictIsRehashing(d))
            return NULL;
    }
    return NULL;
}

void dictTwoPhaseUnlinkFree(dict *d, dictEntry *he, dictEntry **plink, int table_index)
{
    if (he == NULL)
        return;
    d->ht_used[table_index]--;
    *plink = dictGetNext(he);
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    if (!entryIsKey(he))
        zfree(decodeMaskedPtr(he));
    dictResumeRehashing(d);
}

void dictSetKey(dict *d, dictEntry *de, void *key)
{
    assert(!d->type->no_value);
    if (d->type->keyDup)
        de->key = d->type->keyDup(d, key);
    else
        de->key = key;
}

void dictSetVal(dict *d, dictEntry *de, void *val)
{
    assert(entryHasValue(de));
    de->v.val = d->type->valDup ? d->type->valDup(d, val) : val;
}

void dictSetSignedIntegerVal(dictEntry *de, int64_t val)
{
    assert(entryHasValue(de));
    de->v.s64 = val;
}

void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val)
{
    assert(entryHasValue(de));
    de->v.u64 = val;
}

void dictSetDoubleVal(dictEntry *de, double val)
{
    assert(entryHasValue(de));
    de->v.d = val;
}

int64_t dictIncrSignedIntegerVal(dictEntry *de, int64_t val)
{
    assert(entryHasValue(de));
    return de->v.s64 += val;
}

uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val)
{
    assert(entryHasValue(de));
    return de->v.u64 += val;
}

double dictIncrDoubleVal(dictEntry *de, double val)
{
    assert(entryHasValue(de));
    return de->v.d += val;
}

/* A pointer to the metadata section within the dict entry. */
void *dictEntryMetadata(dictEntry *de)
{
    assert(entryHasValue(de));
    return &de->metadata;
}

void *dictGetKey(const dictEntry *de)
{
    if (entryIsKey(de))
        return (void *)de;
    if (entryIsNoValue(de))
        return decodeEntryNoValue(de)->key;
    return de->key;
}

void *dictGetVal(const dictEntry *de)
{
    assert(entryHasValue(de));
    return de->v.val;
}

int64_t dictGetSignedIntegerVal(const dictEntry *de)
{
    assert(entryHasValue(de));
    return de->v.s64;
}

uint64_t dictGetUnsignedIntegerVal(const dictEntry *de)
{
    assert(entryHasValue(de));
    return de->v.u64;
}

double dictGetDoubleVal(const dictEntry *de)
{
    assert(entryHasValue(de));
    return de->v.d;
}

/* Returns a mutable reference to the value as a double within the entry. */
double *dictGetDoubleValPtr(dictEntry *de)
{
    assert(entryHasValue(de));
    return &de->v.d;
}

/* Returns the 'next' field of the entry or NULL if the entry doesn't have a
 * 'next' field. */
static dictEntry *dictGetNext(const dictEntry *de)
{
    if (entryIsKey(de))
        return NULL; /* there's no next */
    if (entryIsNoValue(de))
        return decodeEntryNoValue(de)->next;
    return de->next;
}

/* Returns a pointer to the 'next' field in the entry or NULL if the entry
 * doesn't have a next field. */
static dictEntry **dictGetNextRef(dictEntry *de)
{
    if (entryIsKey(de))
        return NULL;
    if (entryIsNoValue(de))
        return &decodeEntryNoValue(de)->next;
    return &de->next;
}

static void dictSetNext(dictEntry *de, dictEntry *next)
{
    assert(!entryIsKey(de));
    if (entryIsNoValue(de))
    {
        dictEntryNoValue *entry = decodeEntryNoValue(de);
        entry->next = next;
    }
    else
    {
        de->next = next;
    }
}

/* Returns the memory usage in bytes of the dict, excluding the size of the keys
 * and values. */
size_t dictMemUsage(const dict *d)
{
    return dictSize(d) * sizeof(dictEntry) +
           dictSlots(d) * sizeof(dictEntry *);
}

size_t dictEntryMemUsage(void)
{
    return sizeof(dictEntry);
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
unsigned long long dictFingerprint(dict *d)
{
    unsigned long long integers[6], hash = 0;
    int j;

    integers[0] = (long)d->ht_table[0];
    integers[1] = d->ht_size_exp[0];
    integers[2] = d->ht_used[0];
    integers[3] = (long)d->ht_table[1];
    integers[4] = d->ht_size_exp[1];
    integers[5] = d->ht_used[1];

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++)
    {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

void dictInitIterator(dictIterator *iter, dict *d)
{
    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
}

void dictInitSafeIterator(dictIterator *iter, dict *d)
{
    dictInitIterator(iter, d);
    iter->safe = 1;
}

void dictResetIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0))
    {
        if (iter->safe)
            dictResumeRehashing(iter->d);
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
}

dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));
    dictInitIterator(iter, d);
    return iter;
}

dictIterator *dictGetSafeIterator(dict *d)
{
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

dictEntry *dictNext(dictIterator *iter)
{
    while (1)
    {
        if (iter->entry == NULL)
        {
            if (iter->index == -1 && iter->table == 0)
            {
                if (iter->safe)
                    dictPauseRehashing(iter->d);
                else
                    iter->fingerprint = dictFingerprint(iter->d);
            }
            iter->index++;
            if (iter->index >= (long)DICTHT_SIZE(iter->d->ht_size_exp[iter->table]))
            {
                if (dictIsRehashing(iter->d) && iter->table == 0)
                {
                    iter->table++;
                    iter->index = 0;
                }
                else
                {
                    break;
                }
            }
            iter->entry = iter->d->ht_table[iter->table][iter->index];
        }
        else
        {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry)
        {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = dictGetNext(iter->entry);
            return iter->entry;
        }
    }
    return NULL;
}

void dictReleaseIterator(dictIterator *iter)
{
    dictResetIterator(iter);
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0)
        return NULL;
    if (dictIsRehashing(d))
        _dictRehashStep(d);
    if (dictIsRehashing(d))
    {
        unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
        do
        {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (randomULong() % (dictSlots(d) - d->rehashidx));
            he = (h >= s0) ? d->ht_table[1][h - s0] : d->ht_table[0][h];
        } while (he == NULL);
    }
    else
    {
        unsigned long m = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
        do
        {
            h = randomULong() & m;
            he = d->ht_table[0][h];
        } while (he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    listlen = 0;
    orighe = he;
    while (he)
    {
        he = dictGetNext(he);
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while (listele--)
        he = dictGetNext(he);
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count)
{
    unsigned long j;      /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count)
        count = dictSize(d);
    maxsteps = count * 10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++)
    {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
    if (tables > 1 && maxsizemask < DICTHT_SIZE_MASK(d->ht_size_exp[1]))
        maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[1]);

    /* Pick a random point inside the larger table. */
    unsigned long i = randomULong() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while (stored < count && maxsteps--)
    {
        for (j = 0; j < tables; j++)
        {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned long)d->rehashidx)
            {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= DICTHT_SIZE(d->ht_size_exp[1]))
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= DICTHT_SIZE(d->ht_size_exp[j]))
                continue; /* Out of range for this table. */
            dictEntry *he = d->ht_table[j][i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL)
            {
                emptylen++;
                if (emptylen >= 5 && emptylen > count)
                {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            }
            else
            {
                emptylen = 0;
                while (he)
                {
                    /* Collect all the elements of the buckets found non empty while iterating.
                     * To avoid the issue of being unable to sample the end of a long chain,
                     * we utilize the Reservoir Sampling algorithm to optimize the sampling process.
                     * This means that even when the maximum number of samples has been reached,
                     * we continue sampling until we reach the end of the chain.
                     * See https://en.wikipedia.org/wiki/Reservoir_sampling. */
                    if (stored < count)
                    {
                        des[stored] = he;
                    }
                    else
                    {
                        unsigned long r = randomULong() % (stored + 1);
                        if (r < count)
                            des[r] = he;
                    }

                    he = dictGetNext(he);
                    stored++;
                }
                if (stored >= count)
                    goto end;
            }
        }
        i = (i + 1) & maxsizemask;
    }

end:
    return stored > count ? count : stored;
}

/* Reallocate the dictEntry, key and value allocations in a bucket using the
 * provided allocation functions in order to defrag them. */
static void dictDefragBucket(dict *d, dictEntry **bucketref, dictDefragFunctions *defragfns)
{
    dictDefragAllocFunction *defragalloc = defragfns->defragAlloc;
    dictDefragAllocFunction *defragkey = defragfns->defragKey;
    dictDefragAllocFunction *defragval = defragfns->defragVal;
    while (bucketref && *bucketref)
    {
        dictEntry *de = *bucketref, *newde = NULL;
        void *newkey = defragkey ? defragkey(dictGetKey(de)) : NULL;
        void *newval = defragval ? defragval(dictGetVal(de)) : NULL;
        if (entryIsKey(de))
        {
            if (newkey)
                *bucketref = newkey;
            assert(entryIsKey(*bucketref));
        }
        else if (entryIsNoValue(de))
        {
            dictEntryNoValue *entry = decodeEntryNoValue(de), *newentry;
            if ((newentry = defragalloc(entry)))
            {
                newde = encodeMaskedPtr(newentry, ENTRY_PTR_NO_VALUE);
                entry = newentry;
            }
            if (newkey)
                entry->key = newkey;
        }
        else
        {
            assert(entryIsNormal(de));
            newde = defragalloc(de);
            if (newde)
                de = newde;
            if (newkey)
                de->key = newkey;
            if (newval)
                de->v.val = newval;
        }
        if (newde)
        {
            *bucketref = newde;
            if (d->type->afterReplaceEntry)
                d->type->afterReplaceEntry(d, newde);
        }
        bucketref = dictGetNextRef(*bucketref);
    }
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d)
{
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d, entries, GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    if (count == 0)
        return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v)
{
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0)
    {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       void *privdata)
{
    return dictScanDefrag(d, v, fn, NULL, privdata);
}

/* Like dictScan, but additionally reallocates the memory used by the dict
 * entries using the provided allocation function. This feature was added for
 * the active defrag feature.
 *
 * The 'defragfns' callbacks are called with a pointer to memory that callback
 * can reallocate. The callbacks should return a new memory address or NULL,
 * where NULL means that no reallocation happened and the old memory is still
 * valid. */
unsigned long dictScanDefrag(dict *d,
                             unsigned long v,
                             dictScanFunction *fn,
                             dictDefragFunctions *defragfns,
                             void *privdata)
{
    int htidx0, htidx1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    if (dictSize(d) == 0)
        return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    dictPauseRehashing(d);

    if (!dictIsRehashing(d))
    {
        htidx0 = 0;
        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);

        /* Emit entries at cursor */
        if (defragfns)
        {
            dictDefragBucket(d, &d->ht_table[htidx0][v & m0], defragfns);
        }
        de = d->ht_table[htidx0][v & m0];
        while (de)
        {
            next = dictGetNext(de);
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);
    }
    else
    {
        htidx0 = 0;
        htidx1 = 1;

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (DICTHT_SIZE(d->ht_size_exp[htidx0]) > DICTHT_SIZE(d->ht_size_exp[htidx1]))
        {
            htidx0 = 1;
            htidx1 = 0;
        }

        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);
        m1 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx1]);

        /* Emit entries at cursor */
        if (defragfns)
        {
            dictDefragBucket(d, &d->ht_table[htidx0][v & m0], defragfns);
        }
        de = d->ht_table[htidx0][v & m0];
        while (de)
        {
            next = dictGetNext(de);
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do
        {
            /* Emit entries at cursor */
            if (defragfns)
            {
                dictDefragBucket(d, &d->ht_table[htidx1][v & m1], defragfns);
            }
            de = d->ht_table[htidx1][v & m1];
            while (de)
            {
                next = dictGetNext(de);
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    dictResumeRehashing(d);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * expands, we will check this allocation is allowed or not if the dict
 * type has expandAllowed member function. */
static int dictTypeExpandAllowed(dict *d)
{
    if (d->type->expandAllowed == NULL)
        return 1;
    return d->type->expandAllowed(
        DICTHT_SIZE(_dictNextExp(d->ht_used[0] + 1)) * sizeof(dictEntry *),
        (double)d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]));
}

/* Expand the hash table if needed */
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d))
        return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    if (DICTHT_SIZE(d->ht_size_exp[0]) == 0)
        return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if ((dict_can_resize == DICT_RESIZE_ENABLE &&
         d->ht_used[0] >= DICTHT_SIZE(d->ht_size_exp[0])) ||
        (dict_can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]) > dict_force_resize_ratio))
    {
        if (!dictTypeExpandAllowed(d))
            return DICT_OK;
        return dictExpand(d, d->ht_used[0] + 1);
    }
    return DICT_OK;
}

/* Our hash table capability is a power of two */
static signed char _dictNextExp(unsigned long size)
{
    if (size <= DICT_HT_INITIAL_SIZE)
        return DICT_HT_INITIAL_EXP; // 比初始化的尺寸小 则初始化为2个字节
    if (size >= LONG_MAX)
        return (8 * sizeof(long) - 1); // 比long的最大值还大，说明高位都用上了

    // __builtin_clzl(size-1) 返回从最高位开始连续0的个数
    return 8 * sizeof(long) - __builtin_clzl(size - 1); // 上面边界都不满足，则
}

/* Finds and returns the position within the dict where the provided key should
 * be inserted using dictInsertAtPosition if the key does not already exist in
 * the dict. If the key exists in the dict, NULL is returned and the optional
 * 'existing' entry pointer is populated, if provided. */
// 翻译：找到并返回字典中应该插入提供的键的位置，如果键不存在于字典中，则使用dictInsertAtPosition插入。如果键存在于字典中，则返回NULL，并且如果提供了可选的“现有”条目指针，则会填充该指针。
void *dictFindPositionForInsert(dict *d, const void *key, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    uint64_t hash = dictHashKey(d, key);
    if (existing)
        *existing = NULL;
    if (dictIsRehashing(d))
        _dictRehashStep(d); // 如果正在rehash，则进行一步rehash

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return NULL;

    // 计算hash值对应的索引
    for (table = 0; table <= 1; table++)
    {
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        /* Search if this slot does not already contain the given key */
        he = d->ht_table[table][idx]; // 获取对应索引的链表头
        while (he)
        {                                  // 遍历链表
            void *he_key = dictGetKey(he); // 获取当前节点的key
            if (key == he_key || dictCompareKeys(d, key, he_key))
            { // 如果key相等
                if (existing)
                    *existing = he; // 如果存在，则返回当前节点
                return NULL;
            }
            he = dictGetNext(he); // 下一个节点
        }
        if (!dictIsRehashing(d))
            break; // 如果不在rehash，则只遍历第一个表
    }

    /* If we are in the process of rehashing the hash table, the bucket is
     * always returned in the context of the second (new) hash table. */
    dictEntry **bucket = &d->ht_table[dictIsRehashing(d) ? 1 : 0][idx]; // 返回对应索引的链表头
    return bucket;                                                      // 返回链表头
}

// 清空字典
void dictEmpty(dict *d, void(callback)(dict *))
{
    _dictClear(d, 0, callback);
    _dictClear(d, 1, callback);
    d->rehashidx = -1;
    d->pauserehash = 0;
}

// 启用或禁用字典的自动rehash
void dictSetResizeEnabled(dictResizeEnable enable)
{
    dict_can_resize = enable;
}

// 获取对应key的hash值
uint64_t dictGetHash(dict *d, const void *key)
{
    return dictHashKey(d, key);
}

/* Finds the dictEntry using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is a pointer to the dictEntry if found, or NULL if not found. */
// 翻译：使用指针和预先计算的哈希查找dictEntry。oldkey是一个无效的指针，不应访问。应使用dictGetHash提供哈希值。不执行字符串/键比较。如果找到，则返回值是指向dictEntry的指针，如果未找到，则返回NULL。
dictEntry *dictFindEntryByPtrAndHash(dict *d, const void *oldptr, uint64_t hash)
{
    dictEntry *he;
    unsigned long idx, table;

    if (dictSize(d) == 0)
        return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++)
    {
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        he = d->ht_table[table][idx];
        while (he)
        {
            if (oldptr == dictGetKey(he)) // 如果key相等，直接对比指针？
                return he;
            he = dictGetNext(he);
        }
        if (!dictIsRehashing(d))
            return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50 // 统计向量长度

// 获取字典的统计信息
size_t _dictGetStatsHt(char *buf, size_t bufsize, dict *d, int htidx, int full)
{
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;              // 总链长
    unsigned long clvector[DICT_STATS_VECTLEN]; // 链长向量
    size_t l = 0;

    if (d->ht_used[htidx] == 0)
    {
        return snprintf(buf, bufsize,
                        "Hash table %d stats (%s):\n"
                        "No stats available for empty dictionaries\n",
                        htidx, (htidx == 0) ? "main hash table" : "rehashing target");
    }

    if (!full)
    {
        l += snprintf(buf + l, bufsize - l,
                      "Hash table %d stats (%s):\n"
                      " table size: %lu\n"
                      " number of elements: %lu\n",
                      htidx, (htidx == 0) ? "main hash table" : "rehashing target",
                      DICTHT_SIZE(d->ht_size_exp[htidx]), d->ht_used[htidx]);

        /* Make sure there is a NULL term at the end. */
        buf[bufsize - 1] = '\0';
        /* Unlike snprintf(), return the number of characters actually written. */
        return strlen(buf);
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++)
        clvector[i] = 0;
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]); i++)
    {
        dictEntry *he;

        if (d->ht_table[htidx][i] == NULL)
        {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = d->ht_table[htidx][i];
        while (he)
        {
            chainlen++;
            he = dictGetNext(he);
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN - 1)]++;
        if (chainlen > maxchainlen)
            maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf + l, bufsize - l,
                  "Hash table %d stats (%s):\n"
                  " table size: %lu\n"
                  " number of elements: %lu\n"
                  " different slots: %lu\n"
                  " max chain length: %lu\n"
                  " avg chain length (counted): %.02f\n"
                  " avg chain length (computed): %.02f\n"
                  " Chain length distribution:\n",
                  htidx, (htidx == 0) ? "main hash table" : "rehashing target",
                  DICTHT_SIZE(d->ht_size_exp[htidx]), d->ht_used[htidx], slots, maxchainlen,
                  (float)totchainlen / slots, (float)d->ht_used[htidx] / slots);

    for (i = 0; i < DICT_STATS_VECTLEN - 1; i++)
    {
        if (clvector[i] == 0)
            continue;
        if (l >= bufsize)
            break;
        l += snprintf(buf + l, bufsize - l,
                      "   %ld: %ld (%.02f%%)\n",
                      i, clvector[i], ((float)clvector[i] / DICTHT_SIZE(d->ht_size_exp[htidx])) * 100);
    }

    /* Make sure there is a NULL term at the end. */
    buf[bufsize - 1] = '\0';
    /* Unlike snprintf(), return the number of characters actually written. */
    return strlen(buf);
}

// 获取字典的统计信息
void dictGetStats(char *buf, size_t bufsize, dict *d, int full)
{
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf, bufsize, d, 0, full);
    if (dictIsRehashing(d) && bufsize > l)
    {
        buf += l;
        bufsize -= l;
        _dictGetStatsHt(buf, bufsize, d, 1, full);
    }
    /* Make sure there is a NULL term at the end. */
    orig_buf[orig_bufsize - 1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST
#include "testhelp.h"

#define UNUSED(V) ((void)V)

uint64_t hashCallback(const void *key)
{
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

int compareCallback(dict *d, const void *key1, const void *key2)
{
    int l1, l2;
    UNUSED(d);

    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(dict *d, void *val)
{
    UNUSED(d);

    zfree(val);
}

char *stringFromLongLong(long long value)
{
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf, sizeof(buf), "%lld", value);
    s = zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL,
    NULL};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg)                                      \
    do                                                          \
    {                                                           \
        elapsed = timeInMilliseconds() - start;                 \
        printf(msg ": %ld items in %lld ms\n", count, elapsed); \
    } while (0)

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int flags)
{
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType);
    long count = 0;
    int accurate = (flags & REDIS_TEST_ACCURATE);

    if (argc == 4)
    {
        if (accurate)
        {
            count = 5000000;
        }
        else
        {
            count = strtol(argv[3], NULL, 10);
        }
    }
    else
    {
        count = 5000;
    }

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        int retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict))
    {
        dictRehashMilliseconds(dict, 100);
    }

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        dictEntry *de = dictGetRandomKey(dict);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict, key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(j);
        int retval = dictDelete(dict, key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict, key, (void *)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}
#endif
