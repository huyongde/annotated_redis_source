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

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op);

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a regular
 * hash table. */
/*
 * ���ݸ���ֵ value �������Ǵ��� intset ���뻹���ֵ����ļ���
 *
 * T = O(1)
 */
robj *setTypeCreate(robj *value)
{
    if (isObjectRepresentableAsLongLong(value,NULL) == REDIS_OK)
        return createIntsetObject();
    return createSetObject();
}

/*
 * ��̬��Ӳ���
 *
 * T = O(N)
 */
int setTypeAdd(robj *subject, robj *value)
{
    long long llval;
    // subject Ϊ�ֵ�,O(1)
    if (subject->encoding == REDIS_ENCODING_HT)
    {
        if (dictAdd(subject->ptr,value,NULL) == DICT_OK)
        {
            incrRefCount(value);
            return 1;
        }
        // subject Ϊintset , O(N)
    }
    else if (subject->encoding == REDIS_ENCODING_INTSET)
    {
        // value ���Ա�ʾΪ long long ����
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK)
        {
            uint8_t success = 0;
            // ���ֵ, O(N)
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
            // ��ӳɹ�
            if (success)
            {
                /* Convert to regular set when the intset contains
                 * too many entries. */
                // ����Ƿ���Ҫ�� intset ת��Ϊ�ֵ�
                if (intsetLen(subject->ptr) > server.set_max_intset_entries)
                    setTypeConvert(subject,REDIS_ENCODING_HT);
                return 1;
            }
            // value ���ܱ���Ϊ long long ���ͣ�����ת��Ϊ�ֵ�
        }
        else
        {
            /* Failed to get integer from object, convert to regular set. */
            setTypeConvert(subject,REDIS_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. */
            // ���ֵ
            redisAssertWithInfo(NULL,value,dictAdd(subject->ptr,value,NULL) == DICT_OK);

            incrRefCount(value);
            return 1;
        }
    }
    else
    {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

/*
 * ��̬ɾ������
 *
 * T = O(N)
 */
int setTypeRemove(robj *setobj, robj *value)
{
    long long llval;
    // �ֵ����, O(N)
    if (setobj->encoding == REDIS_ENCODING_HT)
    {
        // O(1)
        if (dictDelete(setobj->ptr,value) == DICT_OK)
        {
            // �������Ҫ����С�ֵ�, O(N)
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }
        // intset ����
    }
    else if (setobj->encoding == REDIS_ENCODING_INTSET)
    {
        // ��� value ���Ա���� long long ���ͣ�
        // ��ô������ intset ɾ����
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK)
        {
            int success;
            // O(N)
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    }
    else
    {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

/*
 * ��̬��Ա������
 *
 * T = O(lg N)
 */
int setTypeIsMember(robj *subject, robj *value)
{
    long long llval;

    // �ֵ�
    if (subject->encoding == REDIS_ENCODING_HT)
    {
        // O(1)
        return dictFind((dict*)subject->ptr,value) != NULL;

        // intset
    }
    else if (subject->encoding == REDIS_ENCODING_INTSET)
    {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK)
        {
            // O(lg N)
            return intsetFind((intset*)subject->ptr,llval);
        }

    }
    else
    {
        redisPanic("Unknown set encoding");
    }

    return 0;
}

/*
 * ����һ����̬������
 *
 * T = O(1)
 */
setTypeIterator *setTypeInitIterator(robj *subject)
{
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));

    si->subject = subject;
    si->encoding = subject->encoding;

    if (si->encoding == REDIS_ENCODING_HT)
    {
        si->di = dictGetIterator(subject->ptr);
    }
    else if (si->encoding == REDIS_ENCODING_INTSET)
    {
        si->ii = 0;
    }
    else
    {
        redisPanic("Unknown set encoding");
    }

    return si;
}

/*
 * �ͷŶ�̬������
 *
 * T = O(1)
 */
void setTypeReleaseIterator(setTypeIterator *si)
{
    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position.
 *
 * Since set elements can be internally be stored as redis objects or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointer
 * (eobj) or (llobj) accordingly.
 *
 * When there are no longer elements -1 is returned.
 * Returned objects ref count is not incremented, so this function is
 * copy on write friendly. */
/*
 * ȡ��������ָ��ĵ�ǰԪ��
 *
 * robj ���������ֵ�����ֵ�� llele �������� intset �����ֵ
 *
 * ����ֵָʾ�������ֱ����ֵ��ȡ���ˣ����� -1 ��ʾ����Ϊ��
 *
 * T = O(1)
 */
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele)
{

    // �ֵ�
    if (si->encoding == REDIS_ENCODING_HT)
    {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *objele = dictGetKey(de);
        // intset
    }
    else if (si->encoding == REDIS_ENCODING_INTSET)
    {
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
    }

    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new objects
 * or incrementing the ref count of returned objects. So if you don't
 * retain a pointer to this object you should call decrRefCount() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue as the result will be anyway of incrementing the ref count. */
/*
 * ��ȡ�������ĵ�ǰԪ�أ������ذ�������һ������
 * ��������߲�ʹ���������Ļ���������ʹ�����֮���ͷ�����
 *
 * T = O(1)
 */
robj *setTypeNextObject(setTypeIterator *si)
{
    int64_t intele;
    robj *objele;
    int encoding;

    encoding = setTypeNext(si,&objele,&intele);
    switch(encoding)
    {
    case -1:
        return NULL;
    case REDIS_ENCODING_INTSET:
        return createStringObjectFromLongLong(intele);
    case REDIS_ENCODING_HT:
        incrRefCount(objele);
        return objele;
    default:
        redisPanic("Unsupported encoding");
    }

    return NULL; /* just to suppress warnings */
}

/* Return random element from a non empty set.
 * The returned element can be a int64_t value if the set is encoded
 * as an "intset" blob of integers, or a redis object if the set
 * is a regular set.
 *
 * The caller provides both pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and is used by the caller to check if the
 * int64_t pointer or the redis object pointere was populated.
 *
 * When an object is returned (the set was a real set) the ref count
 * of the object is not incremented so this function can be considered
 * copy on write friendly. */
/*
 * ��̬���Ԫ�ط��غ���
 *
 * objele �����ֵ�����ֵ�� llele ���� intset �����ֵ
 *
 * ����ֵָʾ�������ֱ����ֵ�������ˡ�
 *
 * T = O(N)
 */
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele)
{

    // �ֵ�
    if (setobj->encoding == REDIS_ENCODING_HT)
    {
        // O(N)
        dictEntry *de = dictGetRandomKey(setobj->ptr);
        // O(1)
        *objele = dictGetKey(de);

        // intset
    }
    else if (setobj->encoding == REDIS_ENCODING_INTSET)
    {
        // O(1)
        *llele = intsetRandom(setobj->ptr);

    }
    else
    {
        redisPanic("Unknown set encoding");
    }

    return setobj->encoding;
}

/*
 * ��̬Ԫ�ظ������غ���
 *
 * T = O(1)
 */
unsigned long setTypeSize(robj *subject)
{
    // �ֵ�
    if (subject->encoding == REDIS_ENCODING_HT)
    {
        return dictSize((dict*)subject->ptr);

        // intset
    }
    else if (subject->encoding == REDIS_ENCODING_INTSET)
    {
        return intsetLen((intset*)subject->ptr);

    }
    else
    {
        redisPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
/*
 * �����϶��� setobj ת��Ϊ enc ָ���ı���
 *
 * Ŀǰֻ֧�ֽ� intset ת��Ϊ HT ����
 *
 * T = O(N)
 */
void setTypeConvert(robj *setobj, int enc)
{
    setTypeIterator *si;
    redisAssertWithInfo(NULL,setobj,setobj->type == REDIS_SET &&
                        setobj->encoding == REDIS_ENCODING_INTSET);

    if (enc == REDIS_ENCODING_HT)
    {
        int64_t intele;
        dict *d = dictCreate(&setDictType,NULL);
        robj *element;

        /* Presize the dict to avoid rehashing */
        // O(N)
        dictExpand(d,intsetLen(setobj->ptr));

        /* To add the elements we extract integers and create redis objects */
        // O(N)
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si,NULL,&intele) != -1)
        {
            element = createStringObjectFromLongLong(intele);
            redisAssertWithInfo(NULL,element,dictAdd(d,element,NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        setobj->encoding = REDIS_ENCODING_HT;
        zfree(setobj->ptr);
        setobj->ptr = d;
    }
    else
    {
        redisPanic("Unsupported set conversion");
    }
}

/*
 * T = O(N^2)
 */
void saddCommand(redisClient *c)
{
    robj *set;
    int j, added = 0;

    // ���Ҽ��϶���
    set = lookupKeyWrite(c->db,c->argv[1]);

    // û�ҵ�������һ���ռ���, O(1)
    if (set == NULL)
    {
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db,c->argv[1],set);
        // �ҵ��������Ǽ��ϣ�����
    }
    else
    {
        if (set->type != REDIS_SET)
        {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }

    // ����������Ԫ����ӵ�����
    // O(N^2)
    for (j = 2; j < c->argc; j++)
    {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (setTypeAdd(set,c->argv[j])) added++;
    }

    // ���������һ��Ԫ����ӳɹ�����ô֪ͨ db ��key ���޸�
    if (added) signalModifiedKey(c->db,c->argv[1]);

    // �������Ԫ�صĸ���
    server.dirty += added;
    addReplyLongLong(c,added);
}

/*
 * T = O(N^2)
 */
void sremCommand(redisClient *c)
{
    robj *set;
    int j, deleted = 0;

    // ���Ҽ��ϣ�������ϲ����ڻ����ʹ���ֱ�ӷ���
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,set,REDIS_SET)) return;

    // �Ƴ����Ԫ��, O(N^2)
    for (j = 2; j < c->argc; j++)
    {
        if (setTypeRemove(set,c->argv[j]))
        {
            deleted++;
            if (setTypeSize(set) == 0)
            {
                dbDelete(c->db,c->argv[1]);
                break;
            }
        }
    }

    // ���������һ��Ԫ�ر��Ƴ�����ô֪ͨ db ��key ���޸�
    if (deleted)
    {
        signalModifiedKey(c->db,c->argv[1]);
        server.dirty += deleted;
    }

    addReplyLongLong(c,deleted);
}

/*
 * T = O(N)
 */
void smoveCommand(redisClient *c)
{
    robj *srcset, *dstset, *ele;

    // Դ����
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    // Ŀ�꼯��
    dstset = lookupKeyWrite(c->db,c->argv[2]);

    // Ҫ���ƶ���Ԫ��
    ele = c->argv[3] = tryObjectEncoding(c->argv[3]);

    /* If the source key does not exist return 0 */
    // Դ���ϲ����ڣ�ֱ�ӷ���
    if (srcset == NULL)
    {
        addReply(c,shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    // ���Դ�������Ŀ������Ǽ��ϣ�ֱ�ӷ���
    if (checkType(c,srcset,REDIS_SET) ||
            (dstset && checkType(c,dstset,REDIS_SET))) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    // Դ���Ϻ�Ŀ�꼯����ͬ��ֱ�ӷ���
    if (srcset == dstset)
    {
        addReply(c,shared.cone);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    // ��Դ������ɾ��Ŀ��Ԫ��
    // O(N)
    if (!setTypeRemove(srcset,ele))
    {
        addReply(c,shared.czero);
        return;
    }

    /* Remove the src set from the database when empty */
    // ���Դ�����Ѿ�Ϊ�գ���ôɾ����
    if (setTypeSize(srcset) == 0) dbDelete(c->db,c->argv[1]);

    // ֪ͨ db �� key ���޸�
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    server.dirty++;

    /* Create the destination set when it doesn't exist */
    // ���Ŀ�꼯�ϲ����ڣ�����һ���µ�
    // O(1)
    if (!dstset)
    {
        dstset = setTypeCreate(ele);
        dbAdd(c->db,c->argv[2],dstset);
    }

    /* An extra key has changed when ele was successfully added to dstset */
    // ��Ŀ��Ԫ����ӵ�����
    // O(N)
    if (setTypeAdd(dstset,ele)) server.dirty++;

    addReply(c,shared.cone);
}

/*
 * T = O(lg N)
 */
void sismemberCommand(redisClient *c)
{
    robj *set;

    // ���Ҷ��󣬼������
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,set,REDIS_SET)) return;

    // ������Ԫ�ؽ��б���
    c->argv[2] = tryObjectEncoding(c->argv[2]);

    // ���ؽ��, O(lg N)
    if (setTypeIsMember(set,c->argv[2]))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

/*
 * T = O(1)
 */
void scardCommand(redisClient *c)
{
    robj *o;

    // ���Ҷ��󣬼������
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,o,REDIS_SET)) return;

    // ���� size ,O(1)
    addReplyLongLong(c,setTypeSize(o));
}

/*
 * T = O(N)
 */
void spopCommand(redisClient *c)
{
    robj *set, *ele, *aux;
    int64_t llele;
    int encoding;

    // ���Ҷ��󣬼������
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
            checkType(c,set,REDIS_SET)) return;

    // �ڶ�̬�����л�ȡһ���������һ�� long long ֵ
    // O(N)
    encoding = setTypeRandomElement(set,&ele,&llele);

    // ���ݷ���ֵ�ж��Ǹ�ָ�뱣����Ԫ��
    if (encoding == REDIS_ENCODING_INTSET)
    {
        // ����Ԫ����Ϊ����ֵ
        ele = createStringObjectFromLongLong(llele);
        // ɾ�� intset �е�Ԫ��, O(N)
        set->ptr = intsetRemove(set->ptr,llele,NULL);
    }
    else
    {
        // ΪԪ�صļ�����һ���Է�����
        incrRefCount(ele);
        // ɾ���ֵ���ԭ�е�Ԫ��
        // O(1)
        setTypeRemove(set,ele);
    }

    /* Replicate/AOF this command as an SREM operation */
    aux = createStringObject("SREM",4);
    rewriteClientCommandVector(c,3,aux,c->argv[1],ele);
    decrRefCount(ele);
    decrRefCount(aux);

    addReplyBulk(c,ele);
    if (setTypeSize(set) == 0) dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

/*
 * �� count ������ SRANDMEMBER �����ʵ��
 *
 * T = O(N^2)
 */
void srandmemberWithCountCommand(redisClient *c)
{
    long l;
    unsigned long count, size;
    int uniq = 1;
    robj *set, *ele;
    int64_t llele;
    int encoding;

    dict *d;

    // ��ȡ count ����
    if (getLongFromObjectOrReply(c,c->argv[2],&l,NULL) != REDIS_OK) return;

    if (l >= 0)
    {
        count = (unsigned) l;
    }
    else
    {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    // ���Ҷ��󣬼������, O(1)
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk))
            == NULL || checkType(c,set,REDIS_SET)) return;
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    // ��� count Ϊ 0 ��ֱ�ӷ���
    if (count == 0)
    {
        addReply(c,shared.emptymultibulk);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. */
    // ���� count ��Ԫ�أ�Ԫ�ؿ������ظ�
    if (!uniq)
    {
        addReplyMultiBulkLen(c,count);
        // O(N^2)
        while(count--)
        {
            // O(N)
            encoding = setTypeRandomElement(set,&ele,&llele);
            if (encoding == REDIS_ENCODING_INTSET)
            {
                addReplyBulkLongLong(c,llele);
            }
            else
            {
                addReplyBulk(c,ele);
            }
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    // count ���ڼ��ϻ�����������������
    if (count >= size)
    {
        // O(N^2)
        sunionDiffGenericCommand(c,c->argv,c->argc-1,NULL,REDIS_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    // CASE 3 �� CASE 4 ʹ���ֵ䱣����, O(1)
    d = dictCreate(&setDictType,NULL);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requsted elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient. */
    // ��� count * SRANDMEMBER_SUB_STRATEGY_MUL �ȼ��ϵĴ�С(size)����
    // ��ô�������ϣ������ϵ�Ԫ�ر��浽��һ���¼�����
    // Ȼ����¼��������ɾ��Ԫ�أ�ֱ��ʣ�� count ��Ԫ��Ϊֹ�����ɾ���¼���
    // �����ֱ�Ӽ��� count �����Ԫ��Ч�ʸ���
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size)
    {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        // ������������
        // O(N)
        si = setTypeInitIterator(set);
        while((encoding = setTypeNext(si,&ele,&llele)) != -1)
        {
            int retval;

            if (encoding == REDIS_ENCODING_INTSET)
            {
                retval = dictAdd(d,createStringObjectFromLongLong(llele),NULL);
            }
            else if (ele->encoding == REDIS_ENCODING_RAW)
            {
                retval = dictAdd(d,dupStringObject(ele),NULL);
            }
            else if (ele->encoding == REDIS_ENCODING_INT)
            {
                retval = dictAdd(d,
                                 createStringObjectFromLongLong((long)ele->ptr),NULL);
            }
            redisAssert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        redisAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        // ���ɾ��Ԫ��ֱ�� size == count Ϊֹ
        // O(N^2)
        while(size > count)
        {
            dictEntry *de;

            // O(N)
            de = dictGetRandomKey(d);
            dictDelete(d,dictGetKey(de));
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    // ���ϵĻ����� count ҪС����������£�������� count ��Ԫ�ؼ���
    else
    {
        unsigned long added = 0;

        // O(N^2)
        while(added < count)
        {

            // O(N)
            encoding = setTypeRandomElement(set,&ele,&llele);

            if (encoding == REDIS_ENCODING_INTSET)
            {
                ele = createStringObjectFromLongLong(llele);
            }
            else if (ele->encoding == REDIS_ENCODING_RAW)
            {
                ele = dupStringObject(ele);
            }
            else if (ele->encoding == REDIS_ENCODING_INT)
            {
                ele = createStringObjectFromLongLong((long)ele->ptr);
            }

            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            // O(1)
            if (dictAdd(d,ele,NULL) == DICT_OK)
                added++;
            else
                decrRefCount(ele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    // O(N)
    {
        dictIterator *di;
        dictEntry *de;

        addReplyMultiBulkLen(c,count);
        di = dictGetIterator(d);
        while((de = dictNext(di)) != NULL)
            addReplyBulk(c,dictGetKey(de));
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

/*
 * ���ؼ����еĵ������Ԫ��
 *
 * T = O(N)
 */
void srandmemberCommand(redisClient *c)
{
    robj *set, *ele;
    int64_t llele;
    int encoding;

    if (c->argc == 3)
    {
        srandmemberWithCountCommand(c);
        return;
    }
    else if (c->argc > 3)
    {
        addReply(c,shared.syntaxerr);
        return;
    }

    // ��ȡ/�������󣬲�������Ƿ񼯺�
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
            checkType(c,set,REDIS_SET)) return;

    // ��ȡ���Ԫ��
    // O(N)
    encoding = setTypeRandomElement(set,&ele,&llele);
    if (encoding == REDIS_ENCODING_INTSET)
    {
        addReplyBulkLongLong(c,llele);
    }
    else
    {
        addReplyBulk(c,ele);
    }
}

/*
 * �Ա��������ϵĻ���
 *
 * T = O(1)
 */
int qsortCompareSetsByCardinality(const void *s1, const void *s2)
{
    return setTypeSize(*(robj**)s1)-setTypeSize(*(robj**)s2);
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
/*
 * �Ա��������ϵĻ����Ƿ���ͬ
 *
 * T = O(1)
 */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2)
{
    robj *o1 = *(robj**)s1, *o2 = *(robj**)s2;

    return  (o2 ? setTypeSize(o2) : 0) - (o1 ? setTypeSize(o1) : 0);
}

/*
 * T = O(N^2 lg N)
 */
void sinterGenericCommand(redisClient *c, robj **setkeys, unsigned long setnum, robj *dstkey)
{
    // ����ָ�����飬�������� setnum
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    // ���ϵ�����
    setTypeIterator *si;
    robj *eleobj,
         *dstset = NULL;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding;

    // �����м��϶���ָ�뱣�浽 sets ����
    // O(N)
    for (j = 0; j < setnum; j++)
    {
        robj *setobj = dstkey ?
                       lookupKeyWrite(c->db,setkeys[j]) :
                       lookupKeyRead(c->db,setkeys[j]);
        if (!setobj)
        {
            zfree(sets);
            if (dstkey)
            {
                if (dbDelete(c->db,dstkey))
                {
                    signalModifiedKey(c->db,dstkey);
                    server.dirty++;
                }
                addReply(c,shared.czero);
            }
            else
            {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
        if (checkType(c,setobj,REDIS_SET))
        {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performace */
    // �����ϵĻ�����С�������򼯺�
    // O(N^2)
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey)
    {
        replylen = addDeferredMultiBulkLength(c);
    }
    else
    {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    // ȡ�� sets[0] ��Ԫ�أ�������Ԫ�ؽ��н���������
    // ֻҪĳ��������һ�������� sets[0] ��Ԫ�أ�
    // ��ô���Ԫ�ؾͲ��������ڽ������������

    // ����������
    si = setTypeInitIterator(sets[0]);
    // ���� sets[0] , O(N^2 lg N)
    while((encoding = setTypeNext(si,&eleobj,&intobj)) != -1)
    {
        // ��������������������
        // O(N lg N)
        for (j = 1; j < setnum; j++)
        {
            // ������ͬ�ļ���
            if (sets[j] == sets[0]) continue;

            // sets[0] �� intset ʱ������
            if (encoding == REDIS_ENCODING_INTSET)
            {
                /* intset with intset is simple... and fast */
                // O(lg N)
                if (sets[j]->encoding == REDIS_ENCODING_INTSET &&
                        !intsetFind((intset*)sets[j]->ptr,intobj))
                {
                    break;
                    /* in order to compare an integer with an object we
                     * have to use the generic function, creating an object
                     * for this */
                }
                else if (sets[j]->encoding == REDIS_ENCODING_HT)
                {
                    eleobj = createStringObjectFromLongLong(intobj);
                    // O(1)
                    if (!setTypeIsMember(sets[j],eleobj))
                    {
                        decrRefCount(eleobj);
                        break;
                    }
                    decrRefCount(eleobj);
                }
                // sets[0] ���ֵ�ʱ������
            }
            else if (encoding == REDIS_ENCODING_HT)
            {
                /* Optimization... if the source object is integer
                 * encoded AND the target set is an intset, we can get
                 * a much faster path. */
                if (eleobj->encoding == REDIS_ENCODING_INT &&
                        sets[j]->encoding == REDIS_ENCODING_INTSET &&
                        !intsetFind((intset*)sets[j]->ptr,(long)eleobj->ptr))
                {
                    break;
                    /* else... object to object check is easy as we use the
                     * type agnostic API here. */
                }
                else if (!setTypeIsMember(sets[j],eleobj))
                {
                    break;
                }
            }
        }

        /* Only take action when all sets contain the member */
        // ֻ�����м��϶����� eleobj/intobj ʱ���������
        if (j == setnum)
        {
            // û�� dstkey ��ֱ�ӷ��ظ����
            if (!dstkey)
            {
                if (encoding == REDIS_ENCODING_HT)
                    addReplyBulk(c,eleobj);
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
                // �� dstkey ����ӵ� dstkey
            }
            else
            {
                if (encoding == REDIS_ENCODING_INTSET)
                {
                    eleobj = createStringObjectFromLongLong(intobj);
                    setTypeAdd(dstset,eleobj);
                    decrRefCount(eleobj);
                }
                else
                {
                    setTypeAdd(dstset,eleobj);
                }
            }
        }
    }
    // �ͷŵ�����
    setTypeReleaseIterator(si);

    if (dstkey)
    {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        // �ý�����ϴ���ԭ�е� dstkey
        dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0)
        {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
        }
        else
        {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        signalModifiedKey(c->db,dstkey);
        server.dirty++;
    }
    else
    {
        setDeferredMultiBulkLength(c,replylen,cardinality);
    }

    zfree(sets);
}

void sinterCommand(redisClient *c)
{
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

void sinterstoreCommand(redisClient *c)
{
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}

#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2

/*
 * T = O(N^2)
 */
void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op)
{
    // ����ָ������
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    // ���ϵ�����
    setTypeIterator *si;
    robj *ele, *dstset = NULL;
    int j, cardinality = 0;
    int diff_algo = 1;

    // �ռ����м��϶���ָ��
    // O(N)
    for (j = 0; j < setnum; j++)
    {
        robj *setobj = dstkey ?
                       lookupKeyWrite(c->db,setkeys[j]) :
                       lookupKeyRead(c->db,setkeys[j]);
        if (!setobj)
        {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,REDIS_SET))
        {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. */
    // ���ݼ��ϵĴ�Сѡ��ǡ�����㷨
    if (op == REDIS_OP_DIFF && sets[0])
    {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++)
        {
            if (sets[j] == NULL) continue;

            // ���� N * M
            algo_one_work += setTypeSize(sets[0]);
            // �������м��ϵ��ܴ�С
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;

        // ѡ���㷨
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1)
        {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets+1,setnum-1,sizeof(robj*),
                  qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    // ���ڱ��� union ����Ķ���
    // ��� dstkey ��Ϊ�գ��������ֵ�ͻᱻ����Ϊ dstkey
    dstset = createIntsetObject();

    // union ����
    if (op == REDIS_OP_UNION)
    {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        // �������м��ϵ�����Ԫ�أ���������ӵ� dstset ��ȥ
        for (j = 0; j < setnum; j++)
        {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while((ele = setTypeNextObject(si)) != NULL)
            {
                // ���е�Ԫ�ز��ᱻ����
                if (setTypeAdd(dstset,ele)) cardinality++;
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si);
        }
    }
    else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 1)
    {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. */
        // ���� sets[0] ���������е�ÿ��Ԫ�� elem ��
        // ֻ�� elem �������������κμ���ʱ���Ž�����ӵ� dstset
        si = setTypeInitIterator(sets[0]);
        while((ele = setTypeNextObject(si)) != NULL)
        {
            for (j = 1; j < setnum; j++)
            {
                if (!sets[j]) continue; /* no key is an empty set. */
                if (setTypeIsMember(sets[j],ele)) break;
            }
            if (j == setnum)
            {
                /* There is no other set with this element. Add it. */
                // û��һ�������� elem Ԫ�أ����������
                setTypeAdd(dstset,ele);
                cardinality++;
            }
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);
    }
    else if (op == REDIS_OP_DIFF && sets[0] && diff_algo == 2)
    {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. */
        // �� sets[0] ������Ԫ�ر��浽 dstset ������
        // ������������м��ϣ�������������Ϻ� dstset ����ͬ��Ԫ�أ�
        // ��ô�� dstkey ��ɾ���Ǹ�Ԫ��
        for (j = 0; j < setnum; j++)
        {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while((ele = setTypeNextObject(si)) != NULL)
            {
                if (j == 0)
                {
                    // �������Ԫ�ص� sets[0]
                    if (setTypeAdd(dstset,ele)) cardinality++;
                }
                else
                {
                    // �������������������ͬ��Ԫ�أ���ôɾ����
                    if (setTypeRemove(dstset,ele)) cardinality--;
                }
                decrRefCount(ele);
            }
            setTypeReleaseIterator(si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    // û�� dstkey ��ֱ��������
    if (!dstkey)
    {
        addReplyMultiBulkLen(c,cardinality);
        si = setTypeInitIterator(dstset);
        while((ele = setTypeNextObject(si)) != NULL)
        {
            addReplyBulk(c,ele);
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);
        decrRefCount(dstset);
        // �� dstkey ���� dstset �滻ԭ�� dstkey �Ķ���
    }
    else
    {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0)
        {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
        }
        else
        {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        signalModifiedKey(c->db,dstkey);
        server.dirty++;
    }
    zfree(sets);
}

void sunionCommand(redisClient *c)
{
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_UNION);
}

void sunionstoreCommand(redisClient *c)
{
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_UNION);
}

void sdiffCommand(redisClient *c)
{
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_DIFF);
}

void sdiffstoreCommand(redisClient *c)
{
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_DIFF);
}
