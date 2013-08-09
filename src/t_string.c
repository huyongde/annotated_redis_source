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
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

/*
 * ��鳤�� size �Ƿ񳬹� Redis ���������
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��
 *  REDIS_ERR   ����
 *  REDIS_OK    δ����
 */
static int checkStringLength(redisClient *c, long long size)
{
    if (size > 512*1024*1024)
    {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/*
 * ͨ�� set ������� SET / SETEX �� SETNX ������ĵײ�ʵ��
 *
 * ������
 *  c   �ͻ���
 *  nx  �����Ϊ 0 ����ô��ʾֻ���� key ������ʱ�Ž��� set ����
 *  key
 *  val
 *  expire  ����ʱ��
 *  unit    ����ʱ��ĵ�λ����Ϊ UNIT_SECONDS �� UNIT_MILLISECONDS
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void setGenericCommand(redisClient *c, int nx, robj *key, robj *val, robj *expire, int unit)
{
    long long milliseconds = 0; /* initialized to avoid an harmness warning */

    // ������� expire ��������ô������ sds תΪ long long ����
    if (expire)
    {
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != REDIS_OK)
            return;
        if (milliseconds <= 0)
        {
            addReplyError(c,"invalid expire time in SETEX");
            return;
        }

        // ��������ʱ�����뻹�Ǻ���
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }

    // ��������� nx ���������� key �Ѿ����ڣ���ôֱ����ͻ��˷���
    if (nx && lookupKeyWrite(c->db,key) != NULL)
    {
        addReply(c,shared.czero);
        return;
    }

    // ���� key-value ��
    setKey(c->db,key,val);

    server.dirty++;

    // Ϊ key ���ù���ʱ��
    if (expire) setExpire(c->db,key,mstime()+milliseconds);

    // ��ͻ��˷��ػظ�
    addReply(c, nx ? shared.cone : shared.ok);
}

/*
 * SET �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void setCommand(redisClient *c)
{
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,0,c->argv[1],c->argv[2],NULL,0);
}

/*
 * SETNX �����ʵ��
 *
 * ���Ӷȣ�
 *  O(1)
 *
 * ����ֵ��void
 */
void setnxCommand(redisClient *c)
{
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,1,c->argv[1],c->argv[2],NULL,0);
}

/*
 * SETEX �����ʵ��
 *
 * ���Ӷȣ�
 *  O(1)
 *
 * ����ֵ��void
 */
void setexCommand(redisClient *c)
{
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,0,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS);
}

/*
 * PSETEX �����ʵ��
 *
 * ���Ӷȣ�
 *  O(1)
 *
 * ����ֵ��void
 */
void psetexCommand(redisClient *c)
{
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,0,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS);
}

/*
 * ���ݿͻ���ָ���� key ��������Ӧ��ֵ��
 * ���� get ����ĵײ�ʵ�֡�
 *
 * ���Ӷȣ�
 *  O(1)
 *
 * ����ֵ��
 *  REDIS_OK    ������ɣ������ҵ���Ҳ����û�ҵ���
 *  REDIS_ERR   �ҵ����� key �����ַ�������
 */
int getGenericCommand(redisClient *c)
{
    robj *o;

    // ����
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return REDIS_OK;

    // ����
    if (o->type != REDIS_STRING)
    {
        addReply(c,shared.wrongtypeerr);
        return REDIS_ERR;
    }
    else
    {
        addReplyBulk(c,o);
        return REDIS_OK;
    }
}

/*
 * GET �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void getCommand(redisClient *c)
{
    getGenericCommand(c);
}

/*
 * GETSET �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void getsetCommand(redisClient *c)
{

    // ��ȡ����ֵ������ӵ��ͻ��˻ظ� buffer ��
    if (getGenericCommand(c) == REDIS_ERR) return;

    // ������ֵ
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setKey(c->db,c->argv[1],c->argv[2]);

    server.dirty++;
}

/*
 * SETRANGE �����ʵ��
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void setrangeCommand(redisClient *c)
{
    robj *o;
    long offset;

    // �����滻�����ݵ��ַ���
    sds value = c->argv[3]->ptr;

    // �� offset ת��Ϊ long ����ֵ
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != REDIS_OK)
        return;

    // ��� offset �Ƿ�λ�ںϷ���Χ
    if (offset < 0)
    {
        addReplyError(c,"offset is out of range");
        return;
    }

    // ���Ҹ��� key
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL)
    {
        // key ������ ...

        // �� value Ϊ���ַ������� key ������ʱ������ 0
        if (sdslen(value) == 0)
        {
            addReply(c,shared.czero);
            return;
        }

        // �� value �ĳ��ȹ���ʱ��ֱ�ӷ���
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;

        // �� key ����Ϊ���ַ�������
        o = createObject(REDIS_STRING,sdsempty());
        dbAdd(c->db,c->argv[1],o);
    }
    else
    {
        // key ���� ...

        size_t olen;

        // ��� key �Ƿ��ַ���
        if (checkType(c,o,REDIS_STRING))
            return;

        // ��� value Ϊ���ַ�������ôֱ�ӷ���ԭ���ַ����ĳ���
        olen = stringObjectLen(o);
        if (sdslen(value) == 0)
        {
            addReplyLongLong(c,olen);
            return;
        }

        // ����޸ĺ���ַ������Ȼ�񳬹��������
        if (checkStringLength(c,offset+sdslen(value)) != REDIS_OK)
            return;

        // �� o �ǹ��������߱������ʱ����������һ������
        if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW)
        {
            robj *decoded = getDecodedObject(o);
            o = createStringObject(decoded->ptr, sdslen(decoded->ptr));
            decrRefCount(decoded);
            dbOverwrite(c->db,c->argv[1],o);
        }
    }

    // �����޸Ĳ���
    if (sdslen(value) > 0)
    {
        // ���� 0 �ֽ����������Χ
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));

        // �������ݵ� key
        memcpy((char*)o->ptr+offset,value,sdslen(value));

        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }

    // ���޸ĺ��ַ����ĳ��ȷ��ظ��ͻ���
    addReplyLongLong(c,sdslen(o->ptr));
}

/*
 * GETRANGE �����ʵ��
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void getrangeCommand(redisClient *c)
{
    robj *o;
    long start, end;
    char *str, llbuf[32];
    size_t strlen;

    // ��ȡ start ����
    if (getLongFromObjectOrReply(c,c->argv[2],&start,NULL) != REDIS_OK)
        return;

    // ��ȡ end ����
    if (getLongFromObjectOrReply(c,c->argv[3],&end,NULL) != REDIS_OK)
        return;

    // ��� key �����ڣ����� key �����ַ������ͣ���ôֱ�ӷ���
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
            checkType(c,o,REDIS_STRING)) return;

    // ��ȡ�ַ������Լ����ĳ���
    if (o->encoding == REDIS_ENCODING_INT)
    {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    }
    else
    {
        str = o->ptr;
        strlen = sdslen(str);
    }

    // �Ը�����������ת��
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end)
    {
        addReply(c,shared.emptybulk);
    }
    else
    {
        // �����ַ�����������������
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/*
 * MGET �����ʵ��
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void mgetCommand(redisClient *c)
{
    int j;

    // ִ�ж����ȡ
    addReplyMultiBulkLen(c,c->argc-1);
    for (j = 1; j < c->argc; j++)
    {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL)
        {
            addReply(c,shared.nullbulk);
        }
        else
        {
            if (o->type != REDIS_STRING)
            {
                addReply(c,shared.nullbulk);
            }
            else
            {
                addReplyBulk(c,o);
            }
        }
    }
}

/*
 * MSET / MSETNX ����ĵײ�ʵ��
 *
 * ������
 *  nx  �����Ϊ 0 ����ôֻ���� key ������ʱ�Ž�������
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void msetGenericCommand(redisClient *c, int nx)
{
    int j, busykeys = 0;

    // �����������Ƿ�ɶ�
    if ((c->argc % 2) == 0)
    {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

    // �� nx ѡ���ʱ���������� key �Ƿ��Ѿ�����
    // ֻҪ�κ�һ�� key ���ڣ���ô�Ͳ������޸ģ�ֱ�ӷ��� 0
    if (nx)
    {
        for (j = 1; j < c->argc; j += 2)
        {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL)
            {
                busykeys++;
            }
        }
        // ������Ѵ��� key ����������ֱ�ӷ���
        if (busykeys)
        {
            addReply(c, shared.czero);
            return;
        }
    }

    // ִ�ж��д��
    for (j = 1; j < c->argc; j += 2)
    {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c->db,c->argv[j],c->argv[j+1]);
    }

    server.dirty += (c->argc-1)/2;

    addReply(c, nx ? shared.cone : shared.ok);
}

/*
 * MSET �����ʵ��
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void msetCommand(redisClient *c)
{
    msetGenericCommand(c,0);
}

/*
 * MSETNX �����ʵ��
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void msetnxCommand(redisClient *c)
{
    msetGenericCommand(c,1);
}

/*
 * �Ը����ַ����������ֵ���мӷ����߼�������
 * incr / decr / incrby �� decrby ������ĵײ�ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void incrDecrCommand(redisClient *c, long long incr)
{
    long long value, oldvalue;
    robj *o, *new;

    // ���� key
    o = lookupKeyWrite(c->db,c->argv[1]);

    // ��� key �ǿ��� key ���ʹ���ֱ�ӷ���
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;

    // ���ֵ����ת��Ϊ���֣�ֱ�ӷ���
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != REDIS_OK) return;

    // ����ֵ�Ƿ�����
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
            (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue)))
    {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }

    // �����ֵ
    value += incr;
    // ������������
    new = createStringObjectFromLongLong(value);
    // ���� o �����Ƿ���ڣ�ѡ��д�����½�����
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);

    signalModifiedKey(c->db,c->argv[1]);

    server.dirty++;

    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}

/*
 * INCR �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void incrCommand(redisClient *c)
{
    incrDecrCommand(c,1);
}

/*
 * DECR �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void decrCommand(redisClient *c)
{
    incrDecrCommand(c,-1);
}

/*
 * incrby �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void incrbyCommand(redisClient *c)
{
    long long incr;

    // ��ȡ������ֵ
    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;

    incrDecrCommand(c,incr);
}

/*
 * decrby �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void decrbyCommand(redisClient *c)
{
    long long incr;

    // ��ȡ������ֵ
    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;

    incrDecrCommand(c,-incr);
}

/*
 * INCRBYFLOAT �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void incrbyfloatCommand(redisClient *c)
{
    long double incr, value;
    robj *o, *new, *aux;

    // ��ȡ key ����
    o = lookupKeyWrite(c->db,c->argv[1]);

    // �����������Ҳ�Ϊ�ַ������ͣ�ֱ�ӷ���
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;

    // ������� o ���ߴ���������������ת��Ϊ��������ֱ�ӷ���
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != REDIS_OK ||
            getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != REDIS_OK)
        return;

    // �����
    value += incr;
    // ������
    if (isnan(value) || isinf(value))
    {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

    // ��ֵ���浽�¶��󣬲���дԭ�ж���
    new = createStringObjectFromLongDouble(value);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);

    signalModifiedKey(c->db,c->argv[1]);

    server.dirty++;

    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float pricision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}

/*
 * APPEND �����ʵ��
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��void
 */
void appendCommand(redisClient *c)
{
    size_t totlen;
    robj *o, *append;

    // ���� key ����
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL)
    {
        // ���󲻴��� ...

        // �����ַ������󣬲�������ӵ����ݿ�
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    }
    else
    {
        // �������...

        // ��������ַ�����ֱ�ӷ���
        if (checkType(c,o,REDIS_STRING))
            return;

        // ���ƴ����ɺ��ַ����ĳ����Ƿ�Ϸ�
        append = c->argv[2];
        totlen = stringObjectLen(o) + sdslen(append->ptr);
        if (checkStringLength(c,totlen) != REDIS_OK)
            return;

        // ��� key �����Ǳ������δ������ģ���ô����һ������
        if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW)
        {
            robj *decoded = getDecodedObject(o);
            o = createStringObject(decoded->ptr, sdslen(decoded->ptr));
            decrRefCount(decoded);
            dbOverwrite(c->db,c->argv[1],o);
        }

        // ����ƴ��
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReplyLongLong(c,totlen);
}

/*
 * STRLEN �����ʵ��
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��void
 */
void strlenCommand(redisClient *c)
{
    robj *o;

    // ��� o �����ڣ����߲�Ϊ string ���ͣ�ֱ�ӷ���
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
            checkType(c,o,REDIS_STRING)) return;

    addReplyLongLong(c,stringObjectLen(o));
}
