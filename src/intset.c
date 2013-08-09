/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"
#include "endianconv.h"

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

/*
 * ���ر��� v ����ĳ���
 *
 * T = theta(1)
 */
static uint8_t _intsetValueEncoding(int64_t v)
{
    if (v < INT32_MIN || v > INT32_MAX)
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX)
        return INTSET_ENC_INT32;
    else
        return INTSET_ENC_INT16;
}

/*
 * ���ݸ����ı��뷽ʽ�����ظ���λ���ϵ�ֵ
 *
 * T = theta(1)
 */
static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc)
{
    int64_t v64;
    int32_t v32;
    int16_t v16;

    if (enc == INTSET_ENC_INT64)
    {
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));
        memrev64ifbe(&v64);
        return v64;
    }
    else if (enc == INTSET_ENC_INT32)
    {
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32));
        memrev32ifbe(&v32);
        return v32;
    }
    else
    {
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16));
        memrev16ifbe(&v16);
        return v16;
    }
}

/*
 * ���� intset �ϸ��� pos ��ֵ
 *
 * T = theta(1)
 */
static int64_t _intsetGet(intset *is, int pos)
{
    return _intsetGetEncoded(is,pos,intrev32ifbe(is->encoding));
}

/*
 * �� intset �ϸ��� pos ��ֵ����Ϊ value
 *
 * T = theta(1)
 */
static void _intsetSet(intset *is, int pos, int64_t value)
{
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64)
    {
        ((int64_t*)is->contents)[pos] = value;
        memrev64ifbe(((int64_t*)is->contents)+pos);
    }
    else if (encoding == INTSET_ENC_INT32)
    {
        ((int32_t*)is->contents)[pos] = value;
        memrev32ifbe(((int32_t*)is->contents)+pos);
    }
    else
    {
        ((int16_t*)is->contents)[pos] = value;
        memrev16ifbe(((int16_t*)is->contents)+pos);
    }
}

/*
 * ����һ���յ� intset
 *
 * T = theta(1)
 */
intset *intsetNew(void)
{

    intset *is = zmalloc(sizeof(intset));

    is->encoding = intrev32ifbe(INTSET_ENC_INT16);
    is->length = 0;

    return is;
}

/*
 * ���� intset �Ĵ�С
 *
 * T = O(n)
 */
static intset *intsetResize(intset *is, uint32_t len)
{
    uint32_t size = len*intrev32ifbe(is->encoding);
    is = zrealloc(is,sizeof(intset)+size);
    return is;
}

/*
 * ���� value �� is �е�����
 *
 * ���ҳɹ�ʱ�����������浽 pos �������� 1 ��
 * ����ʧ��ʱ������ 0 ������ value ���Բ�����������浽 pos ��
 *
 * T = O(lg N)
 */
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos)
{
    int min = 0,
        max = intrev32ifbe(is->length)-1,
        mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (intrev32ifbe(is->length) == 0)
    {
        // is Ϊ��ʱ�����ǲ���ʧ��
        if (pos) *pos = 0;
        return 0;
    }
    else
    {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        if (value > _intsetGet(is,intrev32ifbe(is->length)-1))
        {
            // ֵ�� is �е����һ��ֵ(����Ԫ���е����ֵ)Ҫ��
            // ��ô���ֵӦ�ò��뵽 is ���
            if (pos) *pos = intrev32ifbe(is->length);
            return 0;
        }
        else if (value < _intsetGet(is,0))
        {
            // value ��Ϊ�µ���Сֵ�����뵽 is ��ǰ
            if (pos) *pos = 0;
            return 0;
        }
    }

    // �� is Ԫ�������н��ж��ֲ���
    while(max >= min)
    {
        mid = (min+max)/2;
        cur = _intsetGet(is,mid);
        if (value > cur)
        {
            min = mid+1;
        }
        else if (value < cur)
        {
            max = mid-1;
        }
        else
        {
            break;
        }
    }

    if (value == cur)
    {
        if (pos) *pos = mid;
        return 1;
    }
    else
    {
        if (pos) *pos = min;
        return 0;
    }
}

/* Upgrades the intset to a larger encoding and inserts the given integer. */
/*
 * ���� value ���� intset ��ʹ�õı��뷽ʽ���������������� intset
 * ��� value ���뵽�� intset �С�
 *
 * T = O(n)
 */
static intset *intsetUpgradeAndAdd(intset *is, int64_t value)
{

    // ��ǰֵ�ı�������
    uint8_t curenc = intrev32ifbe(is->encoding);
    // ��ֵ�ı�������
    uint8_t newenc = _intsetValueEncoding(value);

    // Ԫ������
    int length = intrev32ifbe(is->length);

    // ������ֵ�����λ�ã�0 Ϊͷ��1 Ϊβ��
    int prepend = value < 0 ? 1 : 0;

    //  �����±��룬�������±���� intset ��������
    is->encoding = intrev32ifbe(newenc);
    is = intsetResize(is,intrev32ifbe(is->length)+1);

    /* Upgrade back-to-front so we don't overwrite values.
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */
    // ������Ԫ�ؿ�ʼ�������²���
    // ����Ԫ�ز��뵽�ʼΪ���ӣ�֮ǰ��
    // | 1 | 2 | 3 |
    // ֮��
    // | 1 | 2 |                      |    3    |   �ز��� 3
    // | 1 |                |    2    |    3    |   �ز��� 2
    // |          |    1    |    2    |    3    |   �ز��� 1
    // |  ??????  |    1    |    2    |    3    |   ??? Ԥ������Ԫ�صĿ�λ
    //
    //  "prepend" ��Ϊ������ֵ�����õ�����ƫ����
    while(length--)
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));

    /* Set the value at the beginning or the end. */
    if (prepend)
        _intsetSet(is,0,value);
    else
        _intsetSet(is,intrev32ifbe(is->length),value);

    // ���� is Ԫ������
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);

    return is;
}

/*
 * �Դ� from ��ʼ���� is ĩβ���������ݽ����ƶ����� to Ϊ���
 *
 * ���� 3 Ϊ from , 2 Ϊ to ��
 * ֮ǰ��
 *   | 1 | 2 | 3 | 4 |
 * ֮��
 *   | 1 | 3 | 4 | 4 |
 *
 * T = theta(n)
 */
static void intsetMoveTail(intset *is, uint32_t from, uint32_t to)
{
    void *src, *dst;

    // ��Ҫ�ƶ���Ԫ�ظ���
    uint32_t bytes = intrev32ifbe(is->length)-from;

    // ������Ԫ�صı��뷽ʽ
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64)
    {
        // �����ַ
        src = (int64_t*)is->contents+from;
        dst = (int64_t*)is->contents+to;
        // ��Ҫ�ƶ����ֽ���
        bytes *= sizeof(int64_t);
    }
    else if (encoding == INTSET_ENC_INT32)
    {
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    }
    else
    {
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }

    // ���㣡
    memmove(dst,src,bytes);
}

/*
 * �� value ��ӵ�������
 *
 * ���Ԫ���Ѿ����ڣ� *success ������Ϊ 0 ��
 * ���Ԫ����ӳɹ��� *success ������Ϊ 1 ��
 *
 * T = O(n)
 */
intset *intsetAdd(intset *is, int64_t value, uint8_t *success)
{
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 1;

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    // �������Ҫ������������������ֵ
    if (valenc > intrev32ifbe(is->encoding))
    {
        /* This always succeeds, so we don't need to curry *success. */
        return intsetUpgradeAndAdd(is,value);
    }
    else
    {
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        // ���ֵ�Ѿ����ڣ���ôֱ�ӷ���
        // ��������ڣ���ô���� *pos ����Ϊ��Ԫ����ӵ�λ��
        if (intsetSearch(is,value,&pos))
        {
            if (success) *success = 0;
            return is;
        }

        // ���� is ��׼�������Ԫ��
        is = intsetResize(is,intrev32ifbe(is->length)+1);
        // ��� pos �������������һ��λ�ã�
        // ��ô�������е�ԭ��Ԫ�ؽ����ƶ�
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);
    }

    // �����Ԫ��
    _intsetSet(is,pos,value);
    // ����Ԫ������
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);

    return is;
}

/*
 * �� value �� intset ���Ƴ�
 *
 * �Ƴ��ɹ��� *success ����Ϊ 1 ��ʧ��������Ϊ 0 ��
 *
 * T = O(n)
 */
intset *intsetRemove(intset *is, int64_t value, int *success)
{
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 0;

    if (valenc <= intrev32ifbe(is->encoding) && // ���뷽ʽƥ��
            intsetSearch(is,value,&pos))            // ��λ�ñ��浽 pos
    {
        uint32_t len = intrev32ifbe(is->length);

        /* We know we can delete */
        if (success) *success = 1;

        /* Overwrite value with tail and update length */
        // ��� pos ���� is ����ĩβ����ô��ʽ��ɾ����
        // ����� pos = (len-1) ����ô�����ռ�ʱֵ�ͻ��Զ�����Ĩ��������
        if (pos < (len-1)) intsetMoveTail(is,pos+1,pos);

        // �����ռ䣬����������������
        is = intsetResize(is,len-1);
        is->length = intrev32ifbe(len-1);
    }

    return is;
}

/*
 * �鿴 value �Ƿ������ is
 *
 * T = O(lg N)
 */
uint8_t intsetFind(intset *is, int64_t value)
{
    uint8_t valenc = _intsetValueEncoding(value);
    return valenc <= intrev32ifbe(is->encoding) &&  // ���뷽ʽƥ��
           intsetSearch(is,value,NULL);             // ���� value
}

/*
 * �������һ�� intset ���Ԫ��
 *
 * T = theta(1)
 */
int64_t intsetRandom(intset *is)
{
    return _intsetGet(is,rand()%intrev32ifbe(is->length));
}

/*
 * �� is ��λ�� pos ��ֵ���浽 *value ���У������� 1 ��
 *
 * ��� pos ���� is ��Ԫ������(out of range)����ô���� 0 ��
 *
 * T = theta(1)
 */
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value)
{
    if (pos < intrev32ifbe(is->length))
    {
        *value = _intsetGet(is,pos);
        return 1;
    }
    return 0;
}

/*
 * ���� intset ��Ԫ������
 *
 * T = theta(1)
 */
uint32_t intsetLen(intset *is)
{
    return intrev32ifbe(is->length);
}

/*
 * ���ֽ���ʽ���� intset ռ�õĿռ��С
 *
 * T = theta(1)
 */
size_t intsetBlobLen(intset *is)
{
    return sizeof(intset)+intrev32ifbe(is->length)*intrev32ifbe(is->encoding);
}

#ifdef INTSET_TEST_MAIN
#include <sys/time.h>

void intsetRepr(intset *is)
{
    int i;
    for (i = 0; i < intrev32ifbe(is->length); i++)
    {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

void error(char *err)
{
    printf("%s\n", err);
    exit(1);
}

void ok(void)
{
    printf("OK\n");
}

long long usec(void)
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

#define assert(_e) ((_e)?(void)0:(_assert(#_e,__FILE__,__LINE__),exit(1)))
void _assert(char *estr, char *file, int line)
{
    printf("\n\n=== ASSERTION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n",file,line,estr);
}

intset *createSet(int bits, int size)
{
    uint64_t mask = (1<<bits)-1;
    uint64_t i, value;
    intset *is = intsetNew();

    for (i = 0; i < size; i++)
    {
        if (bits > 32)
        {
            value = (rand()*rand()) & mask;
        }
        else
        {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

void checkConsistency(intset *is)
{
    int i;

    for (i = 0; i < (intrev32ifbe(is->length)-1); i++)
    {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16)
        {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        }
        else if (encoding == INTSET_ENC_INT32)
        {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        }
        else
        {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

int main(int argc, char **argv)
{
    uint8_t success;
    int i;
    intset *is;
    sranddev();

    printf("Value encodings: ");
    {
        assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(-9223372036854775808ull) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+9223372036854775807ull) == INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: ");
    {
        is = intsetNew();
        is = intsetAdd(is,5,&success);
        assert(success);
        is = intsetAdd(is,6,&success);
        assert(success);
        is = intsetAdd(is,4,&success);
        assert(success);
        is = intsetAdd(is,4,&success);
        assert(!success);
        ok();
    }

    printf("Large number of random adds: ");
    {
        int inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++)
        {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int32: ");
    {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int64: ");
    {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int32 to int64: ");
    {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Stress lookups: ");
    {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",num,size,usec()-start);
    }

    printf("Stress add+delete: ");
    {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++)
        {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
    }
}
#endif
