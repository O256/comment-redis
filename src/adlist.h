/* adlist.h - A generic doubly linked list implementation
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

// 链表节点
typedef struct listNode {
    struct listNode *prev; // 前一个节点
    struct listNode *next; // 后一个节点
    void *value; // 节点的值
} listNode; // 链表节点

// 链表的迭代器
typedef struct listIter {
    listNode *next;
    int direction; // 迭代器的方向
} listIter;

// 链表结构
typedef struct list {
    listNode *head; // 头节点
    listNode *tail; // 尾节点
    void *(*dup)(void *ptr); // 复制函数
    void (*free)(void *ptr); // 释放函数
    int (*match)(void *ptr, void *key); // 匹配函数
    unsigned long len; // 链表长度
} list; // 链表

/* Functions implemented as macros */
#define listLength(l) ((l)->len) // 获取链表长度
#define listFirst(l) ((l)->head) // 获取链表头节点
#define listLast(l) ((l)->tail) // 获取链表尾节点
#define listPrevNode(n) ((n)->prev) // 获取前一个节点
#define listNextNode(n) ((n)->next) // 获取后一个节点
#define listNodeValue(n) ((n)->value) // 获取节点的值

#define listSetDupMethod(l,m) ((l)->dup = (m)) // 设置复制函数
#define listSetFreeMethod(l,m) ((l)->free = (m)) // 设置释放函数
#define listSetMatchMethod(l,m) ((l)->match = (m)) // 设置匹配函数

#define listGetDupMethod(l) ((l)->dup) // 获取复制函数
#define listGetFreeMethod(l) ((l)->free) // 获取释放函数
#define listGetMatchMethod(l) ((l)->match) // 获取匹配函数

/* Prototypes */
list *listCreate(void); // 创建链表
void listRelease(list *list); // 释放链表
void listEmpty(list *list); // 清空链表
list *listAddNodeHead(list *list, void *value); // 在链表头部添加节点
list *listAddNodeTail(list *list, void *value); // 在链表尾部添加节点
list *listInsertNode(list *list, listNode *old_node, void *value, int after); // 在指定节点后或前插入节点
void listDelNode(list *list, listNode *node); // 删除节点
listIter *listGetIterator(list *list, int direction); // 获取迭代器
listNode *listNext(listIter *iter); // 获取迭代器的下一个节点
void listReleaseIterator(listIter *iter); // 释放迭代器
list *listDup(list *orig); // 复制链表
listNode *listSearchKey(list *list, void *key); // 查找节点
listNode *listIndex(list *list, long index); // 获取指定索引的节点
void listRewind(list *list, listIter *li); // 重置迭代器
void listRewindTail(list *list, listIter *li); // 重置迭代器
void listRotateTailToHead(list *list); // 将尾节点移动到头节点
void listRotateHeadToTail(list *list); // 将头节点移动到尾节点
void listJoin(list *l, list *o); // 合并两个链表
void listInitNode(listNode *node, void *value); // 初始化节点
void listLinkNodeHead(list *list, listNode *node); // 将节点链接到头节点
void listLinkNodeTail(list *list, listNode *node); // 将节点链接到尾节点
void listUnlinkNode(list *list, listNode *node); // 将节点从链表中移除

/* Directions for iterators */
#define AL_START_HEAD 0 // 迭代器的方向
#define AL_START_TAIL 1 // 迭代器的方向

#endif /* __ADLIST_H__ */
