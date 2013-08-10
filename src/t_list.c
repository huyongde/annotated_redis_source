/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "redis.h"

void signalListAsReady(redisClient *c, robj *key);

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* Check the argument length to see if it requires us to convert the ziplist
 * to a real list. Only check raw-encoded objects because integer encoded
 * objects are never too long. */
/*
 * ��� value ���������һ���ַ����Ļ������� ziplist �ܷ����㴢�����ĳ���Ҫ��
 * ������ܵĻ����� subject ת��Ϊ˫������
 *
 * ��� value Ϊ�������ͣ���ô���ض�����飬��Ϊ���������ֻ���� long ����
 *
 * T = O(N)
 */
void listTypeTryConversion(robj *subject, robj *value)
{

    // �Ѿ��� LINKEDLIST
    if (subject->encoding != REDIS_ENCODING_ZIPLIST) return;

    if (value->encoding == REDIS_ENCODING_RAW &&
            sdslen(value->ptr) > server.list_max_ziplist_value)
        listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
}

/* The function pushes an elmenet to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to incremnet the refcount of 'value' as
 * the function takes care of it if needed. */
/*
 * ��̬���뺯��
 *
 * ���� where �������� value �����б� subject �ı�ͷ���β
 *
 * �����߲��ض� value ���м�������������ᴦ����
 *
 * T = O(N^2)
 */
void listTypePush(robj *subject, robj *value, int where)
{
    /* Check if we need to convert the ziplist */
    // ����Ƿ���Ҫ���б���б���ת��
    // O(N)
    listTypeTryConversion(subject,value);
    if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
            ziplistLen(subject->ptr) >= server.list_max_ziplist_entries)
        listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);

    // ziplist
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
    {
        int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
        value = getDecodedObject(value);
        // O(N^2)
        subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),pos);
        decrRefCount(value);
        // ˫������
    }
    else if (subject->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // O(1)
        if (where == REDIS_HEAD)
        {
            listAddNodeHead(subject->ptr,value);
        }
        else
        {
            listAddNodeTail(subject->ptr,value);
        }
        incrRefCount(value);
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
}

/*
 * ��̬ pop ����
 *
 * T = O(1)
 */
robj *listTypePop(robj *subject, int where)
{

    robj *value = NULL;

    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // �� ziplist �� pop
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        // pop ��ͷ���β��
        int pos = (where == REDIS_HEAD) ? 0 : -1;
        // O(1)
        p = ziplistIndex(subject->ptr,pos);
        // Ԫ�ػ�ȡ�ɹ���
        if (ziplistGet(p,&vstr,&vlen,&vlong))
        {
            // ȡ��ֵ
            if (vstr)
            {
                value = createStringObject((char*)vstr,vlen);
            }
            else
            {
                value = createStringObjectFromLongLong(vlong);
            }
            /* We only need to delete an element when it exists */
            // ɾ����
            subject->ptr = ziplistDelete(subject->ptr,&p);
        }
    }
    else if (subject->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // O(1)

        // ��˫���б��� pop
        list *list = subject->ptr;
        listNode *ln;
        // ȡ����ͷ�ڵ�
        if (where == REDIS_HEAD)
        {
            ln = listFirst(list);
            // ȡ����β�ڵ�
        }
        else
        {
            ln = listLast(list);
        }

        if (ln != NULL)
        {
            // ȡ���ڵ��ֵ
            value = listNodeValue(ln);

            incrRefCount(value);

            // ɾ���ڵ�
            listDelNode(list,ln);
        }
    }
    else
    {
        redisPanic("Unknown list encoding");
    }

    return value;
}

/*
 * ��̬�б��Ȼ�ȡ����
 *
 * T = O(N)
 */
unsigned long listTypeLength(robj *subject)
{
    // ziplist
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // O(N)
        return ziplistLen(subject->ptr);
    }
    else if (subject->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // adlist
        // O(1)
        return listLength((list*)subject->ptr);
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
/*
 * ������̬�б������
 *
 * T = O(1)
 */
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction)
{

    // ����������������ʼ����������
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));

    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;

    // ���� ziplist
    if (li->encoding == REDIS_ENCODING_ZIPLIST)
    {
        li->zi = ziplistIndex(subject->ptr,index);

        // ����˫������
    }
    else if (li->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        li->ln = listIndex(subject->ptr,index);

    }
    else
    {
        redisPanic("Unknown list encoding");
    }

    return li;
}

/* Clean up the iterator. */
/*
 * �ͷŵ�����
 *
 * T = O(1)
 */
void listTypeReleaseIterator(listTypeIterator *li)
{
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
/*
 * ����ǰ�������Ľڵ㱣�浽 entry ��������������ָ����ǰ����һ����
 *
 * ��ȡ�ڵ�ɹ����� 1 �����򷵻� 0 ��
 *
 * T = O(1)
 */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry)
{
    /* Protect from converting when iterating */
    redisAssert(li->subject->encoding == li->encoding);

    entry->li = li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // ziplist, O(1)
        entry->zi = li->zi;
        if (entry->zi != NULL)
        {
            if (li->direction == REDIS_TAIL)
                li->zi = ziplistNext(li->subject->ptr,li->zi);
            else
                li->zi = ziplistPrev(li->subject->ptr,li->zi);
            return 1;
        }
    }
    else if (li->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // adlist, O(1)
        entry->ln = li->ln;
        if (entry->ln != NULL)
        {
            if (li->direction == REDIS_TAIL)
                li->ln = li->ln->next;
            else
                li->ln = li->ln->prev;
            return 1;
        }
    }
    else
    {
        redisPanic("Unknown list encoding");
    }

    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
/*
 * ���ص�������ǰ�ڵ��ֵ����������Ѿ���ɣ����� NULL
 *
 * T = O(1)
 */
robj *listTypeGet(listTypeEntry *entry)
{

    listTypeIterator *li = entry->li;

    robj *value = NULL;

    if (li->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        redisAssert(entry->zi != NULL);
        // O(1)
        if (ziplistGet(entry->zi,&vstr,&vlen,&vlong))
        {
            if (vstr)
            {
                value = createStringObject((char*)vstr,vlen);
            }
            else
            {
                value = createStringObjectFromLongLong(vlong);
            }
        }
    }
    else if (li->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        redisAssert(entry->ln != NULL);
        // O(1)
        value = listNodeValue(entry->ln);
        incrRefCount(value);
    }
    else
    {
        redisPanic("Unknown list encoding");
    }

    return value;
}

/*
 * ��̬���뺯��
 *
 * T = O(N^2)
 */
void listTypeInsert(listTypeEntry *entry, robj *value, int where)
{

    robj *subject = entry->li->subject;

    if (entry->li->encoding == REDIS_ENCODING_ZIPLIST)
    {
        value = getDecodedObject(value);
        if (where == REDIS_TAIL)
        {
            // ���뵽��β

            // O(1)
            unsigned char *next = ziplistNext(subject->ptr,entry->zi);

            /* When we insert after the current element, but the current element
             * is the tail of the list, we need to do a push. */
            // �ҵ� next �ͽ��ڵ������ next ֮��û�ҵ��ͽ��ڵ�ŵ���β
            if (next == NULL)
            {
                // O(N^2)
                subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),REDIS_TAIL);
            }
            else
            {
                // O(N^2)
                subject->ptr = ziplistInsert(subject->ptr,next,value->ptr,sdslen(value->ptr));
            }
        }
        else
        {
            // ���뵽��ͷ��O(N^2)
            subject->ptr = ziplistInsert(subject->ptr,entry->zi,value->ptr,sdslen(value->ptr));
        }
        decrRefCount(value);
    }
    else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // O(1)
        if (where == REDIS_TAIL)
        {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_TAIL);
        }
        else
        {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_HEAD);
        }
        incrRefCount(value);
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
/*
 * �Ա� entry ��ֵ�Ͷ��� o
 *
 * T = O(N)
 */
int listTypeEqual(listTypeEntry *entry, robj *o)
{

    listTypeIterator *li = entry->li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST)
    {
        redisAssertWithInfo(NULL,o,o->encoding == REDIS_ENCODING_RAW);
        // O(1)
        return ziplistCompare(entry->zi,o->ptr,sdslen(o->ptr));
    }
    else if (li->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // O(N)
        return equalStringObjects(o,listNodeValue(entry->ln));
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
/*
 * ��̬ɾ�� entry ָ���Ԫ��
 *
 * T = O(N^2)
 */
void listTypeDelete(listTypeEntry *entry)
{

    listTypeIterator *li = entry->li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *p = entry->zi;
        // O(N^2)
        li->subject->ptr = ziplistDelete(li->subject->ptr,&p);

        /* Update position of the iterator depending on the direction */
        if (li->direction == REDIS_TAIL)
            li->zi = p;
        else
            li->zi = ziplistPrev(li->subject->ptr,p);
    }
    else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // O(1)
        listNode *next;
        if (li->direction == REDIS_TAIL)
            next = entry->ln->next;
        else
            next = entry->ln->prev;
        listDelNode(li->subject->ptr,entry->ln);
        li->ln = next;
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
}

/*
 * ���б�ת��Ϊ�����ı�������
 *
 * Ŀǰֻ֧�ֽ� ziplist ת��Ϊ˫������
 *
 * T = O(N)
 */
void listTypeConvert(robj *subject, int enc)
{
    listTypeIterator *li;
    listTypeEntry entry;
    redisAssertWithInfo(NULL,subject,subject->type == REDIS_LIST);

    if (enc == REDIS_ENCODING_LINKEDLIST)
    {
        // ����˫���б�
        list *l = listCreate();
        listSetFreeMethod(l,decrRefCount);

        /* listTypeGet returns a robj with incremented refcount */
        // ȡ�� ziplist �е�����Ԫ��
        // ����������ӵ�˫���б���
        // O(N)
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(li,&entry)) listAddNodeTail(l,listTypeGet(&entry));
        listTypeReleaseIterator(li);

        // ���±���
        subject->encoding = REDIS_ENCODING_LINKEDLIST;
        // �ͷ� ziplist
        zfree(subject->ptr);
        // ָ��˫������
        subject->ptr = l;
    }
    else
    {
        redisPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

/*
 * [LR]PUSH �����ʵ��
 *
 * T = O(N^3)
 */
void pushGenericCommand(redisClient *c, int where)
{
    int j, waiting = 0, pushed = 0;

    // �����б����O(1)
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);

    // ����б�Ϊ�գ���ô���������пͻ��˵ȴ�����б�
    int may_have_waiting_clients = (lobj == NULL);

    // ���ͼ��
    if (lobj && lobj->type != REDIS_LIST)
    {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    // ����Ƿ��пͻ����ڵȴ�����б�
    // ����ǵĻ�����֪�������Ϳͻ��ˣ�����б��Ѿ�����
    // O(1)
    if (may_have_waiting_clients) signalListAsReady(c,c->argv[1]);

    // ����������Ԫ�������б�
    // O(N^3)
    for (j = 2; j < c->argc; j++)
    {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        // ����б����ڣ���ô�������б�
        if (!lobj)
        {
            lobj = createZiplistObject();   // Ĭ��ʹ�� ziplist ����
            dbAdd(c->db,c->argv[1],lobj);
        }
        // ��Ԫ�������б�O(N^2)
        listTypePush(lobj,c->argv[j],where);
        pushed++;
    }

    addReplyLongLong(c, waiting + (lobj ? listTypeLength(lobj) : 0));
    if (pushed) signalModifiedKey(c->db,c->argv[1]);
    server.dirty += pushed;
}

/*
 * LPUSH �����ʵ��
 *
 * T = O(N^3)
 */
void lpushCommand(redisClient *c)
{
    pushGenericCommand(c,REDIS_HEAD);
}

/*
 * RPUSH �����ʵ��
 *
 * T = O(N^3)
 */
void rpushCommand(redisClient *c)
{
    pushGenericCommand(c,REDIS_TAIL);
}

/*
 * PUSHX �����ʵ��
 *
 * T = O(N^2)
 */
void pushxGenericCommand(redisClient *c, robj *refval, robj *val, int where)
{
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    // ���һ򴴽����������ͼ��, O(1)
    if ((subject = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,subject,REDIS_LIST)) return;

    // ����ָ�� value Ҫ���� refval ֮ǰ��֮��
    if (refval != NULL)
    {
        /* Note: we expect refval to be string-encoded because it is *not* the
         * last argument of the multi-bulk LINSERT. */
        redisAssertWithInfo(c,refval,refval->encoding == REDIS_ENCODING_RAW);

        /* We're not sure if this value can be inserted yet, but we cannot
         * convert the list inside the iterator. We don't want to loop over
         * the list twice (once to see if the value can be inserted and once
         * to do the actual insert), so we assume this value can be inserted
         * and convert the ziplist to a regular list if necessary. */
        // ������ value �Ƿ���Ҫ�� subject ���б���ת��
        // O(N)
        listTypeTryConversion(subject,val);

        /* Seek refval from head to tail */
        // �ӱ�ͷ��ʼ�����β���Ұ��� refval �Ľڵ�
        // O(N^2)
        iter = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(iter,&entry))
        {
            if (listTypeEqual(&entry,refval))
            {
                // �ҵ������� val, O(N^2)
                listTypeInsert(&entry,val,where);
                inserted = 1;
                break;
            }
        }
        listTypeReleaseIterator(iter);

        // value �Ѿ�����ɹ���
        if (inserted)
        {
            /* Check if the length exceeds the ziplist length threshold. */
            // ����Ƿ���Ҫ���б���б���ת��, O(N)
            if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
                    ziplistLen(subject->ptr) > server.list_max_ziplist_entries)
                listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
            signalModifiedKey(c->db,c->argv[1]);
            server.dirty++;
        }
        else
        {
            /* Notify client of a failed insert */
            addReply(c,shared.cnegone);
            return;
        }
    }
    else
    {
        // �򵥵ؽ� value ���뵽�б��֮ǰ��֮��
        // O(N^2)
        listTypePush(subject,val,where);

        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }

    addReplyLongLong(c,listTypeLength(subject));
}

/*
 * LPUSHX �����ʵ��
 *
 * T = O(N^2)
 */
void lpushxCommand(redisClient *c)
{
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_HEAD);
}

/*
 * RPUSHX �����ʵ��
 *
 * T = O(N^2)
 */
void rpushxCommand(redisClient *c)
{
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_TAIL);
}

/*
 * LINSERT �����ʵ��
 *
 * T = O(N^2)
 */
void linsertCommand(redisClient *c)
{

    c->argv[4] = tryObjectEncoding(c->argv[4]);

    // ���뵽�ڵ�֮��
    if (strcasecmp(c->argv[2]->ptr,"after") == 0)
    {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_TAIL);
        // ���뵽�ڵ�֮ǰ
    }
    else if (strcasecmp(c->argv[2]->ptr,"before") == 0)
    {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_HEAD);
    }
    else
    {
        addReply(c,shared.syntaxerr);
    }
}

/*
 * LLEN �����ʵ��
 *
 * T = O(N)
 */
void llenCommand(redisClient *c)
{

    // ����������б����
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);

    // ���ͼ��
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

    // ���س���
    addReplyLongLong(c,listTypeLength(o));
}

/*
 * LINDEX �����ʵ��
 *
 * T = O(N)
 */
void lindexCommand(redisClient *c)
{
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    long index;
    robj *value = NULL;

    // ��ȡ�б����򷵻ز�����
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;

    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // �� ziplist �л�ȡ
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        // O(1)
        p = ziplistIndex(o->ptr,index);
        if (ziplistGet(p,&vstr,&vlen,&vlong))
        {
            // ȡ��ֵ
            if (vstr)
            {
                value = createStringObject((char*)vstr,vlen);
            }
            else
            {
                value = createStringObjectFromLongLong(vlong);
            }
            addReplyBulk(c,value);
            decrRefCount(value);
        }
        else
        {
            addReply(c,shared.nullbulk);
        }
    }
    else if (o->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // ��˫���б���ȡ��ֵ
        // O(N)
        listNode *ln = listIndex(o->ptr,index);
        if (ln != NULL)
        {
            value = listNodeValue(ln);
            addReplyBulk(c,value);
        }
        else
        {
            addReply(c,shared.nullbulk);
        }
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
}

/*
 * LSET �����ʵ��
 *
 * T = O(N^2)
 */
void lsetCommand(redisClient *c)
{
    // ���Ҷ��󣬻��߷��ز����ڴ���
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);

    // ���ͼ��
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

    long index;
    // ��������ֵ
    robj *value = (c->argv[3] = tryObjectEncoding(c->argv[3]));

    // ��ȡ index ����
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;

    // �������Ҫ��ת���б�ı���,O(N)
    listTypeTryConversion(o,value);

    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // ���µ� ziplist
        unsigned char *p, *zl = o->ptr;
        p = ziplistIndex(zl,index);
        if (p == NULL)
        {
            // index Խ��
            addReply(c,shared.outofrangeerr);
        }
        else
        {
            // ��ɾ�� ziplist ��ָ�� index ��ֵ
            // O(N^2)
            o->ptr = ziplistDelete(o->ptr,&p);
            // �ٽ���ֵ��ӵ� ziplist ��ĩβ
            value = getDecodedObject(value);
            // O(N^2)
            o->ptr = ziplistInsert(o->ptr,p,value->ptr,sdslen(value->ptr));

            decrRefCount(value);

            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            server.dirty++;
        }
    }
    else if (o->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // ��ӵ�˫������
        // O(N)
        listNode *ln = listIndex(o->ptr,index);
        if (ln == NULL)
        {
            addReply(c,shared.outofrangeerr);
        }
        else
        {
            // ɾ�� ln ԭ�е�ֵ
            // O(1)
            decrRefCount((robj*)listNodeValue(ln));
            // ������ֵ�滻��
            listNodeValue(ln) = value;

            incrRefCount(value);

            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            server.dirty++;
        }
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
}

/*
 * ���б��е���һ��Ԫ�أ��������Ԫ��֮���б�Ϊ�գ���ôɾ����
 *
 * T = O(1)
 */
void popGenericCommand(redisClient *c, int where)
{
    // ���Ҷ��󣬻��߷��ز�������Ϣ
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk);

    // ���ͼ��
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

    // ���ö�̬ pop ����
    robj *value = listTypePop(o,where);
    if (value == NULL)
    {
        addReply(c,shared.nullbulk);
    }
    else
    {
        addReplyBulk(c,value);

        decrRefCount(value);

        // ����б�Ϊ�գ���ôɾ����
        if (listTypeLength(o) == 0) dbDelete(c->db,c->argv[1]);

        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

/*
 * LPOP �����ʵ��
 *
 * T = O(1)
 */
void lpopCommand(redisClient *c)
{
    popGenericCommand(c,REDIS_HEAD);
}

/*
 * RPOP �����ʵ��
 *
 * T = O(1)
 */
void rpopCommand(redisClient *c)
{
    popGenericCommand(c,REDIS_TAIL);
}

/*
 * T = O(N)
 */
void lrangeCommand(redisClient *c)
{
    robj *o;
    long start, end, llen, rangelen;

    // ��� start �� end �����Ƿ�����
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
            (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    // ���Ҷ��󣬶��󲻴���ʱ���ؿ�
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
            || checkType(c,o,REDIS_LIST)) return;

    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen)
    {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c,rangelen);
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // ���� ziplist
        unsigned char *p = ziplistIndex(o->ptr,start);
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        // O(N)
        while(rangelen--)
        {
            ziplistGet(p,&vstr,&vlen,&vlong);
            if (vstr)
            {
                addReplyBulkCBuffer(c,vstr,vlen);
            }
            else
            {
                addReplyBulkLongLong(c,vlong);
            }
            p = ziplistNext(o->ptr,p);
        }
    }
    else if (o->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        // ����˫������
        listNode *ln;

        /* If we are nearest to the end of the list, reach the element
         * starting from tail and going backward, as it is faster. */
        if (start > llen/2) start -= llen;
        ln = listIndex(o->ptr,start);

        // O(N)
        while(rangelen--)
        {
            addReplyBulk(c,ln->value);
            ln = ln->next;
        }
    }
    else
    {
        redisPanic("List encoding is not LINKEDLIST nor ZIPLIST!");
    }
}

/*
 * T = O(N^2)
 */
void ltrimCommand(redisClient *c)
{
    robj *o;
    long start, end, llen, j, ltrim, rtrim;
    list *list;
    listNode *ln;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
            (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
            checkType(c,o,REDIS_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen)
    {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    }
    else
    {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    // ɾ��
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // O(N^2)
        o->ptr = ziplistDeleteRange(o->ptr,0,ltrim);
        // O(N^2)
        o->ptr = ziplistDeleteRange(o->ptr,-rtrim,rtrim);
    }
    else if (o->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        list = o->ptr;
        // �ӱ�ͷ���βɾ��, O(N)
        for (j = 0; j < ltrim; j++)
        {
            ln = listFirst(list);
            listDelNode(list,ln);
        }
        // �ӱ�β���ͷɾ��, O(N)
        for (j = 0; j < rtrim; j++)
        {
            ln = listLast(list);
            listDelNode(list,ln);
        }
    }
    else
    {
        redisPanic("Unknown list encoding");
    }

    // �б�Ϊ�գ�ɾ����
    if (listTypeLength(o) == 0) dbDelete(c->db,c->argv[1]);

    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

/*
 * T = O(N^3)
 */
void lremCommand(redisClient *c)
{
    robj *subject, *obj;

    // obj ��Ҫɾ����Ŀ�����
    obj = c->argv[3] = tryObjectEncoding(c->argv[3]);

    long toremove;
    long removed = 0;
    listTypeEntry entry;

    // toremove ����ɾ��ֵ�ķ�ʽ
    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != REDIS_OK))
        return;

    // ��ȡ���󣬻��߷��ؿ�ֵ
    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    // ���ͼ��
    if (subject == NULL || checkType(c,subject,REDIS_LIST)) return;

    /* Make sure obj is raw when we're dealing with a ziplist */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        obj = getDecodedObject(obj);

    // ���� toremove �������ǵ����������ķ�ʽ����ͷ��β���ߴ�β��ͷ��
    listTypeIterator *li;
    if (toremove < 0)
    {
        // �޸�Ϊ����ֵ
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,REDIS_HEAD);
    }
    else
    {
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
    }

    // ����, O(N)
    while (listTypeNext(li,&entry))
    {
        if (listTypeEqual(&entry,obj))
        {
            // ɾ��, O(N^2)
            listTypeDelete(&entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    /* Clean up raw encoded object */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        decrRefCount(obj);

    // �б�Ϊ�գ�ɾ����
    if (listTypeLength(subject) == 0) dbDelete(c->db,c->argv[1]);

    addReplyLongLong(c,removed);
    if (removed) signalModifiedKey(c->db,c->argv[1]);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */

/*
 * �� value ��ӵ� dstkey �б���
 * ��� dstkey Ϊ�գ���ô����һ�����б�Ȼ��ִ����Ӷ���
 *
 * T = O(N^2)
 */
void rpoplpushHandlePush(redisClient *c, robj *dstkey, robj *dstobj, robj *value)
{
    /* Create the list if the key does not exist */
    // �б����ڣ������б�
    if (!dstobj)
    {
        // ���� ziplist
        dstobj = createZiplistObject();
        // ��ӵ� db
        dbAdd(c->db,dstkey,dstobj);
        // �� dstkey ��ӵ� server.ready_keys �б���
        signalListAsReady(c,dstkey);
    }

    signalModifiedKey(c->db,dstkey);

    // ��� value �� dstobj
    // O(N^2)
    listTypePush(dstobj,value,REDIS_HEAD);

    /* Always send the pushed value to the client. */
    addReplyBulk(c,value);
}

/*
 * T = O(N^2)
 */
void rpoplpushCommand(redisClient *c)
{
    robj *sobj, *value;

    // ��ȡԴ���󣬶��󲻴����򷵻� NULL
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
            checkType(c,sobj,REDIS_LIST)) return;

    // �б�Ϊ�գ�
    if (listTypeLength(sobj) == 0)
    {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        addReply(c,shared.nullbulk);
    }
    else
    {
        // �б�ǿ�

        // ��ȡĿ�����
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        // Դ����� key
        robj *touchedkey = c->argv[1];

        // ��Ŀ�����������ͼ��
        if (dobj && checkType(c,dobj,REDIS_LIST)) return;

        // ����Ŀ�����ı�βֵ, O(1)
        value = listTypePop(sobj,REDIS_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        incrRefCount(touchedkey);
        // O(N^2)
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        if (listTypeLength(sobj) == 0) dbDelete(c->db,touchedkey);

        signalModifiedKey(c->db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is how the current blocking POP works, we use BLPOP as example:
 * �������������������ԭ���� BLPOP Ϊ���ӣ�
 *
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - ��� BLPOP �����ã����Ҹ��� key ��Ϊ�գ���ôֱ�ӵ��� POP ��
 *
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - ��� BLPOP �����ã��� key �����ڻ��б�Ϊ�գ���ô�Կͻ��˽���������
 *   �ڶԿͻ��˽�������ʱ��ֻ�����������ݿɶ�������£�����ͻ��˷���֪ͨ��
 *   �������Ϳ�����û����������ʱ�����������ͻ��˽��д�����
 *   ���⻹��һ�� client �� key ��ӳ����ӵ������� key ��ɵ��������棬
 *   ������� key Ϊ�����������ֵ� db->blocking_keys ��
 *
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 *   һ��ĳ����ɿͻ��������� key ������ PUSH ������
 *   ��ô����� key ���Ϊ���������������������/����/�ű�ִ����֮��
 *   ���������ȷ����˳�򣬴������������ key ���������Ŀͻ��ˡ�
 */

/* Set a client in blocking mode for the specified key, with the specified
 * timeout */
/*
 * ���ݸ��������� key ���Ը����ͻ��˽�������
 *
 * ������
 *  keys    ��� key
 *  numkeys key ������
 *  timeout �������ʱ��
 *  target  �ڽ������ʱ����������浽��� key ���󣬶����Ƿ��ظ��ͻ���
 *          ֻ���� BRPOPLPUSH ����
 *
 * T = O(N)
 */
void blockForKeys(redisClient *c, robj **keys, int numkeys, time_t timeout, robj *target)
{
    dictEntry *de;
    list *l;
    int j;

    // ��������״̬�ĳ�ʱ��Ŀ��ѡ��
    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (target != NULL) incrRefCount(target);

    // ������ key ���뵽 client.bpop.keys �ֵ��O(N)
    for (j = 0; j < numkeys; j++)
    {
        /* If the key already exists in the dict ignore it. */
        // ��¼���� key ���ͻ���, O(1)
        if (dictAdd(c->bpop.keys,keys[j],NULL) != DICT_OK) continue;
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        // ���������Ŀͻ�����ӵ� db->blocking_keys �ֵ��������
        // O(1)
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL)
        {
            // ��� key ��һ�α�����������һ������
            int retval;

            /* For every key we take a list of clients blocked for it */
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            redisAssertWithInfo(c,keys[j],retval == DICT_OK);
        }
        else
        {
            // �Ѿ��������ͻ��˱���� key ����
            l = dictGetVal(de);
        }
        // ��������
        listAddNodeTail(l,c);
    }

    /* Mark the client as a blocked client */
    // ���ͻ��˵�״̬����Ϊ����
    c->flags |= REDIS_BLOCKED;

    // Ϊ�������������ͻ���������һ
    server.bpop_blocked_clients++;
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP */
/*
 * ȡ���ͻ��˵�����״̬
 *
 * T = O(N)
 */
void unblockClientWaitingData(redisClient *c)
{
    dictEntry *de;
    dictIterator *di;
    list *l;

    redisAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);

    /* The client may wait for multiple keys, so unblock it for every key. */
    // �������� key �������Ǵӿͻ��� db->blocking_keys ���������Ƴ�
    // O(N)
    di = dictGetIterator(c->bpop.keys);
    while((de = dictNext(di)) != NULL)
    {
        robj *key = dictGetKey(de);

        /* Remove this client from the list of clients waiting for this key. */
        // ��ȡ���� key �����пͻ�������
        l = dictFetchValue(c->db->blocking_keys,key);
        redisAssertWithInfo(c,key,l != NULL);
        // �����ͻ��˴Ӹ��������Ƴ�
        listDelNode(l,listSearchKey(l,c));
        /* If the list is empty we need to remove it to avoid wasting memory */
        // ���û�������ͻ������������ key �ϣ���ôɾ���������
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    // ��� bpop.keys �ֵ�
    dictEmpty(c->bpop.keys);
    if (c->bpop.target)
    {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }

    // ȡ���ͻ��˵�����״̬
    c->flags &= ~REDIS_BLOCKED;
    c->flags |= REDIS_UNBLOCKED;

    server.bpop_blocked_clients--;

    // ���ͻ�����ӵ���һ���¼� loop ǰ��
    // Ҫȡ�������Ŀͻ����б���
    listAddNodeTail(server.unblocked_clients,c);
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is an hash table that allows us to avoid putting
 * the same key agains and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnLists()
 *
 * ����пͻ�������Ϊ�ȴ����� key �� push ��������
 * ��ô����� key �����÷Ž� server.ready_keys �б����档
 *
 * ע�� db->ready_keys ��һ����ϣ��
 * ����Ա�����������߽ű��У���ͬһ�� key һ����һ����ӵ��б��������֡�
 *
 * �б����ջᱻ handleClientsBlockedOnLists() ��������
 *
 * T = O(1)
 */
void signalListAsReady(redisClient *c, robj *key)
{
    readyList *rl;

    /* No clients blocking for this key? No need to queue it. */
    // û�пͻ����ڵȴ���� key ��ֱ�ӷ���
    // O(1)
    if (dictFind(c->db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    // key �Ѿ�λ�ھ����б�ֱ�ӷ���
    // O(1)
    if (dictFind(c->db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    // ��Ӱ��� key ���� db ��Ϣ�� readyList �ṹ���������˵ľ����б�
    // O(1)
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = c->db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    // ͬʱ�� key ��ӵ� db �� ready_keys �ֵ���
    // �ṩ O(1) ���Ӷ�����ѯĳ�� key �Ƿ��Ѿ�����
    incrRefCount(key);
    redisAssert(dictAdd(c->db->ready_keys,key,NULL) == DICT_OK);
}

/* This is an helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * �����Ա������Ŀͻ��� receiver ����������� key �� key ���ڵ����ݿ� db
 * �Լ�һ��ֵ value ��һ��λ��ֵ where ִ�����¶�����
 *
 * 1) Provide the client with the 'value' element.
 *    �� value �ṩ�� receiver
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destionation list (the LPUSH side of the command).
 *    ��� dstkey ��Ϊ�գ�BRPOPLPUSH���������
 *    ��ôҲ�� value ���뵽 dstkey ָ�����б��С�
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *    �� BRPOP �� BLPOP �Ϳ����е� LPUSH ������ AOF ��ͬ���ڵ�
 *
 * The argument 'where' is REDIS_TAIL or REDIS_HEAD, and indicates if the
 * 'value' element was popped fron the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 * where ������ REDIS_TAIL ���� REDIS_HEAD ������ʶ��� value �Ǵ��Ǹ��ط� POP
 * �����������������������ͬ������ BLPOP ���� BRPOP ��
 *
 * The function returns REDIS_OK if we are able to serve the client, otherwise
 * REDIS_ERR is returned to signal the caller that the list POP operation
 * should be undoed as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type.
 * ���һ�гɹ������� REDIS_OK ��
 * ���ִ��ʧ�ܣ���ô���� REDIS_ERR ���� Redis ������Ŀ��ڵ�� POP ������
 * ʧ�ܵ����ֻ������� BRPOPLPUSH �����У�
 * ���� POP �б�ɹ���ȴ������ PUSH ��Ŀ�겻���б�ʱ��
 *
 * T = O(N^2)
 */
int serveClientBlockedOnList(redisClient *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    // ���� BLPOPRPUSH ��
    if (dstkey == NULL)
    {
        /* Propagate the [LR]POP operation. */
        // ���� [LR]POP ����
        argv[0] = (where == REDIS_HEAD) ? shared.lpop :
                  shared.rpop;
        argv[1] = key;
        // O(N)
        propagate((where == REDIS_HEAD) ?
                  server.lpopCommand : server.rpopCommand,
                  db->id,argv,2,REDIS_PROPAGATE_AOF|REDIS_PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        // �ظ��ͻ���
        addReplyMultiBulkLen(receiver,2);
        addReplyBulk(receiver,key);     // ���� value �� key
        addReplyBulk(receiver,value);   // value
    }
    else
    {
        /* BRPOPLPUSH */

        // ��ȡ dstkey ����
        robj *dstobj = lookupKeyWrite(receiver->db,dstkey);

        // ����Ϊ�գ���������ȷ��
        if (!(dstobj &&
                checkType(receiver,dstobj,REDIS_LIST)))
        {
            /* Propagate the RPOP operation. */
            // ���� RPOP ����
            argv[0] = shared.rpop;
            argv[1] = key;
            propagate(server.rpopCommand,
                      db->id,argv,2,
                      REDIS_PROPAGATE_AOF|
                      REDIS_PROPAGATE_REPL);
            // �� value ��ӵ� dstkey �б���
            // ��� dstkey �����ڣ���ô����һ�����б�
            // Ȼ�������Ӳ���
            // O(N^2)
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                                value);
            /* Propagate the LPUSH operation. */
            // ���� LPUSH ����
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            // O(N)
            propagate(server.lpushCommand,
                      db->id,argv,3,
                      REDIS_PROPAGATE_AOF|
                      REDIS_PROPAGATE_REPL);
        }
        else
        {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client.
 *
 * �����������ÿ�οͻ���ִ�е�������/����/�ű�����֮�󱻵��á�
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some PUSH operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BRPOPLPUSH we can have new blocking clients
 * to serve because of the PUSH side of BRPOPLPUSH.
 *
 * �����б�������ĳ���ͻ��˵� key ��˵��ֻҪ��� key ��ִ����ĳ�� PUSH ����
 * ��ô��� key �ͻᱻ�ŵ� serve.ready_keys ȥ��
 *
 * ���������������� serve.ready_keys ������������� key ���д���
 *
 * ������һ����һ�εؽ��е�����
 * �������ִ�� BRPOPLPUSH ����������Ҳ����������ȡ����ȷ���±������ͻ��ˡ�
 */
void handleClientsBlockedOnLists(void)
{
    // ����ֱ�������б�Ϊ��Ϊֹ��O(N^3)
    while(listLength(server.ready_keys) != 0)
    {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalListAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BRPOPLPUSH. */
        // ���ݾɵ� ready_keys ���ٸ��������˸�ֵһ���µ�
        l = server.ready_keys;
        server.ready_keys = listCreate();

        // �������� ready_keys ����O(N^2)
        while(listLength(l) != 0)
        {
            //
            listNode *ln = listFirst(l);
            // ��ȡԪ�ص�ֵ��һ�������������� key �� db �� readyList
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalListAsReady() against this key. */
            // �� db->ready_keys ��ɾ������ key
            dictDelete(rl->db->ready_keys,rl->key);

            /* If the key exists and it's a list, serve blocked clients
             * with data. */
            // ��ȡ key ����
            robj *o = lookupKeyWrite(rl->db,rl->key);
            // ����Ϊ�������б�
            if (o != NULL && o->type == REDIS_LIST)
            {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                // ȡ�������а��������б����� key �����Ŀͻ���
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de)
                {
                    // ����������Ϊ key �������Ŀͻ���
                    list *clients = dictGetVal(de);
                    // ���㱻�����ͻ��˵�����
                    int numclients = listLength(clients);

                    // �������пͻ��ˣ�Ϊ����ȡ������ key ��ֵ
                    // ֱ������ key ��ֵ��ȫ��ȡ����
                    // �������пͻ��˶���������Ϊֹ
                    while(numclients--)
                    {
                        // ȡ�������е��׸��ͻ���
                        listNode *clientnode = listFirst(clients);
                        redisClient *receiver = clientnode->value;

                        // ���õ�����Ŀ�ֻ꣨���� BRPOPLPUSH��
                        robj *dstkey = receiver->bpop.target;

                        // Ҫ����Ԫ�ص�λ��
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == blpopCommand) ?
                                    REDIS_HEAD : REDIS_TAIL;

                        // ��������ֵ
                        robj *value = listTypePop(o,where);

                        // ����б��ﻹ��ֵ���Ա������Ļ�
                        // ��ô�������浽 dstkey �����߷��ظ��ͻ���
                        if (value)
                        {
                            /* Protect receiver->bpop.target, that will be
                             * freed by the next unblockClientWaitingData()
                             * call. */
                            if (dstkey) incrRefCount(dstkey);

                            // ȡ�� receiver �ͻ��˵�����״̬
                            unblockClientWaitingData(receiver);

                            // ��ֵ value ��ӵ�
                            // ��ɿͻ��� receiver ������ key ��
                            if (serveClientBlockedOnList(
                                        receiver,   // �������Ŀͻ���
                                        rl->key,    // ��������� key
                                        dstkey,     // Ŀ�� key �������� BRPOPLPUSH��
                                        rl->db,     // ���ݿ�
                                        value,      // ֵ
                                        where) == REDIS_ERR)
                            {
                                /* If we failed serving the client we need
                                 * to also undo the POP operation. */
                                // �������ʧ�ܣ���Ҫ���½� POP ������
                                // Ԫ�� PUSH ��ȥ
                                listTypePush(o,value,where);
                            }

                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        }
                        else
                        {
                            // ���ֿͻ���û��ȡ��ֵ��������Ȼ��Ҫ����
                            break;
                        }
                    }
                }

                // ��� key �Ѿ�Ϊ�գ���ôɾ����
                if (listTypeLength(o) == 0) dbDelete(rl->db,rl->key);
                /* We don't call signalModifiedKey() as it was already called
                 * when an element was pushed on the list. */
            }

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }
}

int getTimeoutFromObjectOrReply(redisClient *c, robj *object, time_t *timeout)
{
    long tval;

    if (getLongFromObjectOrReply(c,object,&tval,
                                 "timeout is not an integer or out of range") != REDIS_OK)
        return REDIS_ERR;

    if (tval < 0)
    {
        addReplyError(c,"timeout is negative");
        return REDIS_ERR;
    }

    if (tval > 0) tval += server.unixtime;
    *timeout = tval;

    return REDIS_OK;
}

/* Blocking RPOP/LPOP */
/*
 * BLPOP/BRPOP �ĵײ�ʵ��
 */
void blockingPopGenericCommand(redisClient *c, int where)
{
    robj *o;
    time_t timeout;
    int j;

    // ��ȡ timeout ����
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout) != REDIS_OK)
        return;

    // �������� key
    // ����ҵ���һ����Ϊ�յ��б������ô�������� POP ��Ȼ�󷵻�
    for (j = 1; j < c->argc-1; j++)
    {

        o = lookupKeyWrite(c->db,c->argv[j]);

        // ����Ϊ�գ�
        if (o != NULL)
        {
            // ���ͼ��
            if (o->type != REDIS_LIST)
            {
                addReply(c,shared.wrongtypeerr);
                return;
            }
            else
            {
                // key Ϊ�ǿ��б�
                if (listTypeLength(o) != 0)
                {
                    /* Non empty list, this is like a non normal [LR]POP. */
                    // �ǿ��б�ִ����ͨ�� [LR]POP
                    robj *value = listTypePop(o,where);
                    redisAssert(value != NULL);

                    addReplyMultiBulkLen(c,2);
                    addReplyBulk(c,c->argv[j]); // key
                    addReplyBulk(c,value);      // value

                    decrRefCount(value);

                    // ɾ�����б�
                    if (listTypeLength(o) == 0) dbDelete(c->db,c->argv[j]);

                    signalModifiedKey(c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    rewriteClientCommandVector(c,2,
                                               (where == REDIS_HEAD) ? shared.lpop : shared.rpop,
                                               c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    // ����������������������ôֻ�ܷ��صȴ���ʱ
    // ���������������������Ϊ����������һֱ�ȴ���ȥ��
    if (c->flags & REDIS_MULTI)
    {
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* If the list is empty or the key does not exists we must block */
    // ���и��� key ��Ϊ�գ����� block
    blockForKeys(c, c->argv + 1, c->argc - 2, timeout, NULL);
}

void blpopCommand(redisClient *c)
{
    blockingPopGenericCommand(c,REDIS_HEAD);
}

void brpopCommand(redisClient *c)
{
    blockingPopGenericCommand(c,REDIS_TAIL);
}

void brpoplpushCommand(redisClient *c)
{
    time_t timeout;

    // ��ȡ timeout ����
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout) != REDIS_OK)
        return;

    // ���� key ����
    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    // �����ڣ�
    if (key == NULL)
    {
        if (c->flags & REDIS_MULTI)
        {
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            addReply(c, shared.nullbulk);
        }
        else
        {
            /* The list is empty and the client blocks. */
            // ֱ�ӵȴ�Ԫ�� push �� key
            blockForKeys(c, c->argv + 1, 1, timeout, c->argv[2]);
        }
    }
    else
    {
        if (key->type != REDIS_LIST)
        {
            addReply(c, shared.wrongtypeerr);
        }
        else
        {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
            redisAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
