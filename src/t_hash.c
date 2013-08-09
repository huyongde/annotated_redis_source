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
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/*
 * �� argv �����еĶ�����м�飬
 * �����������Ƿ���Ҫ�� o �ı����
 * REDIS_ENCODING_ZIPLIST ת��Ϊ REDIS_ENCODING_HT
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end)
{
    int i;

    // ��������� ziplist ���루��hash����ֱ�ӷ���
    if (o->encoding != REDIS_ENCODING_ZIPLIST) return;

    // ��������ַ��������ĳ��ȣ����Ƿ񳬹� server.hash_max_ziplist_value
    // �����һ�����Ϊ��Ļ����Ͷ� o ����ת��
    for (i = start; i <= end; i++)
    {
        if (argv[i]->encoding == REDIS_ENCODING_RAW &&
                sdslen(argv[i]->ptr) > server.hash_max_ziplist_value)
        {
            // ת��
            hashTypeConvert(o, REDIS_ENCODING_HT);
            break;
        }
    }
}

/*
 * �� subject �� HASH ����ʱ�����Զ� o1 �� o2 ���о͵�(in-place)����
 * �ñ�����Գ��Խ�ʡ�ַ����Ŀռ�
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2)
{
    if (subject->encoding == REDIS_ENCODING_HT)
    {
        if (o1) *o1 = tryObjectEncoding(*o1);
        if (o2) *o2 = tryObjectEncoding(*o2);
    }
}

/* Get the value from a ziplist encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
/*
 * �� ziplist ��ȡ���� field ���Ӧ��ֵ
 *
 * ���Ӷȣ�O(n)
 *
 * ������
 *  field   ��
 *  vstr    ֵ���ַ���ʱ���������浽���ָ��
 *  vlen    �����ַ����ĳ���
 *  ll      ֵʱ����ʱ���������浽���ָ��
 *
 * ����ֵ��
 *  ����ʧ�ܷ��� -1 �����򷵻� 0 ��
 */
int hashTypeGetFromZiplist(robj *o, robj *field,
                           unsigned char **vstr,
                           unsigned int *vlen,
                           long long *vll)
{
    unsigned char *zl,
             *fptr = NULL,
              *vptr = NULL;
    int ret;

    redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);

    // ��������Ϊ ziplist ����ʹ�ö���
    field = getDecodedObject(field);

    // ���� ziplist ����λ���λ��
    zl = o->ptr;
    fptr = ziplistIndex(zl, ZIPLIST_HEAD);
    if (fptr != NULL)
    {
        // ��λ��ڵ��λ��
        fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
        if (fptr != NULL)
        {
            /* Grab pointer to the value (fptr points to the field) */
            // ��λֵ�ڵ��λ��
            vptr = ziplistNext(zl, fptr);
            redisAssert(vptr != NULL);
        }
    }

    decrRefCount(field);

    // �� ziplist �ڵ���ȡ��ֵ
    if (vptr != NULL)
    {
        ret = ziplistGet(vptr, vstr, vlen, vll);
        redisAssert(ret);
        return 0;
    }

    return -1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
/*
 * �� HT ����Ĺ�ϣ���л�ȡ���� field ��ֵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ������
 *  field   ��
 *  value   ����ֵ�����ָ��
 *
 * ����ֵ��
 *  �ҵ�ֵʱ���� 0 ��û�ҵ����� -1 ��
 */
int hashTypeGetFromHashTable(robj *o, robj *field, robj **value)
{
    dictEntry *de;

    redisAssert(o->encoding == REDIS_ENCODING_HT);

    de = dictFind(o->ptr, field);
    if (de == NULL) return -1;
    *value = dictGetVal(de);
    return 0;
}

/* Higher level function of hashTypeGet*() that always returns a Redis
 * object (either new or with refcount incremented), so that the caller
 * can retain a reference or call decrRefCount after the usage.
 *
 * The lower level function can prevent copy on write so it is
 * the preferred way of doing read operations. */
/*
 * ��̬��ȡ����
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��
 *  redisObject ������ field ���ڹ�ϣ���ж�Ӧ��ֵ
 */
robj *hashTypeGetObject(robj *o, robj *field)
{
    robj *value = NULL;

    // �� ziplist �л�ȡ
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0)
        {
            if (vstr)
            {
                // ������ֵ��װ�ɶ����ٷ���
                value = createStringObject((char*)vstr, vlen);
            }
            else
            {
                value = createStringObjectFromLongLong(vll);
            }
        }

        // ���ֵ��л�ȡ
    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        robj *aux;

        if (hashTypeGetFromHashTable(o, field, &aux) == 0)
        {
            incrRefCount(aux);
            value = aux;
        }

        // �������
    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return value;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
/*
 * ������ field �Ƿ������ hash ���� o �С�
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��
 *  ���ڷ��� 1 �� ���򷵻� 0 ��
 */
int hashTypeExists(robj *o, robj *field)
{

    // ��� ziplist
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) return 1;

        // ����ֵ�
    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        robj *aux;

        if (hashTypeGetFromHashTable(o, field, &aux) == 0) return 1;

        // �������
    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return 0;
}

/*
 * �������� field-value pair ��ӵ� hash ��
 *
 * ������������ field �� value �����������ü���������
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��
 *  ���� 0 ��ʾԪ���Ѵ��ڣ�����ֵ�ѱ����¡�
 *  ���� 1 ��ʾԪ��������ӵġ�
 */
int hashTypeSet(robj *o, robj *field, robj *value)
{
    int update = 0;

    // ��ӵ� ziplist
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl, *fptr, *vptr;

        // ������ַ�����������
        field = getDecodedObject(field);
        value = getDecodedObject(value);

        // �������� ziplist �����Բ��Ҳ����� field ��������Ѿ����ڣ�
        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL)
        {
            // ��λ����O(N)
            fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
            if (fptr != NULL)
            {
                /* Grab pointer to the value (fptr points to the field) */
                // ��λ��ֵ
                vptr = ziplistNext(zl, fptr);
                redisAssert(vptr != NULL);

                // ��ʶ��β���Ϊ���²���
                update = 1;

                // ɾ����ֵ
                zl = ziplistDelete(zl, &vptr);

                // ������ֵ
                zl = ziplistInsert(zl, vptr, value->ptr, sdslen(value->ptr));
            }
        }

        // ����ⲻ�Ǹ��²�������ô�����һ����Ӳ���
        if (!update)
        {
            // ���µ���/ֵ�� push �� ziplist ��ĩβ
            zl = ziplistPush(zl, field->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
            zl = ziplistPush(zl, value->ptr, sdslen(value->ptr), ZIPLIST_TAIL);
        }
        o->ptr = zl;
        decrRefCount(field);
        decrRefCount(value);

        /* Check if the ziplist needs to be converted to a hash table */
        if (hashTypeLength(o) > server.hash_max_ziplist_entries)
            hashTypeConvert(o, REDIS_ENCODING_HT);

        // ��ӵ��ֵ䣬O(1)
    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        if (dictReplace(o->ptr, field, value))   /* Insert */
        {
            incrRefCount(field);
        }
        else     /* Update */
        {
            update = 1;
        }
        incrRefCount(value);

        // �������
    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
/*
 * ������ field ���� value �ӹ�ϣ����ɾ��
 *
 * ���Ӷȣ�
 *  O(N)
 *
 * ����ֵ��
 *  ɾ���ɹ����� 1 �����򷵻� 0 ��
 */
int hashTypeDelete(robj *o, robj *field)
{
    int deleted = 0;

    // ziplist
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl, *fptr;

        field = getDecodedObject(field);

        // ���� ziplist ������ɾ�� field-value ��
        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL)
        {
            fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
            // �ҵ�Ŀ�� field
            if (fptr != NULL)
            {
                zl = ziplistDelete(zl,&fptr);
                zl = ziplistDelete(zl,&fptr);
                o->ptr = zl;
                deleted = 1;
            }
        }

        decrRefCount(field);

    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        if (dictDelete((dict*)o->ptr, field) == REDIS_OK)
        {
            deleted = 1;

            /* Always check if the dictionary needs a resize after a delete. */
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);
        }

    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return deleted;
}

/* Return the number of elements in a hash. */
/*
 * ���ع�ϣ��� field-value ������
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��
 *  length
 */
unsigned long hashTypeLength(robj *o)
{
    unsigned long length = ULONG_MAX;

    // ziplist
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // һ�� field-value ��ռ�������ڵ�
        length = ziplistLen(o->ptr) / 2;

        // dict
    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        length = dictSize((dict*)o->ptr);

        // wrong encoding
    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return length;
}

/*
 * ����һ����ϣ���͵ĵ�����
 * hashTypeIterator ���Ͷ����� redis.h
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��
 *  hashTypeIterator
 */
hashTypeIterator *hashTypeInitIterator(robj *subject)
{
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    // �������ͱ��뷽ʽ
    hi->subject = subject;
    hi->encoding = subject->encoding;

    // ziplist ����
    if (hi->encoding == REDIS_ENCODING_ZIPLIST)
    {
        hi->fptr = NULL;
        hi->vptr = NULL;

        // dict ����
    }
    else if (hi->encoding == REDIS_ENCODING_HT)
    {
        hi->di = dictGetIterator(subject->ptr);

    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return hi;
}

/*
 * �ͷŵ�����
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void hashTypeReleaseIterator(hashTypeIterator *hi)
{
    // �ͷ��ֵ������
    if (hi->encoding == REDIS_ENCODING_HT)
    {
        dictReleaseIterator(hi->di);
    }

    // �ͷ� ziplist �ĵ�����
    zfree(hi);
}

/* Move to the next entry in the hash. Return REDIS_OK when the next entry
 * could be found and REDIS_ERR when the iterator reaches the end. */
/*
 * ��ȡ hash �е���һ���ڵ㣬���������浽��������
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��
 *  �����ȡ�ɹ������� REDIS_OK ��
 *  ����Ѿ�û��Ԫ�ؿɻ�ȡ����ô���� REDIS_ERR ��
 */
int hashTypeNext(hashTypeIterator *hi)
{
    // ���� ziplist
    if (hi->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        // ��һ��ִ��ʱ����ʼ��ָ��
        if (fptr == NULL)
        {
            /* Initialize cursor */
            redisAssert(vptr == NULL);
            fptr = ziplistIndex(zl, 0);

            // ��ȡ��һ�������ڵ�
        }
        else
        {
            /* Advance cursor */
            redisAssert(vptr != NULL);
            fptr = ziplistNext(zl, vptr);
        }
        // ������
        if (fptr == NULL) return REDIS_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = ziplistNext(zl, fptr);
        redisAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;

        // �����ֵ�
    }
    else if (hi->encoding == REDIS_ENCODING_HT)
    {
        if ((hi->de = dictNext(hi->di)) == NULL) return REDIS_ERR;

        // wrong encoding
    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return REDIS_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a ziplist. Prototype is similar to `hashTypeGetFromZiplist`. */
/*
 * ���ݵ�������ָ�룬�� ziplist ��ȡ����ָ��Ľڵ� field ���� value ��
 *
 * ���Ӷȣ�O(1)
 */
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll)
{
    int ret;

    redisAssert(hi->encoding == REDIS_ENCODING_ZIPLIST);

    if (what & REDIS_HASH_KEY)
    {
        ret = ziplistGet(hi->fptr, vstr, vlen, vll);
        redisAssert(ret);
    }
    else
    {
        ret = ziplistGet(hi->vptr, vstr, vlen, vll);
        redisAssert(ret);
    }
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a ziplist. Prototype is similar to `hashTypeGetFromHashTable`. */
/*
 * ���ݵ�������ָ�룬���ֵ���ȡ����ָ��ڵ�� field ���� value ��
 *
 * ���Ӷȣ�O(1)
 */
void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, robj **dst)
{
    redisAssert(hi->encoding == REDIS_ENCODING_HT);

    if (what & REDIS_HASH_KEY)
    {
        *dst = dictGetKey(hi->de);
    }
    else
    {
        *dst = dictGetVal(hi->de);
    }
}

/* A non copy-on-write friendly but higher level version of hashTypeCurrent*()
 * that returns an object with incremented refcount (or a new object). It is up
 * to the caller to decrRefCount() the object if no reference is retained. */
/*
 * �ӵ�������ȡ����ǰֵ
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ�� robj һ�����浱ǰֵ�Ķ���
 */
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what)
{
    robj *dst;

    // ziplist
    if (hi->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr)
        {
            // ���Ƿ���ֵ����
            dst = createStringObject((char*)vstr, vlen);
        }
        else
        {
            dst = createStringObjectFromLongLong(vll);
        }

        // �ֵ�
    }
    else if (hi->encoding == REDIS_ENCODING_HT)
    {
        hashTypeCurrentFromHashTable(hi, what, &dst);
        incrRefCount(dst);

    }
    else
    {
        redisPanic("Unknown hash encoding");
    }

    return dst;
}

/*
 * �� key ���� hash ����
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��
 *  ������󲻴��ڣ��ʹ���һ���µ� hash ����������
 *  ��������� hash ����ô���ش���
 */
robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key)
{
    // �����ݿ��в���
    robj *o = lookupKeyWrite(c->db,key);

    // o �����ڣ�����һ���µ� hash
    if (o == NULL)
    {

        o = createHashObject();
        dbAdd(c->db,key,o);
        // o ����
    }
    else
    {
        // o ���� hash ������
        if (o->type != REDIS_HASH)
        {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }

    return o;
}

/*
 * ��һ�� ziplist ����Ĺ�ϣ���� o ת������������
 * ������ dict��
 *
 * ���Ӷȣ�O(N)
 */
void hashTypeConvertZiplist(robj *o, int enc)
{
    redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);

    if (enc == REDIS_ENCODING_ZIPLIST)
    {
        /* Nothing to do... */

    }
    else if (enc == REDIS_ENCODING_HT)
    {
        hashTypeIterator *hi;
        dict *dict;
        int ret;

        // ���� o �ĵ�����
        hi = hashTypeInitIterator(o);
        // �������ֵ�
        dict = dictCreate(&hashDictType, NULL);

        // �������� ziplist
        while (hashTypeNext(hi) != REDIS_ERR)
        {
            robj *field, *value;

            // ȡ�� ziplist ��ļ�
            field = hashTypeCurrentObject(hi, REDIS_HASH_KEY);
            field = tryObjectEncoding(field);

            // ȡ�� ziplist ���ֵ
            value = hashTypeCurrentObject(hi, REDIS_HASH_VALUE);
            value = tryObjectEncoding(value);

            // ����ֵ����ӵ��ֵ�
            ret = dictAdd(dict, field, value);
            if (ret != DICT_OK)
            {
                redisLogHexDump(REDIS_WARNING,"ziplist with dup elements dump",
                                o->ptr,ziplistBlobLen(o->ptr));
                redisAssert(ret == DICT_OK);
            }
        }

        // �ͷ� ziplist �ĵ�����
        hashTypeReleaseIterator(hi);
        // �ͷ� ziplist
        zfree(o->ptr);

        // ���� key ����ı����ֵ
        o->encoding = REDIS_ENCODING_HT;
        o->ptr = dict;

    }
    else
    {
        redisPanic("Unknown hash encoding");
    }
}

/*
 * �� hash ���� o �ı��뷽ʽ����ת��
 *
 * Ŀǰֻ֧�ִ� ziplist ת��Ϊ dict
 *
 * ���Ӷȣ�O(N)
 */
void hashTypeConvert(robj *o, int enc)
{
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        hashTypeConvertZiplist(o, enc);
    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        redisPanic("Not implemented");
    }
    else
    {
        redisPanic("Unknown hash encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

/*
 * HSET �����ʵ��
 *
 * T = O(N)
 */
void hsetCommand(redisClient *c)
{
    int update;
    robj *o;

    // ���� hash
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    // ������������������Ҫ�Ļ����� o ת��Ϊ dict ����
    hashTypeTryConversion(o,c->argv,2,3);

    // ���� field �� value �Խ�ʡ�ռ�
    hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);

    // ���� field �� value �� hash
    // O(N)
    update = hashTypeSet(o,c->argv[2],c->argv[3]);

    // ����״̬������/�����
    addReply(c, update ? shared.czero : shared.cone);

    signalModifiedKey(c->db,c->argv[1]);

    server.dirty++;
}

/*
 * HSETNX �����ʵ��
 *
 * T = O(N)
 */
void hsetnxCommand(redisClient *c)
{
    robj *o;
    // ��������Ҹ��� key ����
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    // ������������м�飬���Ƿ���Ҫ�� o ת��Ϊ�ֵ����
    // O(N)
    hashTypeTryConversion(o,c->argv,2,3);

    if (hashTypeExists(o, c->argv[2]))
    {
        // ��� field �Ѵ��ڣ�ֱ�ӷ���
        addReply(c, shared.czero);
    }
    else
    {
        // δ���ڣ���������

        hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);

        hashTypeSet(o,c->argv[2],c->argv[3]);

        addReply(c, shared.cone);

        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

/*
 * HMSET �����ʵ��
 *
 * T = O(N^2)
 */
void hmsetCommand(redisClient *c)
{
    int i;
    robj *o;

    // �����ĸ��������ǳ�˫�ɶԵ�
    if ((c->argc % 2) == 1)
    {
        addReplyError(c,"wrong number of arguments for HMSET");
        return;
    }

    // ���һ򴴽� hash ����
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    // �������������������Ƿ���Ҫ�� hash �ı���ת��Ϊ�ֵ�
    hashTypeTryConversion(o,c->argv,2,c->argc-1);

    // ��������
    for (i = 2; i < c->argc; i += 2)
    {
        // �� field �� value ���б���
        hashTypeTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
        // ���ã�O(N)
        hashTypeSet(o,c->argv[i],c->argv[i+1]);
    }

    addReply(c, shared.ok);

    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
}

/*
 * HINCRBY �����ʵ��
 *
 * T = O(N)
 */
void hincrbyCommand(redisClient *c)
{
    long long value, incr, oldvalue;
    robj *o, *current, *new;

    // ȡ����������
    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;

    // ���һ򴴽� hash ����
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    // field �Ѵ��ڣ���O(N)
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL)
    {
        // ��ȡ field ��ֵ
        if (getLongLongFromObjectOrReply(c,current,&value,
                                         "hash value is not an integer") != REDIS_OK)
        {
            decrRefCount(current);
            return;
        }
        decrRefCount(current);
    }
    else
    {
        // field-value �Բ����ڣ�����ֵΪ 0
        value = 0;
    }

    // ���ֵ�Ƿ����
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
            (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue)))
    {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }

    // �����
    value += incr;

    // ���ͱ��浽 sds
    new = createStringObjectFromLongLong(value);

    // �� field ���б���
    hashTypeTryObjectEncoding(o,&c->argv[2],NULL);

    // ���� field-value ��
    hashTypeSet(o,c->argv[2],new);

    // ������һ�� hashTypeSet �ᴦ�� field �� value �ļ�����
    decrRefCount(new);

    addReplyLongLong(c,value);
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
}

/*
 * HINCRBYFLOAT �����ʵ��
 *
 * T = O(N)
 */
void hincrbyfloatCommand(redisClient *c)
{
    double long value, incr;
    robj *o, *current, *new, *aux;

    // ȡ������
    if (getLongDoubleFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;

    // ����������Ժ�����
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    // ȡ����ǰֵ��O(N)
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL)
    {
        // ���ֵ���ܱ���ʾΪ����������ôֱ��ʧ��
        if (getLongDoubleFromObjectOrReply(c,current,&value,
                                           "hash value is not a valid float") != REDIS_OK)
        {
            decrRefCount(current);
            return;
        }
        decrRefCount(current);
    }
    else
    {
        value = 0;
    }

    // ������ֵ
    value += incr;
    // ������ֵ
    new = createStringObjectFromLongDouble(value);
    hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    hashTypeSet(o,c->argv[2],new);

    addReplyBulk(c,new);
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;

    /* Always replicate HINCRBYFLOAT as an HSET command with the final value
     * in order to make sure that differences in float pricision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("HSET",4);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,3,new);
    decrRefCount(new);
}

/*
 * �� hash �� field ��ֵ��ӵ��ظ���
 * ��������ķ����ֶ�
 *
 * T = O(N)
 */
static void addHashFieldToReply(redisClient *c, robj *o, robj *field)
{
    int ret;

    if (o == NULL)
    {
        addReply(c, shared.nullbulk);
        return;
    }

    // ziplist
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // ȡ��ֵ��O(N)
        ret = hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll);
        if (ret < 0)
        {
            addReply(c, shared.nullbulk);
        }
        else
        {
            if (vstr)
            {
                addReplyBulkCBuffer(c, vstr, vlen);
            }
            else
            {
                addReplyBulkLongLong(c, vll);
            }
        }

        // �ֵ�
    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        robj *value;

        // ȡ��ֵ��O(1)
        ret = hashTypeGetFromHashTable(o, field, &value);
        if (ret < 0)
        {
            addReply(c, shared.nullbulk);
        }
        else
        {
            addReplyBulk(c, value);
        }

    }
    else
    {
        redisPanic("Unknown hash encoding");
    }
}

/*
 * HGET �����ʵ��
 *
 * T = O(1)
 */
void hgetCommand(redisClient *c)
{
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
            checkType(c,o,REDIS_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]);
}

/*
 * HMGET �����ʵ��
 *
 * T = O(N^2)
 */
void hmgetCommand(redisClient *c)
{
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    // ��ȡ�򴴽�һ���ֵ�
    o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && o->type != REDIS_HASH)
    {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    // ��ȡ field ��ֵ
    addReplyMultiBulkLen(c, c->argc-2);
    for (i = 2; i < c->argc; i++)
    {
        addHashFieldToReply(c, o, c->argv[i]);
    }
}

/*
 * HDEL �����ʵ��
 *
 * T = O(N^2)
 */
void hdelCommand(redisClient *c)
{
    robj *o;
    int j, deleted = 0;

    // ��ȡ�򴴽�һ���ֵ�
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,o,REDIS_HASH)) return;

    for (j = 2; j < c->argc; j++)
    {
        // ɾ��
        if (hashTypeDelete(o,c->argv[j]))
        {
            // ����
            deleted++;

            // ��� hash �Ѿ�Ϊ�գ���ôɾ����
            if (hashTypeLength(o) == 0)
            {
                dbDelete(c->db,c->argv[1]);
                break;
            }
        }
    }

    // �������ɾ����һ�� field-value ��
    // ��ô֪ͨ db ���ѱ��޸�
    if (deleted)
    {
        signalModifiedKey(c->db,c->argv[1]);
        server.dirty += deleted;
    }

    addReplyLongLong(c,deleted);
}

/*
 * HLEN �����ʵ��
 *
 * T = O(1)
 */
void hlenCommand(redisClient *c)
{
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,o,REDIS_HASH)) return;

    addReplyLongLong(c,hashTypeLength(o));
}

/*
 * ȡ����ǰ hash �ڵ�� field ����  value ��
 *
 * T = O(1)
 */
static void addHashIteratorCursorToReply(redisClient *c, hashTypeIterator *hi, int what)
{
    if (hi->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // �� ziplist �Ľڵ���ȡ�� field ����Ӧ��ֵ
        // O(1)
        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr)
        {
            addReplyBulkCBuffer(c, vstr, vlen);
        }
        else
        {
            addReplyBulkLongLong(c, vll);
        }

    }
    else if (hi->encoding == REDIS_ENCODING_HT)
    {
        robj *value;

        // ���ֵ���ȡ�� field ��ֵ
        // O(1)
        hashTypeCurrentFromHashTable(hi, what, &value);
        addReplyBulk(c, value);

    }
    else
    {
        redisPanic("Unknown hash encoding");
    }
}

/*
 * GETALL ������ĵײ�ʵ��
 *
 * T = O(N)
 */
void genericHgetallCommand(redisClient *c, int flags)
{
    robj *o;
    hashTypeIterator *hi;
    int multiplier = 0;
    int length, count = 0;

    // ��������� hash
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
            || checkType(c,o,REDIS_HASH)) return;

    // ѡ��÷�����Щ����
    if (flags & REDIS_HASH_KEY) multiplier++;
    if (flags & REDIS_HASH_VALUE) multiplier++;

    // ����Ҫ���ص����ݣ�����Ҫ��ȡ��Ԫ�ص�����
    // O(1)
    length = hashTypeLength(o) * multiplier;
    addReplyMultiBulkLen(c, length);

    // ��ʼ����
    // O(N)
    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != REDIS_ERR)
    {
        if (flags & REDIS_HASH_KEY)
        {
            addHashIteratorCursorToReply(c, hi, REDIS_HASH_KEY);
            count++;
        }
        if (flags & REDIS_HASH_VALUE)
        {
            addHashIteratorCursorToReply(c, hi, REDIS_HASH_VALUE);
            count++;
        }
    }

    // �ͷŵ�����
    hashTypeReleaseIterator(hi);

    redisAssert(count == length);
}

/*
 * HKEYS �����ʵ��
 *
 * T = O(N)
 */
void hkeysCommand(redisClient *c)
{
    // ֵȡ����
    genericHgetallCommand(c,REDIS_HASH_KEY);
}

/*
 * HVALS �����ʵ��
 *
 * T = O(N)
 */
void hvalsCommand(redisClient *c)
{
    // ֻȡ��ֵ
    genericHgetallCommand(c,REDIS_HASH_VALUE);
}

/*
 * HGETALL �����ʵ��
 */
void hgetallCommand(redisClient *c)
{
    // ȡ������ֵ
    genericHgetallCommand(c,REDIS_HASH_KEY|REDIS_HASH_VALUE);
}

/*
 * HEXISTS �����ʵ��
 *
 * ���Ӷȣ�O(N)
 */
void hexistsCommand(redisClient *c)
{
    robj *o;

    // ȡ������
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,o,REDIS_HASH)) return;

    // ���Ը������Ƿ����
    addReply(c, hashTypeExists(o,c->argv[2]) ? shared.cone : shared.czero);
}
