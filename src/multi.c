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

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
/*
 * ��ʼ���ͻ��˵�����״̬
 *
 * T = O(1)
 */
void initClientMultiState(redisClient *c)
{
    c->mstate.commands = NULL;
    c->mstate.count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
/*
 * �ͷ���������������е�����
 *
 * T = O(N^2)
 */
void freeClientMultiState(redisClient *c)
{
    int j;

    // �ͷ���������
    for (j = 0; j < c->mstate.count; j++)
    {
        int i;
        multiCmd *mc = c->mstate.commands+j;

        // �ͷ���������в���
        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
/*
 * ����������ӵ����������
 *
 * T = O(N)
 */
void queueMultiCommand(redisClient *c)
{
    multiCmd *mc;
    int j;

    // �ط���ռ䣬Ϊ����������ռ�
    c->mstate.commands = zrealloc(c->mstate.commands,
                                  sizeof(multiCmd)*(c->mstate.count+1));

    // ָ��ָ���·���Ŀռ�
    // �����������ݱ����ȥ
    mc = c->mstate.commands+c->mstate.count;
    mc->cmd = c->cmd;   // ����Ҫִ�е�����
    mc->argc = c->argc; // �����������������
    mc->argv = zmalloc(sizeof(robj*)*c->argc);  // Ϊ��������ռ�
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc); // ���Ʋ���
    for (j = 0; j < c->argc; j++)   // Ϊ���������ü�����һ
        incrRefCount(mc->argv[j]);

    // �������������һ
    c->mstate.count++;
}

/*
 * ���������������ÿͻ��˵�����״̬
 *
 * T = O(N^2)
 */
void discardTransaction(redisClient *c)
{
    // �ͷŲ����ռ�
    freeClientMultiState(c);
    // ��������״̬
    initClientMultiState(c);
    // �ر���ص� flag
    c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS|REDIS_DIRTY_EXEC);;
    // ȡ������ key �ļ���, O(N^2)
    unwatchAllKeys(c);
}

/* Flag the transacation as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
/*
 * �������ӵĹ����з����������
 * ��ô�ÿͻ��˱�Ϊ���ࡱ�����´�����ִ��ʧ��
 *
 * T = O(1)
 */
void flagTransaction(redisClient *c)
{
    if (c->flags & REDIS_MULTI)
        c->flags |= REDIS_DIRTY_EXEC;
}

/*
 * MULTI �����ʵ��
 *
 * �򿪿ͻ��˵� FLAG ����������ӵ����������
 *
 * T = O(1)
 */
void multiCommand(redisClient *c)
{
    // MULTI �����Ƕ��
    if (c->flags & REDIS_MULTI)
    {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }

    // ������� FLAG
    // �Ӵ�֮�󣬳� DISCARD �� EXEC ��������������֮��
    // �������е�����ᱻ��ӵ����������
    c->flags |= REDIS_MULTI;
    addReply(c,shared.ok);
}

/*
 * DISCAD �����ʵ��
 *
 * �������񣬲����������Դ
 *
 * T = O(N)
 */
void discardCommand(redisClient *c)
{
    // ֻ���� MULTI ���������õ������ʹ��
    if (!(c->flags & REDIS_MULTI))
    {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    // ��������, O(N)
    discardTransaction(c);
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implememntation for more information. */
/*
 * �����и����ڵ�� AOF �ļ����� MULTI ����
 */
void execCommandReplicateMulti(redisClient *c)
{
    robj *multistring = createStringObject("MULTI",5);

    if (server.aof_state != REDIS_AOF_OFF)
        feedAppendOnlyFile(server.multiCommand,c->db->id,&multistring,1);
    if (listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,&multistring,1);
    decrRefCount(multistring);
}

/*
 * EXEC �����ʵ��
 */
void execCommand(redisClient *c)
{
    int j;
    // ���ڱ���ִ���������Ĳ����Ͳ��������ĸ���
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;

    // ֻ���� MULTI �����õ������ִ��
    if (!(c->flags & REDIS_MULTI))
    {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* Check if we need to abort the EXEC because:
     * �����������ʱ��ȡ������
     *
     * 1) Some WATCHed key was touched.
     *    ĳЩ�����ӵļ��ѱ��޸ģ�״̬Ϊ REDIS_DIRTY_CAS��
     *
     * 2) There was a previous error while queueing commands.
     *    �����������ʱ��������״̬Ϊ REDIS_DIRTY_EXEC��
     *
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned.
     *
     * ��һ��������ض���հ� NULL ����
     * �ڶ����������һ�� EXECABORT ����
     */
    if (c->flags & (REDIS_DIRTY_CAS|REDIS_DIRTY_EXEC))
    {
        // ����״̬���������صĴ��������
        addReply(c, c->flags & REDIS_DIRTY_EXEC ? shared.execaborterr :
                 shared.nullmultibulk);

        // �����ľ������ discardTransaction() ���滻
        freeClientMultiState(c);
        initClientMultiState(c);
        c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS|REDIS_DIRTY_EXEC);
        unwatchAllKeys(c);

        goto handle_monitor;
    }

    /* Replicate a MULTI request now that we are sure the block is executed.
     * This way we'll deliver the MULTI/..../EXEC block as a whole and
     * both the AOF and the replication link will have the same consistency
     * and atomicity guarantees. */
    // �����и����ڵ�� AOF �ļ����� MULTI ����
    execCommandReplicateMulti(c);

    /* Exec all the queued commands */
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */

    // ������ԭʼ������������
    orig_argv = c->argv;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    addReplyMultiBulkLen(c,c->mstate.count);
    // ִ��������ӵ�����
    for (j = 0; j < c->mstate.count; j++)
    {
        // ��Ϊ call �����޸������������Ҫ���͸�����ͬ���ڵ�
        // �������ｫҪִ�е��������������ȱ�������
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;

        // ִ������
        call(c,REDIS_CALL_FULL);

        /* Commands may alter argc/argv, restore mstate. */
        // ��ԭԭʼ�Ĳ�����������
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].cmd = c->cmd;
    }
    // ��ԭ����ԭʼ����
    c->argv = orig_argv;
    c->argc = orig_argc;
    c->cmd = orig_cmd;

    // ��������Ҳ������ discardTransaction() ���滻
    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS|REDIS_DIRTY_EXEC);
    /* Make sure the EXEC command is always replicated / AOF, since we
     * always send the MULTI command (we can't know beforehand if the
     * next operations will contain at least a modification to the DB). */
    server.dirty++;

handle_monitor:
    /* Send EXEC to clients waiting data from MONITOR. We do it here
     * since the natural order of commands execution is actually:
     * MUTLI, EXEC, ... commands inside transaction ...
     * Instead EXEC is flagged as REDIS_CMD_SKIP_MONITOR in the command
     * table, and we do it here with correct ordering. */
    // ��ͬ���ڵ㷢������
    if (listLength(server.monitors) && !server.loading)
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * ʵ��Ϊÿ�� DB ׼��һ���ֵ䣨��ϣ�����ֵ�ļ�Ϊ�����ݿⱻ WATCHED �� key
 * ���ֵ��ֵ��һ���������������м������ key �Ŀͻ���
 * һ��ĳ�� key ���޸ģ�����Ὣ������������пͻ��˶�����Ϊ����Ⱦ
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called.
 *
 * ���⣬�ͻ��˻�ά����һ���������� WATCH key ������
 * �����Ϳ���������ִ�л��� UNWATCH ����ʱ��һ��������� WATCH key ��
 */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
/*
 * �����ӵ� key ������
 */
typedef struct watchedKey
{
    // �����ӵ� key
    robj *key;
    // key ���ڵ����ݿ�
    redisDb *db;
} watchedKey;

/* Watch for the specified key */
/*
 * ���Ӹ��� key
 *
 * T = O(N)
 */
void watchForKey(redisClient *c, robj *key)
{
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    // ���� key �Ƿ��Ѿ��� WATCH
    // �������� WATCH �������ʱһ�� key �������ε������
    // ����ǵĻ���ֱ�ӷ���
    // O(N)
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li)))
    {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }

    // key δ������
    // ���� key �����ͻ��˼��뵽 DB �ļ��� key �ֵ���
    /* This key is not already watched in this DB. Let's add it */
    // O(1)
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients)
    {
        clients = listCreate();
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    listAddNodeTail(clients,c);

    // �� key ��ӵ��ͻ��˵ļ����б���
    /* Add the new key to the lits of keys watched by this client */
    // O(1)
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
/*
 * ȡ�����иÿͻ��˼��ӵ� key
 * ������״̬������ɵ�����ִ��
 *
 * T = O(N^2)
 */
void unwatchAllKeys(redisClient *c)
{
    listIter li;
    listNode *ln;

    // û�м��� watch ��ֱ�ӷ���
    if (listLength(c->watched_keys) == 0) return;

    // �ӿͻ����Լ� DB ��ɾ�����м��� key �Ϳͻ��˵�����
    // O(N^2)
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li)))
    {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        // ȡ�� watchedKey �ṹ
        wk = listNodeValue(ln);
        // ɾ�� db �еĿͻ�����Ϣ, O(1)
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        redisAssertWithInfo(c,NULL,clients != NULL);
        // O(N)
        listDelNode(clients,listSearchKey(clients,c));

        /* Kill the entry at all if this was the only client */
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */

        // �� key �ӿͻ��˵ļ����б���ɾ��, O(1)
        listDelNode(c->watched_keys,ln);

        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
/*
 * ����������touch������ key �������� key ���ڱ����ӵĻ���
 * �ü������Ŀͻ�����ִ�� EXEC ����ʱʧ�ܡ�
 *
 * T = O(N)
 */
void touchWatchedKey(redisDb *db, robj *key)
{
    list *clients;
    listIter li;
    listNode *ln;

    // ������ݿ���û���κ� key �����ӣ���ôֱ�ӷ���
    if (dictSize(db->watched_keys) == 0) return;

    // ȡ�����ݿ������м��Ӹ��� key �Ŀͻ���
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;

    /* Mark all the clients watching this key as REDIS_DIRTY_CAS */
    /* Check if we are already watching for this key */
    // �����м������ key �Ŀͻ��˵� REDIS_DIRTY_CAS ״̬
    // O(N)
    listRewind(clients,&li);
    while((ln = listNext(&li)))
    {
        redisClient *c = listNodeValue(ln);

        c->flags |= REDIS_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */
/*
 * Ϊ FLUSHDB �� FLUSHALL �ر����õĴ�������
 *
 * T = O(N^2)
 */
void touchWatchedKeysOnFlush(int dbid)
{
    listIter li1, li2;
    listNode *ln;

    /* For every client, check all the waited keys */
    // �г����пͻ��ˣ�O(N)
    listRewind(server.clients,&li1);
    while((ln = listNext(&li1)))
    {
        redisClient *c = listNodeValue(ln);
        // �г����м��� key ,O(N)
        listRewind(c->watched_keys,&li2);
        while((ln = listNext(&li2)))
        {
            watchedKey *wk = listNodeValue(ln);

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            // ���Ŀ�� db �ͼ��� key �� DB ��ͬ��
            // ��ô�򿪿ͻ��˵� REDIS_DIRTY_CAS ѡ��
            // O(1)
            if (dbid == -1 || wk->db->id == dbid)
            {
                if (dictFind(wk->db->dict, wk->key->ptr) != NULL)
                    c->flags |= REDIS_DIRTY_CAS;
            }
        }
    }
}

/*
 * �������������ӵ������б���
 *
 * T = O(N^2)
 */
void watchCommand(redisClient *c)
{
    int j;

    // ֻ����������ʹ��
    if (c->flags & REDIS_MULTI)
    {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }

    // �������� key ��O(N^2)
    for (j = 1; j < c->argc; j++)
        // O(N)
        watchForKey(c,c->argv[j]);

    addReply(c,shared.ok);
}

/*
 * ȡ�������� key �ļ���
 * ���رտͻ��˵� REDIS_DIRTY_CAS ѡ��
 *
 * T = O(N^2)
 */
void unwatchCommand(redisClient *c)
{
    // O(N^2)
    unwatchAllKeys(c);

    c->flags &= (~REDIS_DIRTY_CAS);

    addReply(c,shared.ok);
}
