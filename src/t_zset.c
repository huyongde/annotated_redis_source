/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

/*-----------------------------------------------------------------------------
 * Sorted set API
 *----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * Zset Ϊ���򼯺ϣ���ʹ���������ݽṹ����ͬһ������
 * ʹ�ÿ��������򼯺����� O(log(N)) ���Ӷ��ڽ�����Ӻ�ɾ��������
 *
 * The elements are added to an hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view").
 *
 * ��ϣ���� Redis ����Ϊ���� score Ϊֵ��
 * Skiplist ��ͬ�������� Redis ����� score ֵ��ӳ�䡣
 */

/* This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 *
 * ����� skiplist ʵ�ֺ� William Pugh �� "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees" �������Ĳ�ֻ࣬�������ط��������޸ģ�
 *
 * a) this implementation allows for repeated scores.
 *    ���ʵ�������ظ�ֵ
 * b) the comparison is not just by key (our 'score') but by satellite data.
 *    ������ score ���бȶԣ�����Ҫ�� Redis ���������Ϣ���бȶ�
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE.
 *    ÿ���ڵ㶼����һ��ǰ��ָ�룬���ڴӱ�β���ͷ������
 */

#include "redis.h"
#include <math.h>

/*
 * ����������һ����Ծ��ڵ�
 *
 * T = O(1)
 */
zskiplistNode *zslCreateNode(int level, double score, robj *obj)
{
    // �����
    zskiplistNode *zn = zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel));
    // ����
    zn->score = score;
    // ����
    zn->obj = obj;

    return zn;
}

/*
 * ����һ����Ծ��
 *
 * T = O(N)
 */
zskiplist *zslCreate(void)
{
    int j;
    zskiplist *zsl;

    zsl = zmalloc(sizeof(*zsl));

    zsl->level = 1;
    zsl->length = 0;

    // ��ʼ��ͷ�ڵ㣬 O(1)
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL);
    // ��ʼ����ָ�룬O(N)
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++)
    {
        zsl->header->level[j].forward = NULL;
        zsl->header->level[j].span = 0;
    }
    zsl->header->backward = NULL;

    zsl->tail = NULL;

    return zsl;
}

/*
 * �ͷ���Ծ��ڵ�
 *
 * T = O(1)
 */
void zslFreeNode(zskiplistNode *node)
{
    decrRefCount(node->obj);
    zfree(node);
}

/*
 * �ͷ�������Ծ��
 *
 * T = O(N)
 */
void zslFree(zskiplist *zsl)
{

    zskiplistNode *node = zsl->header->level[0].forward,
                   *next;

    zfree(zsl->header);

    // ����ɾ��, O(N)
    while(node)
    {
        next = node->level[0].forward;
        zslFreeNode(node);
        node = next;
    }

    zfree(zsl);
}

/* Returns a random level for the new skiplist node we are going to create.
 * The return value of this function is between 1 and ZSKIPLIST_MAXLEVEL
 * (both inclusive), with a powerlaw-alike distribution where higher
 * levels are less likely to be returned.
 *
 * ����һ������ 1 �� ZSKIPLIST_MAXLEVEL ֮������ֵ����Ϊ�ڵ�Ĳ�����
 *
 * �����ݴζ���(power law)����ֵԽ�󣬺����������ļ��ʾ�ԽС
 *
 * T = O(N)
 */
int zslRandomLevel(void)
{
    int level = 1;
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

/*
 * ���������� score �Ķ��� obj ��ӵ� skiplist ��
 *
 * T_worst = O(N), T_average = O(log N)
 */
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj)
{

    // ��¼Ѱ��Ԫ�ع����У�ÿ���ܵ�������ҽڵ�
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;

    // ��¼Ѱ��Ԫ�ع����У�ÿ������Խ�Ľڵ���
    unsigned int rank[ZSKIPLIST_MAXLEVEL];

    int i, level;

    redisAssert(!isnan(score));
    x = zsl->header;
    // ��¼��;���ʵĽڵ㣬������ span ������
    // ƽ�� O(log N) ��� O(N)
    for (i = zsl->level-1; i >= 0; i--)
    {
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];

        // �ҽڵ㲻Ϊ��
        while (x->level[i].forward &&
                // �ҽڵ�� score �ȸ��� score С
                (x->level[i].forward->score < score ||
                 // �ҽڵ�� score ��ͬ�����ڵ�� member ������ member ҪС
                 (x->level[i].forward->score == score &&
                  compareStringObjects(x->level[i].forward->obj,obj) < 0)))
        {
            // ��¼��Խ�˶��ٸ�Ԫ��
            rank[i] += x->level[i].span;
            // ��������ǰ��
            x = x->level[i].forward;
        }
        // ������ʽڵ�
        update[i] = x;
    }

    /* we assume the key is not already inside, since we allow duplicated
     * scores, and the re-insertion of score and redis object should never
     * happpen since the caller of zslInsert() should test in the hash table
     * if the element is already inside or not. */
    // ��Ϊ������������ܴ�������Ԫ�ص� member �� score ����ͬ�������
    // ����ֱ�Ӵ����½ڵ㣬���ü�������

    // �����µ��������
    level = zslRandomLevel();
    // ��� level �ȵ�ǰ skiplist ����������Ҫ��
    // ��ô���� zsl->level ����
    // ���ҳ�ʼ�� update �� rank ��������Ӧ�Ĳ������
    if (level > zsl->level)
    {
        for (i = zsl->level; i < level; i++)
        {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->level[i].span = zsl->length;
        }
        zsl->level = level;
    }

    // �����½ڵ�
    x = zslCreateNode(level,score,obj);
    // ���� update �� rank ������������ϣ���ʼ���½ڵ�
    // ��������Ӧ��ָ��
    // O(N)
    for (i = 0; i < level; i++)
    {
        // ����ָ��
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;

        /* update span covered by update[i] as x is inserted here */
        // ���� span
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }

    /* increment span for untouched levels */
    // ������;���ʽڵ�� span ֵ
    for (i = level; i < zsl->level; i++)
    {
        update[i]->level[i].span++;
    }

    // ���ú���ָ��
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    // ���� x ��ǰ��ָ��
    if (x->level[0].forward)
        x->level[0].forward->backward = x;
    else
        // ������µı�β�ڵ�
        zsl->tail = x;

    // ������Ծ��ڵ�����
    zsl->length++;

    return x;
}

/* Internal function used by zslDelete, zslDeleteByScore and zslDeleteByRank */
/*
 * �ڵ�ɾ������
 *
 * T = O(N)
 */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update)
{
    int i;

    // �޸���Ӧ��ָ��� span , O(N)
    for (i = 0; i < zsl->level; i++)
    {
        if (update[i]->level[i].forward == x)
        {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        }
        else
        {
            update[i]->level[i].span -= 1;
        }
    }

    // �����ͷ�ͱ�β�ڵ�
    if (x->level[0].forward)
    {
        x->level[0].forward->backward = x->backward;
    }
    else
    {
        zsl->tail = x->backward;
    }

    // ���� level ��ֵ, O(N)
    while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL)
        zsl->level--;

    zsl->length--;
}

/* Delete an element with matching score/object from the skiplist. */
/*
 * �� skiplist ��ɾ���͸��� obj �Լ����� score ƥ���Ԫ��
 *
 * T_worst = O(N), T_average = O(log N)
 */
int zslDelete(zskiplist *zsl, double score, robj *obj)
{
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    x = zsl->header;
    // �������в㣬��¼ɾ���ڵ����Ҫ���޸ĵĽڵ㵽 update ����
    for (i = zsl->level-1; i >= 0; i--)
    {
        while (x->level[i].forward &&
                (x->level[i].forward->score < score ||
                 (x->level[i].forward->score == score &&
                  compareStringObjects(x->level[i].forward->obj,obj) < 0)))
            x = x->level[i].forward;
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    // ��Ϊ�����ͬ�� member ��������ͬ�� score
    // ����Ҫȷ�� x �� member �� score ��ƥ��ʱ���Ž���ɾ��
    x = x->level[0].forward;
    if (x && score == x->score && equalStringObjects(x->obj,obj))
    {
        zslDeleteNode(zsl, x, update);
        zslFreeNode(x);
        return 1;
    }
    else
    {
        return 0; /* not found */
    }
    return 0; /* not found */
}

/*
 * ��� value �Ƿ����� spec ָ���ķ�Χ��
 *
 * T = O(1)
 */
static int zslValueGteMin(double value, zrangespec *spec)
{
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

/*
 * ��� value �Ƿ����� spec ָ���ķ�Χ��
 *
 * T = O(1)
 */
static int zslValueLteMax(double value, zrangespec *spec)
{
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/* Returns if there is a part of the zset is in range. */
/*
 * ��� zset �е�Ԫ���Ƿ��ڸ�����Χ֮��
 *
 * T = O(1)
 */
int zslIsInRange(zskiplist *zsl, zrangespec *range)
{
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    // ��� zset �����ڵ�� score �ȷ�Χ����СֵҪС
    // ��ô zset ���ڷ�Χ֮��
    x = zsl->tail;
    if (x == NULL || !zslValueGteMin(x->score,range))
        return 0;

    // ��� zset ����С�ڵ�� score �ȷ�Χ�����ֵҪ��
    // ��ô zset ���ڷ�Χ֮��
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslValueLteMax(x->score,range))
        return 0;

    // �ڷ�Χ��
    return 1;
}

/* Find the first node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
/*
 * �ҵ���Ծ���е�һ�����ϸ�����Χ��Ԫ��
 *
 * T_worst = O(N) , T_average = O(log N)
 */
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec range)
{
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl,&range)) return NULL;

    // �ҵ���һ�� score ֵ���ڸ�����Χ��Сֵ�Ľڵ�
    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--)
    {
        /* Go forward while *OUT* of range. */
        while (x->level[i].forward &&
                !zslValueGteMin(x->level[i].forward->score,&range))
            x = x->level[i].forward;
    }

    /* This is an inner range, so the next node cannot be NULL. */
    x = x->level[0].forward;
    redisAssert(x != NULL);

    /* Check if score <= max. */
    // O(1)
    if (!zslValueLteMax(x->score,&range)) return NULL;
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
/*
 * �ҵ���Ծ�������һ�����ϸ�����Χ��Ԫ��
 *
 * T_worst = O(N) , T_average = O(log N)
 */
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec range)
{
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl,&range)) return NULL;

    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--)
    {
        /* Go forward while *IN* range. */
        while (x->level[i].forward &&
                zslValueLteMax(x->level[i].forward->score,&range))
            x = x->level[i].forward;
    }

    /* This is an inner range, so this node cannot be NULL. */
    redisAssert(x != NULL);

    /* Check if score >= min. */
    if (!zslValueGteMin(x->score,&range)) return NULL;
    return x;
}

/* Delete all the elements with score between min and max from the skiplist.
 * Min and mx are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
/*
 * ɾ��������Χ�ڵ� score ��Ԫ�ء�
 *
 * T = O(N^2)
 */
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec range, dict *dict)
{
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    // ��¼��;�Ľڵ�
    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--)
    {
        while (x->level[i].forward && (range.minex ?
                                       x->level[i].forward->score <= range.min :
                                       x->level[i].forward->score < range.min))
            x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    // һֱ����ɾ����ֱ������ range �ĵ�Ϊֹ
    // O(N^2)
    while (x && (range.maxex ? x->score < range.max : x->score <= range.max))
    {
        // ������ָ��
        zskiplistNode *next = x->level[0].forward;
        // ����Ծ����ɾ��, O(N)
        zslDeleteNode(zsl,x,update);
        // ���ֵ���ɾ����O(1)
        dictDelete(dict,x->obj);
        // �ͷ�
        zslFreeNode(x);

        removed++;

        x = next;
    }

    return removed;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
/*
 * ɾ����������Χ�ڵ����нڵ�
 *
 * T = O(N^2)
 */
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict)
{
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    // ͨ������ rank ���ƶ���ɾ����ʼ�ĵط�
    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--)
    {
        while (x->level[i].forward && (traversed + x->level[i].span) < start)
        {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    // ���� start �ڵ�
    traversed++;
    // �� start ��ʼ��ɾ��ֱ���������� end ������ĩβ
    // O(N^2)
    x = x->level[0].forward;
    while (x && traversed <= end)
    {
        // �����һ�ڵ��ָ��
        zskiplistNode *next = x->level[0].forward;
        // ɾ�� skiplist �ڵ�, O(N)
        zslDeleteNode(zsl,x,update);
        // ɾ�� dict �ڵ�, O(1)
        dictDelete(dict,x->obj);
        // ɾ���ڵ�
        zslFreeNode(x);
        // ɾ������
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

/* Find the rank for an element by both score and key.
 * Returns 0 when the element cannot be found, rank otherwise.
 * Note that the rank is 1-based due to the span of zsl->header to the
 * first element. */
/*
 * ����Ŀ��Ԫ���������е� rank
 *
 * ���Ԫ�ز����������򼯣���ô���� 0 ��
 *
 * T = O(N)
 */
unsigned long zslGetRank(zskiplist *zsl, double score, robj *o)
{
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;

    x = zsl->header;
    // ���� ziplist �����ۻ���;�� span �� rank ���ҵ�Ŀ��Ԫ��ʱ���� rank
    // O(N)
    for (i = zsl->level-1; i >= 0; i--)
    {
        while (x->level[i].forward &&
                (x->level[i].forward->score < score ||
                 (x->level[i].forward->score == score &&
                  compareStringObjects(x->level[i].forward->obj,o) <= 0)))
        {
            // �ۻ�
            rank += x->level[i].span;
            // ǰ��
            x = x->level[i].forward;
        }

        /* x might be equal to zsl->header, so test if obj is non-NULL */
        // �ҵ�Ŀ��Ԫ��
        if (x->obj && equalStringObjects(x->obj,o))
        {
            return rank;
        }
    }
    return 0;
}

/* Finds an element by its rank. The rank argument needs to be 1-based. */
/*
 * ���ݸ����� rank ����Ԫ��
 *
 * T = O(N)
 */
zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank)
{
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;

    // ����ָ��ǰ����ֱ���ۻ��Ĳ��� traversed ���� rank Ϊֹ
    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--)
    {
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank)
        {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank)
        {
            return x;
        }
    }

    // û�ҵ�
    return NULL;
}

/* Populate the rangespec according to the objects min and max. */
/*
 * ���� min �� max ���󣬽� range ֵ���浽 spec �ϡ�
 *
 * T = O(1)
 */
static int zslParseRange(robj *min, robj *max, zrangespec *spec)
{
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    if (min->encoding == REDIS_ENCODING_INT)
    {
        spec->min = (long)min->ptr;
    }
    else
    {
        if (((char*)min->ptr)[0] == '(')
        {
            spec->min = strtod((char*)min->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return REDIS_ERR;
            spec->minex = 1;
        }
        else
        {
            spec->min = strtod((char*)min->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return REDIS_ERR;
        }
    }
    if (max->encoding == REDIS_ENCODING_INT)
    {
        spec->max = (long)max->ptr;
    }
    else
    {
        if (((char*)max->ptr)[0] == '(')
        {
            spec->max = strtod((char*)max->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return REDIS_ERR;
            spec->maxex = 1;
        }
        else
        {
            spec->max = strtod((char*)max->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return REDIS_ERR;
        }
    }

    return REDIS_OK;
}

/*-----------------------------------------------------------------------------
 * Ziplist-backed sorted set API
 *----------------------------------------------------------------------------*/

/*
 * ȡ�� sptr ��ָ��� ziplist �ڵ�� score ֵ
 *
 * T = O(1)
 */
double zzlGetScore(unsigned char *sptr)
{
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    char buf[128];
    double score;

    redisAssert(sptr != NULL);
    redisAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    if (vstr)
    {
        // �ַ���ֵ
        // ���������ʾ score ��һ���ǳ��������
        // ������ long long ���ͣ�
        // ����һ��������
        memcpy(buf,vstr,vlen);
        buf[vlen] = '\0';
        score = strtod(buf,NULL);
    }
    else
    {
        // ����ֵ
        score = vlong;
    }

    return score;
}

/* Compare element in sorted set with given element. */
/*
 * �� eptr �ڵ㱣���ֵ�� cstr ֮����жԱ�
 *
 * T = O(N)
 */
int zzlCompareElements(unsigned char *eptr, unsigned char *cstr, unsigned int clen)
{
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    redisAssert(ziplistGet(eptr,&vstr,&vlen,&vlong));
    if (vstr == NULL)
    {
        /* Store string representation of long long in buf. */
        // ����ڵ㱣���������ֵ��
        // ��ô����ת��Ϊ�ַ�����ʾ
        vlen = ll2string((char*)vbuf,sizeof(vbuf),vlong);
        vstr = vbuf;
    }

    // �Ա�
    // С�Ż���ֻ�Աȳ��Ƚ϶̵��ַ����ĳ��ȣ�
    minlen = (vlen < clen) ? vlen : clen;
    cmp = memcmp(vstr,cstr,minlen);
    if (cmp == 0) return vlen-clen;
    return cmp;
}

/*
 * ���� ziplist ��ʾ�����򼯵ĳ���
 *
 * T = O(N)
 */
unsigned int zzlLength(unsigned char *zl)
{
    // ÿ������������ ziplist �ڵ��ʾ
    // O(N)
    return ziplistLen(zl)/2;
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. */
/*
 * �ƶ�ָ��ָ�����򼯵��¸��ڵ�
 *
 * ���� eptr ָ���¸��ڵ�� member ��
 * sptr ָ���¸��ڵ�� score ��
 *
 * ������ ziplist ������ʱ������ NULL
 *
 * T = O(1)
 */
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr)
{
    unsigned char *_eptr, *_sptr;
    redisAssert(*eptr != NULL && *sptr != NULL);

    // ָ����һ�ڵ�� member ��
    _eptr = ziplistNext(zl,*sptr);
    if (_eptr != NULL)
    {
        // ָ����һ�ڵ�� score ��
        _sptr = ziplistNext(zl,_eptr);
        redisAssert(_sptr != NULL);
    }
    else
    {
        /* No next entry. */
        _sptr = NULL;
    }

    // ����ָ��
    *eptr = _eptr;
    *sptr = _sptr;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no next entry. */
/*
 * �ƶ��� eptr �� sptr ��ָ��Ľڵ��ǰһ���ڵ㣬
 * �������º��λ�ñ���� eptr �� sptr ��
 *
 * ����Ѿ����ﾡͷ��������ָ�붼��Ϊ NULL
 *
 * T = O(1)
 */
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr)
{
    unsigned char *_eptr, *_sptr;
    redisAssert(*eptr != NULL && *sptr != NULL);

    // ָ��ǰһ�ڵ�� score ��
    _sptr = ziplistPrev(zl,*eptr);
    if (_sptr != NULL)
    {
        // ָ��ǰһ�ڵ�� memeber ��
        _eptr = ziplistPrev(zl,_sptr);
        redisAssert(_eptr != NULL);
    }
    else
    {
        /* No previous entry. */
        _eptr = NULL;
    }

    // ����ָ��
    *eptr = _eptr;
    *sptr = _sptr;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
/*
 * ������򼯵� score ֵ�Ƿ��ڸ����� range ֮��
 *
 * T = O(1)
 */
int zzlIsInRange(unsigned char *zl, zrangespec *range)
{
    unsigned char *p;
    double score;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    // ȡ����������С�� score ֵ
    p = ziplistIndex(zl,-1); /* Last score. */
    if (p == NULL) return 0; /* Empty sorted set */
    score = zzlGetScore(p);
    // ��� score ֵ��λ�ڸ����߽�֮�ڣ����� 0
    if (!zslValueGteMin(score,range))
        return 0;

    // ȡ������������ score ֵ
    p = ziplistIndex(zl,1); /* First score. */
    redisAssert(p != NULL);
    score = zzlGetScore(p);
    // ��� score ֵ��λ�ڸ����߽�֮�ڣ����� 0
    if (!zslValueLteMax(score,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
/*
 * ���ص�һ�� score ֵ�ڸ�����Χ�ڵĽڵ�
 *
 * ���û�нڵ�� score ֵ�ڸ�����Χ������ NULL ��
 *
 * T = O(N)
 */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec range)
{
    // �ӱ�ͷ��ʼ����
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,&range)) return NULL;

    // �ӱ�ͷ���β����
    while (eptr != NULL)
    {
        sptr = ziplistNext(zl,eptr);
        redisAssert(sptr != NULL);

        // ��ȡ score ֵ
        score = zzlGetScore(sptr);
        // score ֵ�ڷ�Χ֮�ڣ�
        if (zslValueGteMin(score,&range))
        {
            /* Check if score <= max. */
            if (zslValueLteMax(score,&range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        // ����ָ��
        eptr = ziplistNext(zl,sptr);
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
/*
 * ���� score ֵ�ڸ�����Χ�ڵ����һ���ڵ�
 *
 * û��Ԫ�ذ�����ʱ������ NULL
 *
 * T = O(N)
 */
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec range)
{
    // �ӱ�β��ʼ����
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,&range)) return NULL;

    // ������� ziplist ��ӱ�β����ͷ����
    while (eptr != NULL)
    {
        sptr = ziplistNext(zl,eptr);
        redisAssert(sptr != NULL);

        // ��ȡ�ڵ�� score ֵ
        score = zzlGetScore(sptr);
        // score �ڸ����ķ�Χ֮�ڣ�
        if (zslValueLteMax(score,&range))
        {
            /* Check if score >= min. */
            if (zslValueGteMin(score,&range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        // ǰ��ָ��
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            redisAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

/*
 * �� ziplist ����Ҹ���Ԫ�� ele ������ҵ��ˣ�
 * ��Ԫ�صĵ������浽 score �������ظ�Ԫ���� ziplist ��ָ�롣
 *
 * T = O(N^2)
 */
unsigned char *zzlFind(unsigned char *zl, robj *ele, double *score)
{

    // ������
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;

    // ����
    ele = getDecodedObject(ele);

    // �������� ziplist �� O(N^2)
    while (eptr != NULL)
    {
        sptr = ziplistNext(zl,eptr);
        redisAssertWithInfo(NULL,ele,sptr != NULL);

        // �Ա�Ԫ�� ele ��ֵ�� eptr �������ֵ
        // O(N)
        if (ziplistCompare(eptr,ele->ptr,sdslen(ele->ptr)))
        {
            /* Matching element, pull out score. */
            // ��ƥ��Ԫ�ص�ָ�뱣�浽 score ��
            if (score != NULL) *score = zzlGetScore(sptr);

            decrRefCount(ele);

            // ����ָ��
            return eptr;
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr);
    }

    decrRefCount(ele);
    return NULL;
}

/* Delete (element,score) pair from ziplist. Use local copy of eptr because we
 * don't want to modify the one given as argument. */
/*
 * �� ziplist ��ɾ�� element-score �ԡ�
 * ʹ��һ���������� eptr ��ֵ��
 *
 * T = O(N^2)
 */
unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr)
{
    unsigned char *p = eptr;

    /* TODO: add function to ziplist API to delete N elements from offset. */
    // ɾ�� member �� ��O(N^2)
    zl = ziplistDelete(zl,&p);
    // ɾ�� score �� ��O(N^2)
    zl = ziplistDelete(zl,&p);

    return zl;
}

/*
 * �����򼯽ڵ㱣�浽 eptr ��ָ��ĵط�
 *
 * T = O(N^2)
 */
unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr, robj *ele, double score)
{
    unsigned char *sptr;
    char scorebuf[128];
    int scorelen;
    size_t offset;

    redisAssertWithInfo(NULL,ele,ele->encoding == REDIS_ENCODING_RAW);
    // �� score ֵת��Ϊ�ַ���
    scorelen = d2string(scorebuf,sizeof(scorebuf),score);
    if (eptr == NULL)
    {
        // ���뵽 ziplist �����, O(N^2)
        // ziplist �ĵ�һ���ڵ㱣�����򼯵� member
        zl = ziplistPush(zl,ele->ptr,sdslen(ele->ptr),ZIPLIST_TAIL);
        // ziplist �ĵڶ����ڵ㱣�����򼯵� score
        zl = ziplistPush(zl,(unsigned char*)scorebuf,scorelen,ZIPLIST_TAIL);
    }
    else
    {
        // ���뵽����λ��, O(N^2)
        /* Keep offset relative to zl, as it might be re-allocated. */
        // ��¼ ziplist �����ƫ������������ָ�룩�������ڴ��ط���֮��λ�ö�ʧ
        offset = eptr-zl;
        // ���� member
        zl = ziplistInsert(zl,eptr,ele->ptr,sdslen(ele->ptr));
        eptr = zl+offset;

        /* Insert score after the element. */
        redisAssertWithInfo(NULL,ele,(sptr = ziplistNext(zl,eptr)) != NULL);
        // ���� score
        zl = ziplistInsert(zl,sptr,(unsigned char*)scorebuf,scorelen);
    }

    return zl;
}

/* Insert (element,score) pair in ziplist. This function assumes the element is
 * not yet present in the list. */
/*
 * �� ele ��Ա�����ķ�ֵ score ��ӵ� ziplist ����
 *
 * ziplist ��ĸ����ڵ㰴 score ֵ��С��������
 *
 * ����������� elem ������������
 *
 * T = O(N^2)
 */
unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score)
{
    // ָ�� ziplist ��һ���ڵ㣨Ҳ�������򼯵� member ��
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;
    double s;

    // ����ֵ
    ele = getDecodedObject(ele);
    // �������� ziplist
    while (eptr != NULL)
    {
        // ָ�� score ��
        sptr = ziplistNext(zl,eptr);
        redisAssertWithInfo(NULL,ele,sptr != NULL);
        // ȡ�� score ֵ
        s = zzlGetScore(sptr);

        if (s > score)
        {
            /* First element with score larger than score for element to be
             * inserted. This means we should take its spot in the list to
             * maintain ordering. */
            // ������һ�� score ֵ������ score ��Ľڵ�
            // ���½ڵ����������ڵ��ǰ�棬
            // �ýڵ��� ziplist ����� score ��С��������
            // O(N^2)
            zl = zzlInsertAt(zl,eptr,ele,score);
            break;
        }
        else if (s == score)
        {
            /* Ensure lexicographical ordering for elements. */
            // ������� score �ͽڵ�� score ��ͬ
            // ��ô���� member ���ַ���λ���������½ڵ�Ĳ���λ��
            if (zzlCompareElements(eptr,ele->ptr,sdslen(ele->ptr)) > 0)
            {
                // O(N^2)
                zl = zzlInsertAt(zl,eptr,ele,score);
                break;
            }
        }

        /* Move to next element. */
        // ���� score �Ƚڵ�� score ֵҪ��
        // �ƶ�����һ���ڵ�
        eptr = ziplistNext(zl,sptr);
    }

    /* Push on tail of list when it was not yet inserted. */
    // ���������Ŀǰû��һ���ڵ�� score ֵ������ score ��
    // ��ô���½ڵ���ӵ� ziplist �����
    if (eptr == NULL)
        // O(N^2)
        zl = zzlInsertAt(zl,NULL,ele,score);

    decrRefCount(ele);
    return zl;
}

/*
 * ɾ������ score ��Χ�ڵĽڵ�
 *
 * T = O(N^3)
 */
unsigned char *zzlDeleteRangeByScore(unsigned char *zl, zrangespec range, unsigned long *deleted)
{
    unsigned char *eptr, *sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    // �� range ��λ��ʼ�ڵ��λ��
    eptr = zzlFirstInRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. */
    // һֱ����ɾ����ֱ������ score ֵ�� range->max ����Ľڵ�Ϊֹ
    // O(N^3)
    while ((sptr = ziplistNext(zl,eptr)) != NULL)
    {
        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,&range))
        {
            /* Delete both the element and the score. */
            // O(N^2)
            zl = ziplistDelete(zl,&eptr);
            // O(N^2)
            zl = ziplistDelete(zl,&eptr);
            num++;
        }
        else
        {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
/*
 * ɾ���������������ڵ����нڵ�
 *
 * T = O(N^2)
 */
unsigned char *zzlDeleteRangeByRank(unsigned char *zl, unsigned int start, unsigned int end, unsigned long *deleted)
{
    // ����ɾ���Ľڵ�����
    unsigned int num = (end-start)+1;
    if (deleted) *deleted = num;
    // ɾ��
    zl = ziplistDeleteRange(zl,2*(start-1),2*num);
    return zl;
}

/*-----------------------------------------------------------------------------
 * Common sorted set API
 *----------------------------------------------------------------------------*/

/*
 * �������򼯵�Ԫ�ظ���
 *
 * T = O(N)
 */
unsigned int zsetLength(robj *zobj)
{
    int length = -1;
    // O(N)
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        length = zzlLength(zobj->ptr);
        // O(1)
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        length = ((zset*)zobj->ptr)->zsl->length;
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }
    return length;
}

/*
 * �������� zobj ת���ɸ�������
 *
 * T = O(N^3)
 */
void zsetConvert(robj *zobj, int encoding)
{
    zset *zs;
    zskiplistNode *node, *next;
    robj *ele;
    double score;

    // ������ͬ������ת��
    if (zobj->encoding == encoding) return;

    // �� ziplist ����ת���� skiplist ����
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != REDIS_ENCODING_SKIPLIST)
            redisPanic("Unknown target encoding");

        // ������ zset
        zs = zmalloc(sizeof(*zs));
        zs->dict = dictCreate(&zsetDictType,NULL);
        zs->zsl = zslCreate();

        // ָ���һ���ڵ�� member ��
        eptr = ziplistIndex(zl,0);
        redisAssertWithInfo(NULL,zobj,eptr != NULL);
        // ָ���һ���ڵ�� score ��
        sptr = ziplistNext(zl,eptr);
        redisAssertWithInfo(NULL,zobj,sptr != NULL);

        // �������� ziplist �������� member �� score ��ӵ� zset
        // O(N^2)
        while (eptr != NULL)
        {
            // ȡ�� score ֵ
            score = zzlGetScore(sptr);
            // ȡ�� member ֵ
            redisAssertWithInfo(NULL,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));

            // Ϊ member ֵ���� robj ����
            if (vstr == NULL)
                ele = createStringObjectFromLongLong(vlong);
            else
                ele = createStringObject((char*)vstr,vlen);

            /* Has incremented refcount since it was just created. */
            // �� score �� member�������ele����ӵ� skiplist
            // O(N)
            node = zslInsert(zs->zsl,score,ele);
            // �� member ��Ϊ���� score ��Ϊֵ�����浽�ֵ�
            // O(1)
            redisAssertWithInfo(NULL,zobj,dictAdd(zs->dict,ele,&node->score) == DICT_OK);
            incrRefCount(ele); /* Added to dictionary. */

            // ǰ�����¸��ڵ�
            zzlNext(zl,&eptr,&sptr);
        }

        zfree(zobj->ptr);
        zobj->ptr = zs;
        zobj->encoding = REDIS_ENCODING_SKIPLIST;

        // �� skiplist ת��Ϊ ziplist
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        unsigned char *zl = ziplistNew();

        if (encoding != REDIS_ENCODING_ZIPLIST)
            redisPanic("Unknown target encoding");

        /* Approach similar to zslFree(), since we want to free the skiplist at
         * the same time as creating the ziplist. */
        zs = zobj->ptr;
        // �ͷ������ֵ�
        dictRelease(zs->dict);
        // ָ���׸��ڵ�
        node = zs->zsl->header->level[0].forward;
        // �ͷ� zset ��ͷ
        zfree(zs->zsl->header);
        zfree(zs->zsl);

        // ������Ԫ�ر��浽 ziplist , O(N^3)
        while (node)
        {
            // ȡ�������� member
            ele = getDecodedObject(node->obj);
            // ���� member �� score �� ziplist, O(N^2)
            zl = zzlInsertAt(zl,NULL,ele,node->score);
            decrRefCount(ele);

            next = node->level[0].forward;
            zslFreeNode(node);
            node = next;
        }

        zfree(zs);
        zobj->ptr = zl;
        zobj->encoding = REDIS_ENCODING_ZIPLIST;
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Sorted set commands
 *----------------------------------------------------------------------------*/

/* This generic command implements both ZADD and ZINCRBY. */
/*
 * ��̬��Ӳ���
 *
 * ZADD �� ZINCRBY �ĵײ�ʵ��
 *
 * T = O(N^4)
 */
void zaddGenericCommand(redisClient *c, int incr)
{
    static char *nanerr = "resulting score is not a number (NaN)";
    robj *key = c->argv[1];
    robj *ele;
    robj *zobj;
    robj *curobj;
    double score = 0, *scores, curscore = 0.0;
    int j, elements = (c->argc-2)/2;
    int added = 0;

    // ���� member - score �ԣ�ֱ�ӱ���
    if (c->argc % 2)
    {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Start parsing all the scores, we need to emit any syntax error
     * before executing additions to the sorted set, as the command should
     * either execute fully or nothing at all. */
    // parse ��������� score
    // ������ִ���������������
    scores = zmalloc(sizeof(double)*elements);
    for (j = 0; j < elements; j++)
    {
        if (getDoubleFromObjectOrReply(c,c->argv[2+j*2],&scores[j],NULL)
                != REDIS_OK)
        {
            zfree(scores);
            return;
        }
    }

    /* Lookup the key and create the sorted set if does not exist. */
    // �� key �������򼯺ϣ���� key Ϊ�վʹ���һ��
    zobj = lookupKeyWrite(c->db,key);
    // ���󲻴��ڣ���������
    if (zobj == NULL)
    {

        // ���� skiplist ����� zset
        if (server.zset_max_ziplist_entries == 0 ||
                server.zset_max_ziplist_value < sdslen(c->argv[3]->ptr))
        {
            zobj = createZsetObject();
            // ���� ziplist ����� zset
        }
        else
        {
            zobj = createZsetZiplistObject();
        }

        // ��������򼯵� db
        dbAdd(c->db,key,zobj);
    }
    else
    {
        // ���Ѵ��ڶ���������ͼ��
        if (zobj->type != REDIS_ZSET)
        {
            addReply(c,shared.wrongtypeerr);
            zfree(scores);
            return;
        }
    }

    // ��������Ԫ�أ������Ǽ��뵽����
    // O(N^4)
    for (j = 0; j < elements; j++)
    {
        score = scores[j];

        // ���Ԫ�ص� ziplist ���������, O(N^3)
        if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
        {
            unsigned char *eptr;

            /* Prefer non-encoded element when dealing with ziplists. */
            // ��ȡԪ��
            ele = c->argv[3+j*2];
            // ���Ԫ�ش��ڣ���ôȡ����
            // O(N)
            if ((eptr = zzlFind(zobj->ptr,ele,&curscore)) != NULL)
            {
                // zincrby ������׷�� incr ��Ԫ�ص�ֵ
                if (incr)
                {
                    score += curscore;
                    // ������
                    if (isnan(score))
                    {
                        addReplyError(c,nanerr);
                        /* Don't need to check if the sorted set is empty
                         * because we know it has at least one element. */
                        zfree(scores);
                        return;
                    }
                }

                /* Remove and re-insert when score changed. */
                // �������Ԫ�ص�����ͬ����ô����Ԫ���滻��Ԫ��
                if (score != curscore)
                {
                    // ɾ����Ԫ��-����, O(N^2)
                    zobj->ptr = zzlDelete(zobj->ptr,eptr);
                    // ������Ԫ��-����, O(N^2)
                    zobj->ptr = zzlInsert(zobj->ptr,ele,score);

                    signalModifiedKey(c->db,key);
                    server.dirty++;
                }
                // Ԫ�ز�����
            }
            else
            {
                /* Optimize: check if the element is too large or the list
                 * becomes too long *before* executing zzlInsert. */
                // ���Ԫ�ص� ziplist
                // O(N^2)
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);

                // �������Ҫ���� ziplist ת��Ϊ skiplist ����
                if (zzlLength(zobj->ptr) > server.zset_max_ziplist_entries)
                    // O(N^3)
                    zsetConvert(zobj,REDIS_ENCODING_SKIPLIST);
                if (sdslen(ele->ptr) > server.zset_max_ziplist_value)
                    // O(N^3)
                    zsetConvert(zobj,REDIS_ENCODING_SKIPLIST);

                signalModifiedKey(c->db,key);
                server.dirty++;

                // ���� incrby ���� zadd ������
                if (!incr) added++;
            }
            // ���Ԫ�ص� skiplist
        }
        else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
        {
            zset *zs = zobj->ptr;
            zskiplistNode *znode;
            dictEntry *de;

            // ����Ԫ��
            ele = c->argv[3+j*2] = tryObjectEncoding(c->argv[3+j*2]);

            // ���ֵ��в���Ԫ��, O(1)
            de = dictFind(zs->dict,ele);

            // Ԫ�ش��ڣ����� score��
            if (de != NULL)
            {
                // ��ǰ member
                curobj = dictGetKey(de);
                // ��ǰ score
                curscore = *(double*)dictGetVal(de);

                // INCRBY ����
                if (incr)
                {
                    score += curscore;
                    // ������
                    if (isnan(score))
                    {
                        addReplyError(c,nanerr);
                        /* Don't need to check if the sorted set is empty
                         * because we know it has at least one element. */
                        zfree(scores);
                        return;
                    }
                }

                /* Remove and re-insert when score changed. We can safely
                 * delete the key object from the skiplist, since the
                 * dictionary still has a reference to it. */
                // �¾� score ֵ��ͬ���Ƚ���Ԫ�أ��ͷ�ֵ��ɾ������������ӵ� ziplist
                if (score != curscore)
                {
                    // ɾ�� zset �ɽڵ�, O(N)
                    redisAssertWithInfo(c,curobj,zslDelete(zs->zsl,curscore,curobj));

                    // ��Ӵ��µ����Ľڵ㵽 zset, O(N)
                    znode = zslInsert(zs->zsl,score,curobj);

                    incrRefCount(curobj); /* Re-inserted in skiplist. */

                    // �����ֵ䱣���Ԫ�صķ�ֵ
                    dictGetVal(de) = &znode->score; /* Update score ptr. */

                    signalModifiedKey(c->db,key);
                    server.dirty++;
                }
                // Ԫ�ز����ڣ���Ӳ�����
            }
            else
            {

                // ��ӵ� zset, O(N)
                znode = zslInsert(zs->zsl,score,ele);
                incrRefCount(ele); /* Inserted in skiplist. */

                // ��ӵ� dict , O(1)
                redisAssertWithInfo(c,NULL,dictAdd(zs->dict,ele,&znode->score) == DICT_OK);
                incrRefCount(ele); /* Added to dictionary. */

                signalModifiedKey(c->db,key);
                server.dirty++;

                // ��Ӳ���
                if (!incr) added++;
            }
        }
        else
        {
            redisPanic("Unknown sorted set encoding");
        }
    }

    zfree(scores);

    if (incr) /* ZINCRBY */
        addReplyDouble(c,score);
    else /* ZADD */
        addReplyLongLong(c,added);
}

void zaddCommand(redisClient *c)
{
    zaddGenericCommand(c,0);
}

void zincrbyCommand(redisClient *c)
{
    zaddGenericCommand(c,1);
}

/*
 * ��̬Ԫ��ɾ������
 *
 * T = O(N^3)
 */
void zremCommand(redisClient *c)
{
    robj *key = c->argv[1];
    robj *zobj;
    int deleted = 0, j;

    // ���Ҷ��󣬼������
    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
            checkType(c,zobj,REDIS_ZSET)) return;

    // ziplist
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *eptr;

        // O(N^3)
        for (j = 2; j < c->argc; j++)
        {
            // ���Ԫ�ش��ڣ���ô��������ɾ��, O(N^2)
            if ((eptr = zzlFind(zobj->ptr,c->argv[j],NULL)) != NULL)
            {
                deleted++;
                // O(N^2)
                zobj->ptr = zzlDelete(zobj->ptr,eptr);

                // �������Ϊ�գ���ôɾ����
                if (zzlLength(zobj->ptr) == 0)
                {
                    dbDelete(c->db,key);
                    break;
                }
            }
        }
        // ��Ծ��
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        dictEntry *de;
        double score;

        // O(N^2)
        for (j = 2; j < c->argc; j++)
        {
            // ȡ��Ԫ�� O(1)
            de = dictFind(zs->dict,c->argv[j]);
            if (de != NULL)
            {
                deleted++;

                /* Delete from the skiplist */
                // ɾ�� score ��
                score = *(double*)dictGetVal(de);
                // �� skiplist ��ɾ��Ԫ��
                // O(N)
                redisAssertWithInfo(c,c->argv[j],zslDelete(zs->zsl,score,c->argv[j]));

                /* Delete from the hash table */
                // ���ֵ���ɾ��Ԫ��
                // O(1)
                dictDelete(zs->dict,c->argv[j]);
                // O(N)
                if (htNeedsResize(zs->dict)) dictResize(zs->dict);
                if (dictSize(zs->dict) == 0)
                {
                    dbDelete(c->db,key);
                    break;
                }
            }
        }
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }

    if (deleted)
    {
        signalModifiedKey(c->db,key);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

/*
 * ��̬���Ƴ����� score ��Χ�ڵ�Ԫ��
 *
 * T = O(N^2)
 */
void zremrangebyscoreCommand(redisClient *c)
{
    robj *key = c->argv[1];
    robj *zobj;
    zrangespec range;
    unsigned long deleted;

    /* Parse the range arguments. */
    // �������뷶Χ
    if (zslParseRange(c->argv[2],c->argv[3],&range) != REDIS_OK)
    {
        addReplyError(c,"min or max is not a float");
        return;
    }

    // ���� key ���������
    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
            checkType(c,zobj,REDIS_ZSET)) return;

    // ziplist
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // O(N^3)
        zobj->ptr = zzlDeleteRangeByScore(zobj->ptr,range,&deleted);
        // ɾ���� ziplist
        if (zzlLength(zobj->ptr) == 0) dbDelete(c->db,key);
        // skiplist
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        // O(N^2)
        deleted = zslDeleteRangeByScore(zs->zsl,range,zs->dict);
        // �����ռ�
        if (htNeedsResize(zs->dict)) dictResize(zs->dict);
        // ɾ�����ֵ�
        if (dictSize(zs->dict) == 0) dbDelete(c->db,key);
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }

    if (deleted) signalModifiedKey(c->db,key);

    server.dirty += deleted;
    addReplyLongLong(c,deleted);
}

/*
 * ɾ����������Χ�ڵ�����Ԫ��
 *
 * T = O(N^2)
 */
void zremrangebyrankCommand(redisClient *c)
{
    robj *key = c->argv[1];
    robj *zobj;
    long start;
    long end;
    int llen;
    unsigned long deleted;

    // ���� start �� end ����
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
            (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
            checkType(c,zobj,REDIS_ZSET)) return;

    /* Sanitize indexes. */
    llen = zsetLength(zobj);
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen)
    {
        addReply(c,shared.czero);
        return;
    }
    if (end >= llen) end = llen-1;

    // ziplist
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        /* Correct for 1-based rank. */
        // O(N^2)
        zobj->ptr = zzlDeleteRangeByRank(zobj->ptr,start+1,end+1,&deleted);
        if (zzlLength(zobj->ptr) == 0) dbDelete(c->db,key);
        // skiplist
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;

        /* Correct for 1-based rank. */
        // O(N^2)
        deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict);
        if (htNeedsResize(zs->dict)) dictResize(zs->dict);
        if (dictSize(zs->dict) == 0) dbDelete(c->db,key);
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }

    if (deleted) signalModifiedKey(c->db,key);
    server.dirty += deleted;
    addReplyLongLong(c,deleted);
}

/*
 * �������ṹ
 */
typedef struct
{
    // ����Ŀ��
    robj *subject;
    // ���ͣ������Ǽ��ϻ�����
    int type; /* Set, sorted set */
    // ����
    int encoding;
    // Ȩ��
    double weight;

    union
    {
        // ���ϵ�����
        /* Set iterators. */
        union _iterset
        {
            struct
            {
                intset *is;
                int ii;
            } is;
            struct
            {
                dict *dict;
                dictIterator *di;
                dictEntry *de;
            } ht;
        } set;

        /* Sorted set iterators. */
        // ���򼯵�����
        union _iterzset
        {
            // ziplist ����
            struct
            {
                unsigned char *zl;
                unsigned char *eptr, *sptr;
            } zl;
            // zset ����
            struct
            {
                zset *zs;
                zskiplistNode *node;
            } sl;
        } zset;
    } iter;
} zsetopsrc;


/* Use dirty flags for pointers that need to be cleaned up in the next
 * iteration over the zsetopval. The dirty flag for the long long value is
 * special, since long long values don't need cleanup. Instead, it means that
 * we already checked that "ell" holds a long long, or tried to convert another
 * representation into a long long value. When this was successful,
 * OPVAL_VALID_LL is set as well. */
#define OPVAL_DIRTY_ROBJ 1
#define OPVAL_DIRTY_LL 2
#define OPVAL_VALID_LL 4

/* Store value retrieved from the iterator. */
/*
 * ����ӵ�����ȡ�õ�ֵ
 */
typedef struct
{
    int flags;
    unsigned char _buf[32]; /* Private buffer. */
    // ���ܱ��� member �ļ�������
    robj *ele;
    unsigned char *estr;
    unsigned int elen;
    long long ell;
    // score ֵ
    double score;
} zsetopval;

typedef union _iterset iterset;
typedef union _iterzset iterzset;

/*
 * ��ʼ��������
 */
void zuiInitIterator(zsetopsrc *op)
{
    if (op->subject == NULL)
        return;

    if (op->type == REDIS_SET)
    {
        iterset *it = &op->iter.set;
        if (op->encoding == REDIS_ENCODING_INTSET)
        {
            it->is.is = op->subject->ptr;
            it->is.ii = 0;
        }
        else if (op->encoding == REDIS_ENCODING_HT)
        {
            it->ht.dict = op->subject->ptr;
            it->ht.di = dictGetIterator(op->subject->ptr);
            it->ht.de = dictNext(it->ht.di);
        }
        else
        {
            redisPanic("Unknown set encoding");
        }
    }
    else if (op->type == REDIS_ZSET)
    {
        iterzset *it = &op->iter.zset;
        if (op->encoding == REDIS_ENCODING_ZIPLIST)
        {
            it->zl.zl = op->subject->ptr;
            it->zl.eptr = ziplistIndex(it->zl.zl,0);
            if (it->zl.eptr != NULL)
            {
                it->zl.sptr = ziplistNext(it->zl.zl,it->zl.eptr);
                redisAssert(it->zl.sptr != NULL);
            }
        }
        else if (op->encoding == REDIS_ENCODING_SKIPLIST)
        {
            it->sl.zs = op->subject->ptr;
            it->sl.node = it->sl.zs->zsl->header->level[0].forward;
        }
        else
        {
            redisPanic("Unknown sorted set encoding");
        }
    }
    else
    {
        redisPanic("Unsupported type");
    }
}

/*
 * ��յ�����
 */
void zuiClearIterator(zsetopsrc *op)
{
    if (op->subject == NULL)
        return;

    if (op->type == REDIS_SET)
    {
        iterset *it = &op->iter.set;
        if (op->encoding == REDIS_ENCODING_INTSET)
        {
            REDIS_NOTUSED(it); /* skip */
        }
        else if (op->encoding == REDIS_ENCODING_HT)
        {
            dictReleaseIterator(it->ht.di);
        }
        else
        {
            redisPanic("Unknown set encoding");
        }
    }
    else if (op->type == REDIS_ZSET)
    {
        iterzset *it = &op->iter.zset;
        if (op->encoding == REDIS_ENCODING_ZIPLIST)
        {
            REDIS_NOTUSED(it); /* skip */
        }
        else if (op->encoding == REDIS_ENCODING_SKIPLIST)
        {
            REDIS_NOTUSED(it); /* skip */
        }
        else
        {
            redisPanic("Unknown sorted set encoding");
        }
    }
    else
    {
        redisPanic("Unsupported type");
    }
}

/*
 * ���ص��������Ԫ������
 */
int zuiLength(zsetopsrc *op)
{
    if (op->subject == NULL)
        return 0;

    if (op->type == REDIS_SET)
    {
        iterset *it = &op->iter.set;
        if (op->encoding == REDIS_ENCODING_INTSET)
        {
            return intsetLen(it->is.is);
        }
        else if (op->encoding == REDIS_ENCODING_HT)
        {
            return dictSize(it->ht.dict);
        }
        else
        {
            redisPanic("Unknown set encoding");
        }
    }
    else if (op->type == REDIS_ZSET)
    {
        iterzset *it = &op->iter.zset;
        if (op->encoding == REDIS_ENCODING_ZIPLIST)
        {
            return zzlLength(it->zl.zl);
        }
        else if (op->encoding == REDIS_ENCODING_SKIPLIST)
        {
            return it->sl.zs->zsl->length;
        }
        else
        {
            redisPanic("Unknown sorted set encoding");
        }
    }
    else
    {
        redisPanic("Unsupported type");
    }
}

/* Check if the current value is valid. If so, store it in the passed structure
 * and move to the next element. If not valid, this means we have reached the
 * end of the structure and can abort. */
/*
 * �ӵ�������ȡ����һ��Ԫ�أ����������浽 val ��Ȼ�󷵻� 1 ��
 *
 * ��û����һ��Ԫ��ʱ������ 0 ��
 *
 * T = O(N)
 */
int zuiNext(zsetopsrc *op, zsetopval *val)
{
    if (op->subject == NULL)
        return 0;

    if (val->flags & OPVAL_DIRTY_ROBJ)
        decrRefCount(val->ele);

    // ����
    memset(val,0,sizeof(zsetopval));

    // �����Ǽ���
    if (op->type == REDIS_SET)
    {
        iterset *it = &op->iter.set;
        // intset ����
        if (op->encoding == REDIS_ENCODING_INTSET)
        {
            int64_t ell;

            // O(1)
            if (!intsetGet(it->is.is,it->is.ii,&ell))
                return 0;
            // ȡ��member
            val->ell = ell;
            // ����Ԫ��û�� score ��Ϊ������һ��Ĭ�� score
            val->score = 1.0;

            /* Move to next element. */
            it->is.ii++;
            // ht ����
        }
        else if (op->encoding == REDIS_ENCODING_HT)
        {
            if (it->ht.de == NULL)
                return 0;
            // ȡ��member
            // O(1)
            val->ele = dictGetKey(it->ht.de);
            // ����Ԫ��û�� score ��Ϊ������һ��Ĭ�� score
            val->score = 1.0;

            /* Move to next element. */
            it->ht.de = dictNext(it->ht.di);
        }
        else
        {
            redisPanic("Unknown set encoding");
        }

        // ����������
    }
    else if (op->type == REDIS_ZSET)
    {
        iterzset *it = &op->iter.zset;
        // ziplist ����, O(N)
        if (op->encoding == REDIS_ENCODING_ZIPLIST)
        {
            /* No need to check both, but better be explicit. */
            if (it->zl.eptr == NULL || it->zl.sptr == NULL)
                return 0;
            // ȡ�� member
            redisAssert(ziplistGet(it->zl.eptr,&val->estr,&val->elen,&val->ell));
            // ȡ�� score
            val->score = zzlGetScore(it->zl.sptr);

            /* Move to next element. */
            zzlNext(it->zl.zl,&it->zl.eptr,&it->zl.sptr);
            // skiplist ����, O(N)
        }
        else if (op->encoding == REDIS_ENCODING_SKIPLIST)
        {
            if (it->sl.node == NULL)
                return 0;
            // ȡ�� member
            val->ele = it->sl.node->obj;
            // ȡ�� score
            val->score = it->sl.node->score;

            /* Move to next element. */
            it->sl.node = it->sl.node->level[0].forward;
        }
        else
        {
            redisPanic("Unknown sorted set encoding");
        }
    }
    else
    {
        redisPanic("Unsupported type");
    }


    return 1;
}

/*
 * �� val ��ȡ������������ֵ
 *
 * T = O(1)
 */
int zuiLongLongFromValue(zsetopval *val)
{
    if (!(val->flags & OPVAL_DIRTY_LL))
    {
        val->flags |= OPVAL_DIRTY_LL;

        // ����Ϊ����ʱʹ��...
        if (val->ele != NULL)
        {
            // �� intset ��ȡ��
            if (val->ele->encoding == REDIS_ENCODING_INT)
            {
                val->ell = (long)val->ele->ptr;
                val->flags |= OPVAL_VALID_LL;
                // �� sds ��ȡ��
            }
            else if (val->ele->encoding == REDIS_ENCODING_RAW)
            {
                if (string2ll(val->ele->ptr,sdslen(val->ele->ptr),&val->ell))
                    val->flags |= OPVAL_VALID_LL;
            }
            else
            {
                redisPanic("Unsupported element encoding");
            }

            // ����Ϊ����ʱʹ��
            // ���ַ�����ȡ��
        }
        else if (val->estr != NULL)
        {
            if (string2ll((char*)val->estr,val->elen,&val->ell))
                val->flags |= OPVAL_VALID_LL;
        }
        else
        {
            /* The long long was already set, flag as valid. */
            val->flags |= OPVAL_VALID_LL;
        }
    }
    return val->flags & OPVAL_VALID_LL;
}

/*
 * �� zsetopval ��Ľ��ȡ�������浽 robj �����Ȼ�󷵻�
 *
 * T = O(1)
 */
robj *zuiObjectFromValue(zsetopval *val)
{
    if (val->ele == NULL)
    {
        if (val->estr != NULL)
        {
            // member Ϊ����
            val->ele = createStringObject((char*)val->estr,val->elen);
        }
        else
        {
            // member Ϊ����ֵ
            val->ele = createStringObjectFromLongLong(val->ell);
        }
        val->flags |= OPVAL_DIRTY_ROBJ;
    }
    // member Ϊ����
    return val->ele;
}

int zuiBufferFromValue(zsetopval *val)
{
    if (val->estr == NULL)
    {
        if (val->ele != NULL)
        {
            if (val->ele->encoding == REDIS_ENCODING_INT)
            {
                val->elen = ll2string((char*)val->_buf,sizeof(val->_buf),(long)val->ele->ptr);
                val->estr = val->_buf;
            }
            else if (val->ele->encoding == REDIS_ENCODING_RAW)
            {
                val->elen = sdslen(val->ele->ptr);
                val->estr = val->ele->ptr;
            }
            else
            {
                redisPanic("Unsupported element encoding");
            }
        }
        else
        {
            val->elen = ll2string((char*)val->_buf,sizeof(val->_buf),val->ell);
            val->estr = val->_buf;
        }
    }
    return 1;
}

/* Find value pointed to by val in the source pointer to by op. When found,
 * return 1 and store its score in target. Return 0 otherwise. */
/*
 * �� op �в��� member �� score ֵ��
 *
 * �ҵ����� 1 �����򷵻� 0 ��
 *
 * T = O(N)
 */
int zuiFind(zsetopsrc *op, zsetopval *val, double *score)
{
    if (op->subject == NULL)
        return 0;

    if (op->type == REDIS_SET)
    {
        iterset *it = &op->iter.set;

        if (op->encoding == REDIS_ENCODING_INTSET)
        {
            // O(lg N)
            if (zuiLongLongFromValue(val) && intsetFind(it->is.is,val->ell))
            {
                *score = 1.0;
                return 1;
            }
            else
            {
                return 0;
            }
        }
        else if (op->encoding == REDIS_ENCODING_HT)
        {
            zuiObjectFromValue(val);
            // O(1)
            if (dictFind(it->ht.dict,val->ele) != NULL)
            {
                *score = 1.0;
                return 1;
            }
            else
            {
                return 0;
            }
        }
        else
        {
            redisPanic("Unknown set encoding");
        }
    }
    else if (op->type == REDIS_ZSET)
    {
        iterzset *it = &op->iter.zset;
        zuiObjectFromValue(val);

        if (op->encoding == REDIS_ENCODING_ZIPLIST)
        {
            // O(N)
            if (zzlFind(it->zl.zl,val->ele,score) != NULL)
            {
                /* Score is already set by zzlFind. */
                return 1;
            }
            else
            {
                return 0;
            }
        }
        else if (op->encoding == REDIS_ENCODING_SKIPLIST)
        {
            dictEntry *de;
            // O(1)
            if ((de = dictFind(it->sl.zs->dict,val->ele)) != NULL)
            {
                *score = *(double*)dictGetVal(de);
                return 1;
            }
            else
            {
                return 0;
            }
        }
        else
        {
            redisPanic("Unknown sorted set encoding");
        }
    }
    else
    {
        redisPanic("Unsupported type");
    }
}

int zuiCompareByCardinality(const void *s1, const void *s2)
{
    return zuiLength((zsetopsrc*)s1) - zuiLength((zsetopsrc*)s2);
}

#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define zunionInterDictValue(_e) (dictGetVal(_e) == NULL ? 1.0 : *(double*)dictGetVal(_e))

/*
 * ���� aggregate ������ָ����ģʽ���ۺ� *target �� val ����ֵ��
 */
inline static void zunionInterAggregate(double *target, double val, int aggregate)
{
    if (aggregate == REDIS_AGGR_SUM)
    {
        *target = *target + val;
        /* The result of adding two doubles is NaN when one variable
         * is +inf and the other is -inf. When these numbers are added,
         * we maintain the convention of the result being 0.0. */
        if (isnan(*target)) *target = 0.0;
    }
    else if (aggregate == REDIS_AGGR_MIN)
    {
        *target = val < *target ? val : *target;
    }
    else if (aggregate == REDIS_AGGR_MAX)
    {
        *target = val > *target ? val : *target;
    }
    else
    {
        /* safety net */
        redisPanic("Unknown ZUNION/INTER aggregate type");
    }
}

/*
 * ZUNIONSTORE �� ZINTERSTORE ��������ĵײ�ʵ��
 *
 * T = O(N^4)
 */
void zunionInterGenericCommand(redisClient *c, robj *dstkey, int op)
{
    int i, j;
    long setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    zsetopval zval;
    robj *tmp;
    unsigned int maxelelen = 0;
    robj *dstobj;
    zset *dstzset;
    zskiplistNode *znode;
    int touched = 0;

    /* expect setnum input keys to be given */
    // ȡ�� setnum ����
    if ((getLongFromObjectOrReply(c, c->argv[2], &setnum, NULL) != REDIS_OK))
        return;

    if (setnum < 1)
    {
        addReplyError(c,
                      "at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
        return;
    }

    /* test if the expected number of keys would overflow */
    if (setnum > c->argc-3)
    {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* read keys to be used for input */
    // �������� key , O(N)
    src = zcalloc(sizeof(zsetopsrc) * setnum);
    for (i = 0, j = 3; i < setnum; i++, j++)
    {
        // ȡ�� key
        robj *obj = lookupKeyWrite(c->db,c->argv[j]);
        if (obj != NULL)
        {
            // key ������ sorted set ���� set
            if (obj->type != REDIS_ZSET && obj->type != REDIS_SET)
            {
                zfree(src);
                addReply(c,shared.wrongtypeerr);
                return;
            }

            // ����
            src[i].subject = obj;
            src[i].type = obj->type;
            src[i].encoding = obj->encoding;
        }
        else
        {
            src[i].subject = NULL;
        }

        /* Default all weights to 1. */
        src[i].weight = 1.0;
    }

    /* parse optional extra arguments */
    // �������Ӳ���, O(N)
    if (j < c->argc)
    {
        int remaining = c->argc - j;

        // O(N)
        while (remaining)
        {
            // �������� weight ����, O(N)
            if (remaining >= (setnum + 1) && !strcasecmp(c->argv[j]->ptr,"weights"))
            {
                j++;
                remaining--;
                for (i = 0; i < setnum; i++, j++, remaining--)
                {
                    // �� weight ���浽 src ������
                    if (getDoubleFromObjectOrReply(c,c->argv[j],&src[i].weight,
                                                   "weight value is not a float") != REDIS_OK)
                    {
                        zfree(src);
                        return;
                    }
                }
                // ��ȡ���� aggregate ����, O(N)
            }
            else if (remaining >= 2 && !strcasecmp(c->argv[j]->ptr,"aggregate"))
            {
                j++;
                remaining--;
                if (!strcasecmp(c->argv[j]->ptr,"sum"))
                {
                    aggregate = REDIS_AGGR_SUM;
                }
                else if (!strcasecmp(c->argv[j]->ptr,"min"))
                {
                    aggregate = REDIS_AGGR_MIN;
                }
                else if (!strcasecmp(c->argv[j]->ptr,"max"))
                {
                    aggregate = REDIS_AGGR_MAX;
                }
                else
                {
                    zfree(src);
                    addReply(c,shared.syntaxerr);
                    return;
                }
                j++;
                remaining--;
            }
            else
            {
                zfree(src);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    // Ϊ���� key ����������
    for (i = 0; i < setnum; i++)
        zuiInitIterator(&src[i]);

    /* sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    // �����м��ϰ�������С�������У������㷨����
    qsort(src,setnum,sizeof(zsetopsrc),zuiCompareByCardinality);

    // ������϶���
    dstobj = createZsetObject();
    dstzset = dstobj->ptr;
    // ��ʼ�� zval ����
    memset(&zval, 0, sizeof(zval));

    // INTER ����, O(N^3)
    if (op == REDIS_OP_INTER)
    {
        /* Skip everything if the smallest input is empty. */
        // �����С����Ϊ�ռ�����ô����
        // ��С�Ż������������������һ���ռ�����ô����ؽ��ǿռ���
        if (zuiLength(&src[0]) > 0)
        {
            /* Precondition: as src[0] is non-empty and the inputs are ordered
             * by size, all src[i > 0] are non-empty too. */
            // ȡ����һ�����ϵ�Ԫ��
            // O(N^3)
            while (zuiNext(&src[0],&zval))
            {
                double score, value;

                // ���� weight �����µ� score ֵ
                score = src[0].weight * zval.score;
                if (isnan(score)) score = 0;

                // �����������뼯�ϣ����㽻��Ԫ�أ�����Ԫ�ص� score ֵ���оۺ�
                // O(N^2)
                for (j = 1; j < setnum; j++)
                {
                    /* It is not safe to access the zset we are
                     * iterating, so explicitly check for equal object. */
                    // ���ͬһ�����ϳ��������Σ���ô��ͬһ��Ԫ�ؼ����ۺ�һ��
                    if (src[j].subject == src[0].subject)
                    {
                        value = zval.score*src[j].weight;
                        // O(1)
                        zunionInterAggregate(&score,value,aggregate);
                        // ���Ҽ������Ƿ�����ͬԪ��
                        // ����оͽ��оۺ�, O(N)
                    }
                    else if (zuiFind(&src[j],&zval,&value))
                    {
                        value *= src[j].weight;
                        // O(1)
                        zunionInterAggregate(&score,value,aggregate);
                        // û�н���Ԫ�أ�����
                    }
                    else
                    {
                        break;
                    }
                }

                /* Only continue when present in every input. */
                // ���ǰ��Ľ�������û����������ôִ�����²���
                // O(N)
                if (j == setnum)
                {
                    // ȡ�� member
                    tmp = zuiObjectFromValue(&zval);
                    // ��ӵ� skiplist, O(N)
                    znode = zslInsert(dstzset->zsl,score,tmp);
                    incrRefCount(tmp); /* added to skiplist */
                    // ��ӵ��ֵ�, O(1)
                    dictAdd(dstzset->dict,tmp,&znode->score);
                    incrRefCount(tmp); /* added to dictionary */

                    if (tmp->encoding == REDIS_ENCODING_RAW)
                        if (sdslen(tmp->ptr) > maxelelen)
                            maxelelen = sdslen(tmp->ptr);
                }
            }
        }

        // ZUNIONSTORE ������ O(N^4)
    }
    else if (op == REDIS_OP_UNION)
    {
        // �������м���
        // O(N^4)
        for (i = 0; i < setnum; i++)
        {
            if (zuiLength(&src[i]) == 0)
                continue;

            // ȡ�������е�����Ԫ��
            // O(N^3)
            while (zuiNext(&src[i],&zval))
            {
                double score, value;

                /* Skip key when already processed */
                // �����Ԫ���Ѿ������ڽ��������ô����ѭ��
                if (dictFind(dstzset->dict,zuiObjectFromValue(&zval)) != NULL)
                    continue;

                /* Initialize score */
                // ���� weight ���� score
                score = src[i].weight * zval.score;
                if (isnan(score)) score = 0;

                /* Because the inputs are sorted by size, it's only possible
                 * for sets at larger indices to hold this element. */
                // ������ǰ����֮������м��ϣ��� member ���оۺ�
                // O(N^2)
                for (j = (i+1); j < setnum; j++)
                {
                    /* It is not safe to access the zset we are
                     * iterating, so explicitly check for equal object. */
                    // ����������ͬ, O(1)
                    if(src[j].subject == src[i].subject)
                    {
                        value = zval.score*src[j].weight;
                        zunionInterAggregate(&score,value,aggregate);
                        // ����Ԫ��, O(N)
                    }
                    else if (zuiFind(&src[j],&zval,&value))
                    {
                        value *= src[j].weight;
                        zunionInterAggregate(&score,value,aggregate);
                    }
                }

                // ��������浽 zset ������
                tmp = zuiObjectFromValue(&zval);
                znode = zslInsert(dstzset->zsl,score,tmp);
                incrRefCount(zval.ele); /* added to skiplist */
                dictAdd(dstzset->dict,tmp,&znode->score);
                incrRefCount(zval.ele); /* added to dictionary */

                if (tmp->encoding == REDIS_ENCODING_RAW)
                    if (sdslen(tmp->ptr) > maxelelen)
                        maxelelen = sdslen(tmp->ptr);
            }
        }
    }
    else
    {
        redisPanic("Unknown operator");
    }

    // ������е�����
    for (i = 0; i < setnum; i++)
        zuiClearIterator(&src[i]);

    // ɾ���ɵ� dstkey
    if (dbDelete(c->db,dstkey))
    {
        signalModifiedKey(c->db,dstkey);
        touched = 1;
        server.dirty++;
    }

    // ����ۺϽ���� dstkey
    if (dstzset->zsl->length)
    {
        /* Convert to ziplist when in limits. */
        if (dstzset->zsl->length <= server.zset_max_ziplist_entries &&
                maxelelen <= server.zset_max_ziplist_value)
            zsetConvert(dstobj,REDIS_ENCODING_ZIPLIST);

        dbAdd(c->db,dstkey,dstobj);
        addReplyLongLong(c,zsetLength(dstobj));
        if (!touched) signalModifiedKey(c->db,dstkey);
        server.dirty++;
    }
    else
    {
        decrRefCount(dstobj);
        addReply(c,shared.czero);
    }
    zfree(src);
}

void zunionstoreCommand(redisClient *c)
{
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_UNION);
}

void zinterstoreCommand(redisClient *c)
{
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_INTER);
}

/*
 * T = O(N)
 */
void zrangeGenericCommand(redisClient *c, int reverse)
{
    robj *key = c->argv[1];
    robj *zobj;
    int withscores = 0;
    long start;
    long end;
    int llen;
    int rangelen;

    // ȡ�� start �� end ����
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
            (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    // ȡ�� withscores ����
    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores"))
    {
        withscores = 1;
    }
    else if (c->argc >= 5)
    {
        addReply(c,shared.syntaxerr);
        return;
    }

    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL
            || checkType(c,zobj,REDIS_ZSET)) return;

    /* Sanitize indexes. */
    llen = zsetLength(zobj);
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
    addReplyMultiBulkLen(c, withscores ? (rangelen*2) : rangelen);

    // ziplist ����, O(N)
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        // ���������ķ���
        // ��ָ���һ�� member
        // O(1)
        if (reverse)
            eptr = ziplistIndex(zl,-2-(2*start));
        else
            eptr = ziplistIndex(zl,2*start);

        redisAssertWithInfo(c,zobj,eptr != NULL);
        // ָ���һ�� score
        sptr = ziplistNext(zl,eptr);

        // ȡ��Ԫ��, O(N)
        while (rangelen--)
        {
            // Ԫ�ز�Ϊ�գ�
            redisAssertWithInfo(c,zobj,eptr != NULL && sptr != NULL);
            // ȡ�� member
            redisAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));
            if (vstr == NULL)
                addReplyBulkLongLong(c,vlong);
            else
                addReplyBulkCBuffer(c,vstr,vlen);

            // ȡ�� score
            if (withscores)
                addReplyDouble(c,zzlGetScore(sptr));

            // �ƶ�ָ�뵽��һ���ڵ�
            if (reverse)
                zzlPrev(zl,&eptr,&sptr);
            else
                zzlNext(zl,&eptr,&sptr);
        }

    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;
        robj *ele;

        /* Check if starting point is trivial, before doing log(N) lookup. */
        // ������ʼ�ڵ�, O(N)
        if (reverse)
        {
            ln = zsl->tail;
            if (start > 0)
                // O(N)
                ln = zslGetElementByRank(zsl,llen-start);
        }
        else
        {
            ln = zsl->header->level[0].forward;
            if (start > 0)
                // O(N)
                ln = zslGetElementByRank(zsl,start+1);
        }

        // O(N)
        while(rangelen--)
        {
            redisAssertWithInfo(c,zobj,ln != NULL);
            // ���� member
            ele = ln->obj;
            addReplyBulk(c,ele);
            // ���� score
            if (withscores)
                addReplyDouble(c,ln->score);
            // O(1)
            ln = reverse ? ln->backward : ln->level[0].forward;
        }
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }
}

void zrangeCommand(redisClient *c)
{
    zrangeGenericCommand(c,0);
}

void zrevrangeCommand(redisClient *c)
{
    zrangeGenericCommand(c,1);
}

/* This command implements ZRANGEBYSCORE, ZREVRANGEBYSCORE. */
/*
 * T = O(N)
 */
void genericZrangebyscoreCommand(redisClient *c, int reverse)
{
    zrangespec range;
    robj *key = c->argv[1];
    robj *zobj;
    long offset = 0, limit = -1;
    int withscores = 0;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    int minidx, maxidx;

    /* Parse the range arguments. */
    if (reverse)
    {
        /* Range is given as [max,min] */
        maxidx = 2;
        minidx = 3;
    }
    else
    {
        /* Range is given as [min,max] */
        minidx = 2;
        maxidx = 3;
    }

    if (zslParseRange(c->argv[minidx],c->argv[maxidx],&range) != REDIS_OK)
    {
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. */
    // O(1)
    if (c->argc > 4)
    {
        int remaining = c->argc - 4;
        int pos = 4;

        // �������, O(N)
        while (remaining)
        {
            // withscores ����
            if (remaining >= 1 && !strcasecmp(c->argv[pos]->ptr,"withscores"))
            {
                pos++;
                remaining--;
                withscores = 1;
                // limit ����
            }
            else if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit"))
            {
                // offset �� limit ����
                if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) != REDIS_OK) ||
                        (getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) != REDIS_OK)) return;
                pos += 3;
                remaining -= 3;
            }
            else
            {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range */
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL ||
            checkType(c,zobj,REDIS_ZSET)) return;

    // O(N)
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        double score;

        /* If reversed, get the last node in range as starting point. */
        // O(N)
        if (reverse)
        {
            eptr = zzlLastInRange(zl,range);
        }
        else
        {
            eptr = zzlFirstInRange(zl,range);
        }

        /* No "first" element in the specified interval. */
        if (eptr == NULL)
        {
            addReply(c, shared.emptymultibulk);
            return;
        }

        /* Get score pointer for the first element. */
        redisAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        // O(N)
        while (eptr && offset--)
        {
            if (reverse)
            {
                zzlPrev(zl,&eptr,&sptr);
            }
            else
            {
                zzlNext(zl,&eptr,&sptr);
            }
        }

        // O(N)
        while (eptr && limit--)
        {
            score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            // ������Χ������
            if (reverse)
            {
                if (!zslValueGteMin(score,&range)) break;
            }
            else
            {
                if (!zslValueLteMax(score,&range)) break;
            }

            /* We know the element exists, so ziplistGet should always succeed */
            // ȡ�� member
            redisAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));

            rangelen++;
            if (vstr == NULL)
            {
                addReplyBulkLongLong(c,vlong);
            }
            else
            {
                addReplyBulkCBuffer(c,vstr,vlen);
            }

            // ȡ�� score
            if (withscores)
            {
                addReplyDouble(c,score);
            }

            /* Move to next node */
            // �ƶ�����һ�ڵ�
            if (reverse)
            {
                zzlPrev(zl,&eptr,&sptr);
            }
            else
            {
                zzlNext(zl,&eptr,&sptr);
            }
        }
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* If reversed, get the last node in range as starting point. */
        // O(N)
        if (reverse)
        {
            ln = zslLastInRange(zsl,range);
        }
        else
        {
            ln = zslFirstInRange(zsl,range);
        }

        /* No "first" element in the specified interval. */
        if (ln == NULL)
        {
            addReply(c, shared.emptymultibulk);
            return;
        }

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        // O(N)
        while (ln && offset--)
        {
            if (reverse)
            {
                ln = ln->backward;
            }
            else
            {
                ln = ln->level[0].forward;
            }
        }

        // O(N)
        while (ln && limit--)
        {
            /* Abort when the node is no longer in range. */
            // ������Χ�� ����
            if (reverse)
            {
                if (!zslValueGteMin(ln->score,&range)) break;
            }
            else
            {
                if (!zslValueLteMax(ln->score,&range)) break;
            }

            rangelen++;
            // ȡ�� member
            addReplyBulk(c,ln->obj);

            // ȡ�� score
            if (withscores)
            {
                addReplyDouble(c,ln->score);
            }

            /* Move to next node */
            // �ƶ�����һ�ڵ�
            if (reverse)
            {
                ln = ln->backward;
            }
            else
            {
                ln = ln->level[0].forward;
            }
        }
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }

    if (withscores)
    {
        rangelen *= 2;
    }

    setDeferredMultiBulkLength(c, replylen, rangelen);
}

void zrangebyscoreCommand(redisClient *c)
{
    genericZrangebyscoreCommand(c,0);
}

void zrevrangebyscoreCommand(redisClient *c)
{
    genericZrangebyscoreCommand(c,1);
}

/*
 * ���������ڸ�����Χ�ڵ�Ԫ��
 *
 * T = O(N)
 */
void zcountCommand(redisClient *c)
{
    robj *key = c->argv[1];
    robj *zobj;
    zrangespec range;
    int count = 0;

    /* Parse the range arguments */
    // range ����
    if (zslParseRange(c->argv[2],c->argv[3],&range) != REDIS_OK)
    {
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Lookup the sorted set */
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL ||
            checkType(c, zobj, REDIS_ZSET)) return;

    // O(N)
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        double score;

        /* Use the first element in range as the starting point */
        // ȡ����һ�����Ϸ�Χ�� member
        eptr = zzlFirstInRange(zl,range);

        /* No "first" element */
        // û�з��Ϸ�Χ�Ľڵ�
        if (eptr == NULL)
        {
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range */
        // ָ�� score ��
        sptr = ziplistNext(zl,eptr);
        // ȡ�� score ֵ
        score = zzlGetScore(sptr);
        redisAssertWithInfo(c,zobj,zslValueLteMax(score,&range));

        /* Iterate over elements in range */
        // O(N)
        while (eptr)
        {
            score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            if (!zslValueLteMax(score,&range))
            {
                break;
            }
            else
            {
                count++;
                zzlNext(zl,&eptr,&sptr);
            }
        }
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range */
        // O(N)
        zn = zslFirstInRange(zsl, range);

        /* Use rank of first element, if any, to determine preliminary count */
        if (zn != NULL)
        {
            // O(N)
            rank = zslGetRank(zsl, zn->score, zn->obj);
            count = (zsl->length - (rank - 1));

            /* Find last element in range */
            zn = zslLastInRange(zsl, range);

            /* Use rank of last element, if any, to determine the actual count */
            if (zn != NULL)
            {
                rank = zslGetRank(zsl, zn->score, zn->obj);
                // �ҳ���һ�������һ�����Ϸ�Χ�Ľڵ�
                // �����ǵ� rank ������Ƿ�Χ�ڵĽڵ������
                count -= (zsl->length - rank);
            }
        }
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }

    addReplyLongLong(c, count);
}

/*
 * �������򼯵Ļ���
 *
 * T = O(N)
 */
void zcardCommand(redisClient *c)
{
    robj *key = c->argv[1];
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.czero)) == NULL ||
            checkType(c,zobj,REDIS_ZSET)) return;

    addReplyLongLong(c,zsetLength(zobj));
}

/*
 * �ҳ�����Ԫ�ص� score ֵ
 *
 * T = O(N)
 */
void zscoreCommand(redisClient *c)
{
    robj *key = c->argv[1];
    robj *zobj;
    double score;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.nullbulk)) == NULL ||
            checkType(c,zobj,REDIS_ZSET)) return;

    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        // O(N)
        if (zzlFind(zobj->ptr,c->argv[2],&score) != NULL)
            addReplyDouble(c,score);
        else
            addReply(c,shared.nullbulk);
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        dictEntry *de;

        // O(1)
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        de = dictFind(zs->dict,c->argv[2]);
        if (de != NULL)
        {
            score = *(double*)dictGetVal(de);
            addReplyDouble(c,score);
        }
        else
        {
            addReply(c,shared.nullbulk);
        }
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }
}

/*
 * ���ظ���Ԫ���������е���λ
 *
 * T = O(N)
 */
void zrankGenericCommand(redisClient *c, int reverse)
{
    robj *key = c->argv[1];
    robj *ele = c->argv[2];
    robj *zobj;
    unsigned long llen;
    unsigned long rank;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.nullbulk)) == NULL ||
            checkType(c,zobj,REDIS_ZSET)) return;
    llen = zsetLength(zobj);

    redisAssertWithInfo(c,ele,ele->encoding == REDIS_ENCODING_RAW);
    if (zobj->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = ziplistIndex(zl,0);
        redisAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        redisAssertWithInfo(c,zobj,sptr != NULL);

        // ����ָ�룬һ·����Խ���Ľڵ�����
        rank = 1;
        while(eptr != NULL)
        {
            if (ziplistCompare(eptr,ele->ptr,sdslen(ele->ptr)))
                break;
            rank++;
            zzlNext(zl,&eptr,&sptr);
        }

        if (eptr != NULL)
        {
            if (reverse)
                addReplyLongLong(c,llen-rank);
            else
                addReplyLongLong(c,rank-1);
        }
        else
        {
            addReply(c,shared.nullbulk);
        }
    }
    else if (zobj->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        dictEntry *de;
        double score;

        // O(1)
        ele = c->argv[2] = tryObjectEncoding(c->argv[2]);
        de = dictFind(zs->dict,ele);
        if (de != NULL)
        {
            score = *(double*)dictGetVal(de);
            // ����Ԫ������Ծ���е�λ�� O(N)
            rank = zslGetRank(zsl,score,ele);
            redisAssertWithInfo(c,ele,rank); /* Existing elements always have a rank. */
            if (reverse)
                addReplyLongLong(c,llen-rank);
            else
                addReplyLongLong(c,rank-1);
        }
        else
        {
            addReply(c,shared.nullbulk);
        }
    }
    else
    {
        redisPanic("Unknown sorted set encoding");
    }
}

void zrankCommand(redisClient *c)
{
    zrankGenericCommand(c, 0);
}

void zrevrankCommand(redisClient *c)
{
    zrankGenericCommand(c, 1);
}
