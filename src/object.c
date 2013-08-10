/* Redis Object implementation.
 *
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
#include <ctype.h>

/*
 * ���ݸ������ͺ�ֵ�������¶���
 */
robj *createObject(int type, void *ptr)
{

    // ����ռ�
    robj *o = zmalloc(sizeof(*o));

    // ��ʼ��������
    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;   // Ĭ�ϱ���
    o->ptr = ptr;
    o->refcount = 1;

    /* Set the LRU to the current lruclock (minutes resolution). */
    o->lru = server.lruclock;

    return o;
}

/*
 * ���ݸ����ַ����飬����һ�� String ����
 */
robj *createStringObject(char *ptr, size_t len)
{
    return createObject(REDIS_STRING,sdsnewlen(ptr,len));
}

/*
 * ���ݸ�������ֵ value ������һ�� String ����
 */
robj *createStringObjectFromLongLong(long long value)
{

    robj *o;

    if (value >= 0 && value < REDIS_SHARED_INTEGERS)
    {
        // �����������ʹ�ù������
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    }
    else
    {
        // ���򣬴����� String ����
        if (value >= LONG_MIN && value <= LONG_MAX)
        {
            // long ���͵�����ֵ�� long ���ͱ���
            o = createObject(REDIS_STRING, NULL);
            o->encoding = REDIS_ENCODING_INT;   // ���ñ���
            o->ptr = (void*)((long)value);
        }
        else
        {
            // long long ���͵�����ֵ������ַ���������
            o = createObject(REDIS_STRING,sdsfromlonglong(value));
        }
    }

    return o;
}

/* Note: this function is defined into object.c since here it is where it
 * belongs but it is actually designed to be used just for INCRBYFLOAT */
/*
 * ���ݸ��� long double ֵ value ������ String ����
 */
robj *createStringObjectFromLongDouble(long double value)
{
    char buf[256];
    int len;

    /* We use 17 digits precision since with 128 bit floats that precision
     * after rouding is able to represent most small decimal numbers in a way
     * that is "non surprising" for the user (that is, most small decimal
     * numbers will be represented in a way that when converted back into
     * a string are exactly the same as what the user typed.) */
    // �� long double ֵת��Ϊ�ַ���
    len = snprintf(buf,sizeof(buf),"%.17Lf", value);
    /* Now remove trailing zeroes after the '.' */
    if (strchr(buf,'.') != NULL)
    {
        char *p = buf+len-1;
        while(*p == '0')
        {
            p--;
            len--;
        }
        if (*p == '.') len--;
    }
    return createStringObject(buf,len);
}

/*
 * ����һ�� String ����ĸ���
 */
robj *dupStringObject(robj *o)
{
    redisAssertWithInfo(NULL,o,o->encoding == REDIS_ENCODING_RAW);
    return createStringObject(o->ptr,sdslen(o->ptr));
}

/*
 * ����һ�� list ����
 */
robj *createListObject(void)
{
    // ʹ��˫������(adlist)
    list *l = listCreate();

    // ��������
    robj *o = createObject(REDIS_LIST,l);

    // ��Ϊ�б����ֵҲ����������
    // ����Ҫ���� decrRefCount ��Ϊֵ���ͷ���
    listSetFreeMethod(l,decrRefCount);

    // ���ñ���
    o->encoding = REDIS_ENCODING_LINKEDLIST;

    return o;
}

/*
 * ����һ�� ziplist ����
 */
robj *createZiplistObject(void)
{
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_LIST,zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}

/*
 * ����һ�� set ����
 */
robj *createSetObject(void)
{
    dict *d = dictCreate(&setDictType,NULL);
    robj *o = createObject(REDIS_SET,d);
    o->encoding = REDIS_ENCODING_HT;
    return o;
}

/*
 * ����һ�� intset ����
 */
robj *createIntsetObject(void)
{
    intset *is = intsetNew();
    robj *o = createObject(REDIS_SET,is);
    o->encoding = REDIS_ENCODING_INTSET;
    return o;
}

/*
 * ����һ�� hash ����
 */
robj *createHashObject(void)
{
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_HASH, zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}

/*
 * ����һ�� zset ����
 */
robj *createZsetObject(void)
{
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    // zset ʹ�� dict �� skiplist �������ݽṹ
    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();

    o = createObject(REDIS_ZSET,zs);

    o->encoding = REDIS_ENCODING_SKIPLIST;

    return o;
}

/*
 * ����һ�� ziplist ��ʾ�� zset ����
 */
robj *createZsetZiplistObject(void)
{
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_ZSET,zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}

/*
 * �ͷ� string ����
 */
void freeStringObject(robj *o)
{
    if (o->encoding == REDIS_ENCODING_RAW)
    {
        sdsfree(o->ptr);
    }
}

/*
 * �ͷ� list ����
 */
void freeListObject(robj *o)
{
    switch (o->encoding)
    {
        // �ͷ�˫������
    case REDIS_ENCODING_LINKEDLIST:
        listRelease((list*) o->ptr);
        break;
        // �ͷ� ziplist
    case REDIS_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown list encoding type");
    }
}

/*
 * �ͷ� set ����
 */
void freeSetObject(robj *o)
{
    switch (o->encoding)
    {
        // hash ��ʾ
    case REDIS_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
        // intset ��ʾ
    case REDIS_ENCODING_INTSET:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown set encoding type");
    }
}

/*
 *  �ͷ� zset ����
 */
void freeZsetObject(robj *o)
{
    zset *zs;
    switch (o->encoding)
    {
        // skiplist ��ʾ
    case REDIS_ENCODING_SKIPLIST:
        zs = o->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
        break;
        // ziplist ��ʾ
    case REDIS_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown sorted set encoding");
    }
}

/*
 * �ͷ� hash ����
 */
void freeHashObject(robj *o)
{
    switch (o->encoding)
    {
        // hash ��ʾ
    case REDIS_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
        // ziplist ��ʾ
    case REDIS_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown hash encoding type");
        break;
    }
}

/*
 * ���Ӷ�������ü���
 */
void incrRefCount(robj *o)
{
    o->refcount++;
}

/*
 * ���ٶ�������ü���
 *
 * ����������ü�����Ϊ 0 ʱ���ͷ��������
 */
void decrRefCount(void *obj)
{
    robj *o = obj;

    if (o->refcount <= 0) redisPanic("decrRefCount against refcount <= 0");

    if (o->refcount == 1)
    {
        // �����������Ϊ 0
        // ���ݶ������ͣ�������Ӧ�Ķ����ͷź������ͷŶ����ֵ
        switch(o->type)
        {
        case REDIS_STRING:
            freeStringObject(o);
            break;
        case REDIS_LIST:
            freeListObject(o);
            break;
        case REDIS_SET:
            freeSetObject(o);
            break;
        case REDIS_ZSET:
            freeZsetObject(o);
            break;
        case REDIS_HASH:
            freeHashObject(o);
            break;
        default:
            redisPanic("Unknown object type");
            break;
        }
        // �ͷŶ�����
        zfree(o);
    }
    else
    {
        // ����ֻ����������
        o->refcount--;
    }
}

/* This function set the ref count to zero without freeing the object.
 * It is useful in order to pass a new object to functions incrementing
 * the ref count of the received object. Example:
 *
 *    functionThatWillIncrementRefCount(resetRefCount(CreateObject(...)));
 *
 * Otherwise you need to resort to the less elegant pattern:
 *
 *    *obj = createObject(...);
 *    functionThatWillIncrementRefCount(obj);
 *    decrRefCount(obj);
 */
/*
 * �����������������Ϊ 0 �������ͷŸö���
 */
robj *resetRefCount(robj *obj)
{
    obj->refcount = 0;
    return obj;
}

/*
 * ���������� o �������Ƿ�Ϊ�������� type
 *
 * ���Ͳ���ͬʱ���� 1 ������ͻ��˱������ʹ���
 * ������ͬʱ���� 0 ��
 */
int checkType(redisClient *c, robj *o, int type)
{
    if (o->type != type)
    {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

/*
 * �������� string �����ܷ��ʾΪ long long ����ֵ
 */
int isObjectRepresentableAsLongLong(robj *o, long long *llval)
{
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
    if (o->encoding == REDIS_ENCODING_INT)
    {
        if (llval) *llval = (long) o->ptr;
        return REDIS_OK;
    }
    else
    {
        return string2ll(o->ptr,sdslen(o->ptr),llval) ? REDIS_OK : REDIS_ERR;
    }
}

/* Try to encode a string object in order to save space */
/*
 * ���Խ����� o ����������������Խ������뵽�����������
 */
robj *tryObjectEncoding(robj *o)
{
    long value;
    sds s = o->ptr;

    // �����ѱ���?
    if (o->encoding != REDIS_ENCODING_RAW)
        return o; /* Already encoded */

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Redis. Encoded objects can only
     * appear as "values" (and not, for instance, as keys) */
    // ���Թ��������б���
    if (o->refcount > 1) return o;

    /* Currently we try to encode only strings */
    // ֻ���Զ� string ������б���
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);

    /* Check if we can represent this string as a long integer */
    // ���Խ��ַ���ֵת��Ϊ long ����
    // ת��ʧ��ֱ�ӷ��� o ��ת���ɹ�ʱ����ִ��
    if (!string2l(s,sdslen(s),&value)) return o;

    /* Ok, this object can be encoded...
     *
     * Can I use a shared object? Only if the object is inside a given range
     *
     * Note that we also avoid using shared integers when maxmemory is used
     * because every object needs to have a private LRU field for the LRU
     * algorithm to work well. */
    // ִ�е���һ��ʱ�� value ������Ѿ���һ�� long ������
    if (server.maxmemory == 0 && value >= 0 && value < REDIS_SHARED_INTEGERS)
    {
        // ���� value �Ƿ����ڿɹ���ֵ�ķ�Χ
        // ����ǵĻ����ù���������������� o
        decrRefCount(o);
        incrRefCount(shared.integers[value]);

        // ��������󷵻�
        return shared.integers[value];
    }
    else
    {
        // value �����ڹ���Χ���������浽���� o ��
        o->encoding = REDIS_ENCODING_INT;   // ���±��뷽ʽ
        sdsfree(o->ptr);        // �ͷž�ֵ
        o->ptr = (void*) value; // ������ֵ

        return o;
    }
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
/*
 * ����һ�������δ����汾
 *
 * �������������ѱ���ģ���ô���صĶ��������������¶��󸱱�
 * ������������δ����ģ���ôΪ�������ü�����һ��Ȼ�󷵻���
 */
robj *getDecodedObject(robj *o)
{
    robj *dec;

    // ����δ�������
    if (o->encoding == REDIS_ENCODING_RAW)
    {
        incrRefCount(o);
        return o;
    }

    // �����ѱ�������δ����汾
    if (o->type == REDIS_STRING &&
            o->encoding == REDIS_ENCODING_INT)
    {
        char buf[32];

        // ������ֵת����һ���ַ�������
        ll2string(buf,32,(long)o->ptr);
        dec = createStringObject(buf,strlen(buf));
        return dec;
    }
    else
    {
        redisPanic("Unknown encoding type");
    }
}

/* Compare two string objects via strcmp() or alike.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: if objects are not integer encoded, but binary-safe strings,
 * sdscmp() from sds.c will apply memcmp() so this function ca be considered
 * binary safe. */
int compareStringObjects(robj *a, robj *b)
{
    redisAssertWithInfo(NULL,a,a->type == REDIS_STRING && b->type == REDIS_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    int bothsds = 1;

    if (a == b) return 0;
    if (a->encoding != REDIS_ENCODING_RAW)
    {
        ll2string(bufa,sizeof(bufa),(long) a->ptr);
        astr = bufa;
        bothsds = 0;
    }
    else
    {
        astr = a->ptr;
    }
    if (b->encoding != REDIS_ENCODING_RAW)
    {
        ll2string(bufb,sizeof(bufb),(long) b->ptr);
        bstr = bufb;
        bothsds = 0;
    }
    else
    {
        bstr = b->ptr;
    }
    return bothsds ? sdscmp(astr,bstr) : strcmp(astr,bstr);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. */
int equalStringObjects(robj *a, robj *b)
{
    if (a->encoding != REDIS_ENCODING_RAW && b->encoding != REDIS_ENCODING_RAW)
    {
        return a->ptr == b->ptr;
    }
    else
    {
        return compareStringObjects(a,b) == 0;
    }
}

size_t stringObjectLen(robj *o)
{
    redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
    if (o->encoding == REDIS_ENCODING_RAW)
    {
        return sdslen(o->ptr);
    }
    else
    {
        char buf[32];

        return ll2string(buf,32,(long)o->ptr);
    }
}

int getDoubleFromObject(robj *o, double *target)
{
    double value;
    char *eptr;

    if (o == NULL)
    {
        value = 0;
    }
    else
    {
        redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
        if (o->encoding == REDIS_ENCODING_RAW)
        {
            errno = 0;
            value = strtod(o->ptr, &eptr);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                    errno == ERANGE || isnan(value))
                return REDIS_ERR;
        }
        else if (o->encoding == REDIS_ENCODING_INT)
        {
            value = (long)o->ptr;
        }
        else
        {
            redisPanic("Unknown string encoding");
        }
    }
    *target = value;
    return REDIS_OK;
}

int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target, const char *msg)
{
    double value;
    if (getDoubleFromObject(o, &value) != REDIS_OK)
    {
        if (msg != NULL)
        {
            addReplyError(c,(char*)msg);
        }
        else
        {
            addReplyError(c,"value is not a valid float");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}

int getLongDoubleFromObject(robj *o, long double *target)
{
    long double value;
    char *eptr;

    if (o == NULL)
    {
        value = 0;
    }
    else
    {
        redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
        if (o->encoding == REDIS_ENCODING_RAW)
        {
            errno = 0;
            value = strtold(o->ptr, &eptr);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                    errno == ERANGE || isnan(value))
                return REDIS_ERR;
        }
        else if (o->encoding == REDIS_ENCODING_INT)
        {
            value = (long)o->ptr;
        }
        else
        {
            redisPanic("Unknown string encoding");
        }
    }
    *target = value;
    return REDIS_OK;
}

int getLongDoubleFromObjectOrReply(redisClient *c, robj *o, long double *target, const char *msg)
{
    long double value;
    if (getLongDoubleFromObject(o, &value) != REDIS_OK)
    {
        if (msg != NULL)
        {
            addReplyError(c,(char*)msg);
        }
        else
        {
            addReplyError(c,"value is not a valid float");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}

/*
 * �Ӷ��� o ����ȡ long long ֵ����ʹ��ָ�뱣������ȡ��ֵ
 */
int getLongLongFromObject(
    robj *o,            // ��ȡ����
    long long *target   // ����ֵ��ָ��
)
{
    long long value;
    char *eptr;

    if (o == NULL)
    {
        value = 0;
    }
    else
    {
        redisAssertWithInfo(NULL,o,o->type == REDIS_STRING);
        // ���ݲ�ͬ���룬ȡ��ֵ
        if (o->encoding == REDIS_ENCODING_RAW)
        {
            errno = 0;
            value = strtoll(o->ptr, &eptr, 10);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                    errno == ERANGE)
                return REDIS_ERR;
        }
        else if (o->encoding == REDIS_ENCODING_INT)
        {
            value = (long)o->ptr;
        }
        else
        {
            redisPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return REDIS_OK;
}

/*
 * ���������� o �� long long ֵȡ������ͨ��ָ�� target ���б���
 * �����ȡʧ�ܣ���Ӵ�����Ϣ msg ���ͻ��� c
 */
int getLongLongFromObjectOrReply(
    redisClient *c,
    robj *o,            // Ҫ��ȡ long long ֵ�Ķ���
    long long *target,  // ����ֵ��ָ��
    const char *msg     // ������Ϣ
)
{
    long long value;
    if (getLongLongFromObject(o, &value) != REDIS_OK)
    {
        if (msg != NULL)
        {
            addReplyError(c,(char*)msg);
        }
        else
        {
            addReplyError(c,"value is not an integer or out of range");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}

int getLongFromObjectOrReply(redisClient *c, robj *o, long *target, const char *msg)
{
    long long value;

    if (getLongLongFromObjectOrReply(c, o, &value, msg) != REDIS_OK) return REDIS_ERR;
    if (value < LONG_MIN || value > LONG_MAX)
    {
        if (msg != NULL)
        {
            addReplyError(c,(char*)msg);
        }
        else
        {
            addReplyError(c,"value is out of range");
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}

/*
 * ���ر�����ַ�����ʽ
 */
char *strEncoding(int encoding)
{
    switch(encoding)
    {
    case REDIS_ENCODING_RAW:
        return "raw";
    case REDIS_ENCODING_INT:
        return "int";
    case REDIS_ENCODING_HT:
        return "hashtable";
    case REDIS_ENCODING_LINKEDLIST:
        return "linkedlist";
    case REDIS_ENCODING_ZIPLIST:
        return "ziplist";
    case REDIS_ENCODING_INTSET:
        return "intset";
    case REDIS_ENCODING_SKIPLIST:
        return "skiplist";
    default:
        return "unknown";
    }
}

/* Given an object returns the min number of seconds the object was never
 * requested, using an approximated LRU algorithm. */
unsigned long estimateObjectIdleTime(robj *o)
{
    if (server.lruclock >= o->lru)
    {
        return (server.lruclock - o->lru) * REDIS_LRU_CLOCK_RESOLUTION;
    }
    else
    {
        return ((REDIS_LRU_CLOCK_MAX - o->lru) + server.lruclock) *
               REDIS_LRU_CLOCK_RESOLUTION;
    }
}

/* This is an helper function for the DEBUG command. We need to lookup keys
 * without any modification of LRU or other parameters. */
robj *objectCommandLookup(redisClient *c, robj *key)
{
    dictEntry *de;

    if ((de = dictFind(c->db->dict,key->ptr)) == NULL) return NULL;
    return (robj*) dictGetVal(de);
}

robj *objectCommandLookupOrReply(redisClient *c, robj *key, robj *reply)
{
    robj *o = objectCommandLookup(c,key);

    if (!o) addReply(c, reply);
    return o;
}

/*
 * OBJECT �����ʵ��
 */
void objectCommand(redisClient *c)
{
    robj *o;

    // �鿴���ü���
    if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3)
    {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,o->refcount);
        // �鿴���뷽ʽ
    }
    else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3)
    {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyBulkCString(c,strEncoding(o->encoding));
        // �鿴��תʱ��
    }
    else if (!strcasecmp(c->argv[1]->ptr,"idletime") && c->argc == 3)
    {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,estimateObjectIdleTime(o));
    }
    else
    {
        addReplyError(c,"Syntax error. Try OBJECT (refcount|encoding|idletime)");
    }
}

