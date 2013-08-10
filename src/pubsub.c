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
 * Pubsub low level API
 *----------------------------------------------------------------------------*/

/*
 * �ͷ�ָ����ģʽ
 *
 * T = O(1)
 */
void freePubsubPattern(void *p)
{
    pubsubPattern *pat = p;

    decrRefCount(pat->pattern);
    zfree(pat);
}

/*
 * �Ա�����ģʽ�Ƿ���ͬ
 *
 * T = O(1)
 */
int listMatchPubsubPattern(void *a, void *b)
{
    pubsubPattern *pa = a, *pb = b;

    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
/*
 * Ϊ�ͻ��˶���ָ����Ƶ��
 *
 * ���ĳɹ����� 1 �����Ƶ���Ѿ����ģ����� 0 ��
 *
 * T = O(1)
 */
int pubsubSubscribeChannel(redisClient *c, robj *channel)
{
    struct dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    /* Add the channel to the client -> channels hash table */
    // �� channel ��ӵ��ͻ��˵� pubsub_channels �ֵ���
    // ��Ϊ channel ��ֵΪ NULL
    // O(1)
    if (dictAdd(c->pubsub_channels,channel,NULL) == DICT_OK)
    {
        retval = 1;
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        // ���ͻ�����ӵ�Ƶ���Ķ���������
        // ���� server.pubsub_channels Ϊ�ֵ�
        // �ֵ�ļ�Ϊ channel ��ֵΪ���������ﱣ���˶��ĸ�Ƶ�������пͻ���
        // O(1)
        de = dictFind(server.pubsub_channels,channel);
        if (de == NULL)
        {
            clients = listCreate();
            // O(1)
            dictAdd(server.pubsub_channels,channel,clients);
            incrRefCount(channel);
        }
        else
        {
            clients = dictGetVal(de);
        }
        // O(1)
        listAddNodeTail(clients,c);
    }

    /* Notify the client */
    // ��ͻ��˷���ֵ����֪�����ѳɹ�
    addReply(c,shared.mbulkhdr[3]);
    addReply(c,shared.subscribebulk);
    addReplyBulk(c,channel);
    addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));

    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
/*
 * ȡ���ͻ��˶� channel �Ķ���
 *
 * �˶��ɹ����� 1 ���ͻ���δ���� channel ����ɵ��˶�ʧ�ܷ��� 0 ��
 *
 * T = O(N)
 */
int pubsubUnsubscribeChannel(redisClient *c, robj *channel, int notify)
{
    struct dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    // ɾ���ͻ����е�Ƶ����Ϣ, O(1)
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK)
    {
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        // ɾ���������пͻ��˶���Ƶ������Ϣ, O(1)
        de = dictFind(server.pubsub_channels,channel);
        redisAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);
        ln = listSearchKey(clients,c); // O(N)
        redisAssertWithInfo(c,NULL,ln != NULL);
        listDelNode(clients,ln); // O(1)

        // ��������Ѿ�����գ���ôɾ����
        if (listLength(clients) == 0)
        {
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            // O(1)
            dictDelete(server.pubsub_channels,channel);
        }
    }

    /* Notify the client */
    // �ظ��ͻ���
    if (notify)
    {
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.unsubscribebulk);
        addReplyBulk(c,channel);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                         listLength(c->pubsub_patterns));

    }

    decrRefCount(channel); /* it is finally safe to release it */

    return retval;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the clinet was already subscribed to that pattern. */
/*
 * ����һ��ģʽ
 *
 * ���ĳɹ����� 1 ������Ѿ����ķ��� 0 ��
 *
 * T = O(N)
 */
int pubsubSubscribePattern(redisClient *c, robj *pattern)
{
    int retval = 0;

    // ��� pattern δ������ c->pubsub_patterns ������ô������
    // O(N)
    if (listSearchKey(c->pubsub_patterns,pattern) == NULL)
    {
        retval = 1;
        pubsubPattern *pat;

        // ��ģʽ����ͻ�������, O(1)
        listAddNodeTail(c->pubsub_patterns,pattern);

        incrRefCount(pattern);

        // ��ģʽ�Ϳͻ�����Ϣ��¼��������
        // O(1)
        pat = zmalloc(sizeof(*pat));
        pat->pattern = getDecodedObject(pattern);
        pat->client = c;
        listAddNodeTail(server.pubsub_patterns,pat);
    }

    /* Notify the client */
    addReply(c,shared.mbulkhdr[3]);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));

    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
/*
 * �ÿͻ����˶�������ģʽ
 *
 * �˶��ɹ����� 1 �������Ϊ�ͻ��˲�û�ж���ģʽ������˶�ʧ�ܣ����� 0
 *
 * T = O(N)
 */
int pubsubUnsubscribePattern(redisClient *c, robj *pattern, int notify)
{
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    // �ͻ��˶�������� pattern �� , O(N)
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL)
    {
        retval = 1;
        // �ӿͻ������Ƴ� pattern , O(1)
        listDelNode(c->pubsub_patterns,ln);
        pat.client = c;
        pat.pattern = pattern;
        // �ӷ��������Ƴ���� pattern ,O(N)
        ln = listSearchKey(server.pubsub_patterns,&pat);
        // O(1)
        listDelNode(server.pubsub_patterns,ln);
    }
    /* Notify the client */
    if (notify)
    {
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.punsubscribebulk);
        addReplyBulk(c,pattern);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                         listLength(c->pubsub_patterns));
    }
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed from. */
/*
 * �ÿͻ����˶�����Ƶ��
 *
 * T = O(N^2)
 */
int pubsubUnsubscribeAllChannels(redisClient *c, int notify)
{
    dictIterator *di = dictGetSafeIterator(c->pubsub_channels);
    dictEntry *de;
    int count = 0;

    while((de = dictNext(di)) != NULL)
    {
        robj *channel = dictGetKey(de);

        // O(N)
        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    dictReleaseIterator(di);
    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
/*
 * �ÿͻ����˶�����ģʽ
 *
 * T = O(N^2)
 */
int pubsubUnsubscribeAllPatterns(redisClient *c, int notify)
{
    listNode *ln;
    listIter li;
    int count = 0;

    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL)
    {
        robj *pattern = ln->value;

        // O(N)
        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    return count;
}

/* Publish a message */
int pubsubPublishMessage(robj *channel, robj *message)
{
    int receivers = 0;
    struct dictEntry *de;
    listNode *ln;
    listIter li;

    /* Send to clients listening for that channel */
    // ȡ�����ж��ĸ���Ƶ���Ŀͻ���, O(1)
    de = dictFind(server.pubsub_channels,channel);
    if (de)
    {
        list *list = dictGetVal(de);
        listNode *ln;
        listIter li;

        // ����Ϣ�����������ж�����, O(N)
        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL)
        {
            redisClient *c = ln->value;

            addReply(c,shared.mbulkhdr[3]); // ��Ϣͷ
            addReply(c,shared.messagebulk); // ��Ϣ����
            addReplyBulk(c,channel);        // ��ԴƵ��
            addReplyBulk(c,message);        // ��Ϣ����

            receivers++;
        }
    }

    /* Send to clients listening to matching channels */
    // ƥ���������Ϊ 0
    if (listLength(server.pubsub_patterns))
    {
        // ��������ģʽ��������Ϣ���͸�ƥ���ģʽ
        listRewind(server.pubsub_patterns,&li);
        channel = getDecodedObject(channel);
        while ((ln = listNext(&li)) != NULL)
        {
            pubsubPattern *pat = ln->value;

            if (stringmatchlen((char*)pat->pattern->ptr,
                               sdslen(pat->pattern->ptr),
                               (char*)channel->ptr,
                               sdslen(channel->ptr),0))
            {

                addReply(pat->client,shared.mbulkhdr[4]);   // ��Ϣͷ
                addReply(pat->client,shared.pmessagebulk);  // ��Ϣ����
                addReplyBulk(pat->client,pat->pattern);     // ƥ���ģʽ
                addReplyBulk(pat->client,channel);          // ��ƥ���Ƶ��
                addReplyBulk(pat->client,message);          // ��Ϣ����

                receivers++;
            }
        }
        decrRefCount(channel);
    }

    return receivers;
}

/*-----------------------------------------------------------------------------
 * Pubsub commands implementation
 *----------------------------------------------------------------------------*/

/*
 * ����Ƶ��
 */
void subscribeCommand(redisClient *c)
{
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
}

void unsubscribeCommand(redisClient *c)
{
    if (c->argc == 1)
    {
        pubsubUnsubscribeAllChannels(c,1);
        return;
    }
    else
    {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
}

void psubscribeCommand(redisClient *c)
{
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
}

void punsubscribeCommand(redisClient *c)
{
    if (c->argc == 1)
    {
        pubsubUnsubscribeAllPatterns(c,1);
        return;
    }
    else
    {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
}

void publishCommand(redisClient *c)
{
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    if (server.cluster_enabled) clusterPropagatePublish(c->argv[1],c->argv[2]);
    addReplyLongLong(c,receivers);
}
