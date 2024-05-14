/* adlist.c - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * listRelease(), but private value of every node need to be freed
 * by the user before to call listRelease(), or by setting a free method using
 * listSetFreeMethod.
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
// 创建一个新的链表
list *listCreate(void)
{
    struct list *list;

    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* Remove all the elements from the list without destroying the list itself. */
// 清空链表
void listEmpty(list *list)
{
    unsigned long len;
    listNode *current, *next;

    current = list->head;
    len = list->len;
    while(len--) {
        next = current->next;
        if (list->free) list->free(current->value); // 释放节点的值
        zfree(current);
        current = next;
    }
    list->head = list->tail = NULL; // 头尾节点置空
    list->len = 0; // 长度置0
}

/* Free the whole list.
 *
 * This function can't fail. */
// 释放链表
void listRelease(list *list)
{
    listEmpty(list); // 清空链表
    zfree(list); // 释放链表
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
// 在链表头部添加节点
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    listLinkNodeHead(list, node); // 将节点链接到头节点
    return list;
}

/*
 * Add a node that has already been allocated to the head of list
 */
// 将节点链接到头节点
void listLinkNodeHead(list* list, listNode *node) {
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
// 在链表尾部添加节点
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    listLinkNodeTail(list, node);
    return list;
}

/*
 * Add a node that has already been allocated to the tail of list
 */
// 将节点链接到尾节点
void listLinkNodeTail(list *list, listNode *node) {
    if (list->len == 0) {
        list->head = list->tail = node; // 头尾节点都指向node, 赋值运算符顺序是从右到左
        node->prev = node->next = NULL;
    } else {
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
}

// 在指定节点后或前插入节点，after为1表示在后面插入，为0表示在前面插入
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (after) {
        node->prev = old_node; // 在后面插入
        node->next = old_node->next;
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
        node->next = old_node; // 在前面插入
        node->prev = old_node->prev;
        if (list->head == old_node) {
            list->head = node;
        }
    }
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    list->len++;
    return list;
}

/* Remove the specified node from the specified list.
 * The node is freed. If free callback is provided the value is freed as well.
 *
 * This function can't fail. */
// 删除节点
void listDelNode(list *list, listNode *node)
{
    listUnlinkNode(list, node); // 将节点从链表中移除
    if (list->free) list->free(node->value); // 释放节点的值
    zfree(node); // 释放节点
}

/*
 * Remove the specified node from the list without freeing it.
 */
// 将节点从链表中移除
void listUnlinkNode(list *list, listNode *node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;

    node->next = NULL;
    node->prev = NULL;

    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
// 获取迭代器
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
// 释放迭代器
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
// 重置迭代器, 从头部开始
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

// 重置迭代器, 从尾部开始
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage
 * pattern is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */
// 获取迭代器的下一个节点
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next; // 当前节点

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next; // 下一个节点
        else
            iter->next = current->prev; // 上一个节点
    }
    return current; // 返回当前节点
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
list *listDup(list *orig)
{
    list *copy; // 复制的链表
    listIter iter; // 迭代器
    listNode *node; // 节点

    if ((copy = listCreate()) == NULL) // 创建链表
        return NULL;
    copy->dup = orig->dup; // 复制函数
    copy->free = orig->free; // 释放函数
    copy->match = orig->match; // 匹配函数
    listRewind(orig, &iter); // 重置迭代器
    while((node = listNext(&iter)) != NULL) {
        void *value; // 值

        if (copy->dup) {
            value = copy->dup(node->value); // 复制节点的值
            if (value == NULL) { // 复制失败
                listRelease(copy); // 释放链表
                return NULL; // 返回NULL
            }
        } else {
            value = node->value; // 直接赋值，不复制
        }

        if (listAddNodeTail(copy, value) == NULL) { // 在尾部添加节点
            /* Free value if dup succeed but listAddNodeTail failed. */
            if (copy->free) copy->free(value); // 释放节点的值

            listRelease(copy); // 释放链表
            return NULL;
        }
    }
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
// 查找节点，返回第一个匹配的节点，如果没有匹配的节点则返回NULL，从头部开始查找，如果没有设置匹配函数，则直接比较节点的值和key，如果相等则返回，否则继续查找
listNode *listSearchKey(list *list, void *key)
{
    listIter iter; // 迭代器
    listNode *node; // 节点

    listRewind(list, &iter); // 重置迭代器
    while((node = listNext(&iter)) != NULL) { // 遍历链表
        if (list->match) { // 如果设置了匹配函数
            if (list->match(node->value, key)) { // 调用匹配函数
                return node; // 返回节点
            }
        } else {
            if (key == node->value) { // 直接比较节点的值和key
                return node;
            }
        }
    }
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
// 获取指定索引的节点
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) { // 如果是负数
        index = (-index)-1; // 转换为正数
        n = list->tail; // 尾节点
        while(index-- && n) n = n->prev; // 向前查找，直到index为0或者n为NULL
    } else {
        n = list->head; // 头节点
        while(index-- && n) n = n->next; // 向后查找
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
// 旋转链表，将尾节点移动到头部
void listRotateTailToHead(list *list) {
    if (listLength(list) <= 1) return; // 链表长度小于等于1，直接返回

    /* Detach current tail */
    listNode *tail = list->tail; // 尾节点
    list->tail = tail->prev; // 尾节点指向前一个节点
    list->tail->next = NULL; // 前一个节点的next指针置空
    /* Move it as head */
    list->head->prev = tail; // 头节点的prev指针指向tail
    tail->prev = NULL; // tail的prev指针置空
    tail->next = list->head; // tail的next指针指向头节点
    list->head = tail; // 头节点指向tail
}

/* Rotate the list removing the head node and inserting it to the tail. */
// 旋转链表，将头节点移动到尾部
void listRotateHeadToTail(list *list) {
    if (listLength(list) <= 1) return;

    listNode *head = list->head; // 头节点
    /* Detach current head */
    list->head = head->next; // 头节点指向下一个节点
    list->head->prev = NULL; // 下一个节点的prev指针置空
    /* Move it as tail */
    list->tail->next = head; // 尾节点的next指针指向head
    head->next = NULL; // head的next指针置空
    head->prev = list->tail; // head的prev指针指向尾节点
    list->tail = head; // 尾节点指向head
}

/* Add all the elements of the list 'o' at the end of the
 * list 'l'. The list 'other' remains empty but otherwise valid. */
// 合并两个链表
void listJoin(list *l, list *o) {
    if (o->len == 0) return;

    o->head->prev = l->tail; // o的头节点的prev指针指向l的尾节点

    if (l->tail)
        l->tail->next = o->head; // 尾节点的next指针指向o的头节点
    else
        l->head = o->head; // 头节点指向o的头节点

    l->tail = o->tail; // 尾节点指向o的尾节点
    l->len += o->len; // 长度增加

    /* Setup other as an empty list. */
    o->head = o->tail = NULL; // o的头尾节点置空
    o->len = 0; // 长度置0
}

/* Initializes the node's value and sets its pointers
 * so that it is initially not a member of any list.
 */
// 初始化节点
void listInitNode(listNode *node, void *value) {
    node->prev = NULL;
    node->next = NULL;
    node->value = value;
}
