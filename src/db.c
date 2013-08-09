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

#include <signal.h>
#include <ctype.h>

void SlotToKeyAdd(robj *key);
void SlotToKeyDel(robj *key);

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

/*
 * �����ݿ� db �в��Ҹ��� key
 *
 * T = O(1)
 */
robj *lookupKey(redisDb *db, robj *key)
{

    // ���� key ����
    dictEntry *de = dictFind(db->dict,key->ptr);

    // ���ڣ�
    if (de)
    {
        // ȡ�� key ��Ӧ��ֵ����
        robj *val = dictGetVal(de);

        /* Update the access time for the aging algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        // �������������ô���� lru ʱ��
        if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
            val->lru = server.lruclock;

        return val;
    }
    else
    {
        // ������
        return NULL;
    }
}

/*
 * Ϊ���ж���������ȡ���ݿ�
 */
robj *lookupKeyRead(redisDb *db, robj *key)
{

    robj *val;

    // ��� key �Ƿ���ڣ�����ǵĻ�������ɾ��
    expireIfNeeded(db,key);

    // ���� key �������ݲ��ҽ����������/��������
    val = lookupKey(db,key);
    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;

    // ���� key ��ֵ
    return val;
}

/*
 * Ϊ����д��������ȡ���ݿ�
 *
 * ��������� lookupKeyRead ��������
 * �����������������/�����м���
 */
robj *lookupKeyWrite(redisDb *db, robj *key)
{
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}

/*
 * Ϊִ�ж�ȡ�����������ݿ���ȡ������ key ��ֵ��
 * ��� key �����ڣ���ͻ��˷�����Ϣ reply ��
 */
robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply)
{
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/*
 * Ϊִ��д������������ݿ���ȡ������ key ��ֵ��
 * ��� key �����ڣ���ͻ��˷�����Ϣ reply ��
 */
robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply)
{
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counte of the value if needed.
 *
 * ��Ӹ��� key - value �Ե����ݿ�
 * �� value �����ü��������ɵ����߾���
 *
 * The program is aborted if the key already exists.
 *
 * ���ֻ�� key �����ڵ�����½���
 */
void dbAdd(redisDb *db, robj *key, robj *val)
{
    // �����ַ�����
    sds copy = sdsdup(key->ptr);
    // ���� ��-ֵ ��
    int retval = dictAdd(db->dict, copy, val);

    redisAssertWithInfo(NULL,key,retval == REDIS_OK);

    if (server.cluster_enabled) SlotToKeyAdd(key);
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * ʹ����ֵ value ����ԭ�� key �ľ�ֵ
 * �� value �����ü��������ɵ����߾���
 *
 * The program is aborted if the key was not already present.
 *
 * ���ֻ�� key ���ڵ�����½���
 */
void dbOverwrite(redisDb *db, robj *key, robj *val)
{
    // ȡ���ڵ�
    struct dictEntry *de = dictFind(db->dict,key->ptr);

    redisAssertWithInfo(NULL,key,de != NULL);

    // ����ֵ���Ǿ�ֵ
    dictReplace(db->dict, key->ptr, val);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * �߽� set ������
 * ���Ը�һ�� key ���� value ������ key �Ƿ���ڡ�
 *
 * 1) The ref count of the value object is incremented.
 *    value ��������ü���������
 * 2) clients WATCHing for the destination key notified.
 *    ����� key ���ڱ� WATCH ����ô��֪�ͻ������ key �ѱ��޸�
 * 3) The expire time of the key is reset (the key is made persistent).
 *    key �Ĺ���ʱ�䣨����еĻ����ᱻ���ã��� key ��Ϊ�־û���
 */
void setKey(redisDb *db, robj *key, robj *val)
{
    // ���� key �Ĵ������������ key ��д��򸲸ǲ���
    if (lookupKeyWrite(db,key) == NULL)
    {
        dbAdd(db,key,val);
    }
    else
    {
        dbOverwrite(db,key,val);
    }

    // ����ֵ�����ü���
    incrRefCount(val);

    // �Ƴ��� key ԭ�еĹ���ʱ�䣨����еĻ���
    removeExpire(db,key);

    // ��֪�������� WATCH ������Ŀͻ��ˣ����Ѿ����޸�
    signalModifiedKey(db,key);
}

/*
 * ��� key �Ƿ������ DB
 *
 * �ǵĻ����� 1 �����򷵻� 0
 */
int dbExists(redisDb *db, robj *key)
{
    return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * �� Redis Object ����ʽ����������ݿ��е�һ�� key
 * ������ݿ�Ϊ�գ���ô���� NULL
 *
 * The function makes sure to return keys not already expired.
 *
 * ����ֻ����δ���ڵ� key
 */
robj *dbRandomKey(redisDb *db)
{
    struct dictEntry *de;

    while(1)
    {
        sds key;
        robj *keyobj;

        // ���ֵ��з������ֵ�� O(N)
        de = dictGetRandomKey(db->dict);
        // ���ݿ�Ϊ��
        if (de == NULL) return NULL;

        // ȡ��ֵ����
        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        // ��� key �Ƿ��ѹ���
        if (dictFind(db->expires,key))
        {
            if (expireIfNeeded(db,keyobj))
            {
                decrRefCount(keyobj);
                // ��� key �ѹ��ڣ�����Ѱ���¸� key
                continue; /* search for another key. This expired. */
            }
        }

        return keyobj;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
/*
 * �����ݿ���ɾ�� key ��key ��Ӧ��ֵ���Լ���Ӧ�Ĺ���ʱ�䣨����еĻ���
 */
int dbDelete(redisDb *db, robj *key)
{
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    // ��ɾ������ʱ��
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);

    // ɾ�� key �� value
    if (dictDelete(db->dict,key->ptr) == DICT_OK)
    {
        if (server.cluster_enabled) SlotToKeyDel(key);
        return 1;
    }
    else
    {
        return 0;
    }
}

/*
 * ����������ݿ�
 *
 * T = O(N^2)
 */
long long emptyDb()
{
    int j;
    long long removed = 0;

    // ����������ݿ�, O(N^2)
    for (j = 0; j < server.dbnum; j++)
    {
        removed += dictSize(server.db[j].dict);
        // O(N)
        dictEmpty(server.db[j].dict);
        // O(N)
        dictEmpty(server.db[j].expires);
    }

    // ��������� key ����
    return removed;
}

/*
 * ѡ�����ݿ�
 *
 * T = O(1)
 */
int selectDb(redisClient *c, int id)
{

    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;

    c->db = &server.db[id];

    return REDIS_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

/*
 * ֪ͨ���м��� key �Ŀͻ��ˣ�key �ѱ��޸ġ�
 *
 * touchWatchedKey ������ multi.c
 */
void signalModifiedKey(redisDb *db, robj *key)
{
    touchWatchedKey(db,key);
}

/*
 * FLUSHDB/FLUSHALL �������֮���֪ͨ����
 *
 * touchWatchedKeysOnFlush ������ multi.c
 */
void signalFlushedDb(int dbid)
{
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

/*
 * ��տͻ��˵�ǰ��ʹ�õ����ݿ�
 */
void flushdbCommand(redisClient *c)
{
    server.dirty += dictSize(c->db->dict);
    signalFlushedDb(c->db->id);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    addReply(c,shared.ok);
}

/*
 * ����������ݿ�
 */
void flushallCommand(redisClient *c)
{

    signalFlushedDb(-1);

    // ����������ݿ�
    server.dirty += emptyDb();

    addReply(c,shared.ok);

    // �������ִ�����ݿ�ı��湤������ôǿ���ж���
    if (server.rdb_child_pid != -1)
    {
        kill(server.rdb_child_pid,SIGKILL);
        rdbRemoveTempFile(server.rdb_child_pid);
    }

    if (server.saveparamslen > 0)
    {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        int saved_dirty = server.dirty;
        rdbSave(server.rdb_filename);
        server.dirty = saved_dirty;
    }
    server.dirty++;
}

/*
 * �����ݿ���ɾ�����и��� key
 */
void delCommand(redisClient *c)
{
    int deleted = 0, j;

    for (j = 1; j < c->argc; j++)
    {
        if (dbDelete(c->db,c->argv[j]))
        {
            signalModifiedKey(c->db,c->argv[j]);
            server.dirty++;
            deleted++;
        }
    }

    addReplyLongLong(c,deleted);
}

/*
 * ������ key �Ƿ����
 */
void existsCommand(redisClient *c)
{
    expireIfNeeded(c->db,c->argv[1]);
    if (dbExists(c->db,c->argv[1]))
    {
        addReply(c, shared.cone);
    }
    else
    {
        addReply(c, shared.czero);
    }
}

/*
 * �л����ݿ�
 */
void selectCommand(redisClient *c)
{
    long id;

    // id �ű���������
    if (getLongFromObjectOrReply(c, c->argv[1], &id,
                                 "invalid DB index") != REDIS_OK)
        return;

    // �������ڼ�Ⱥģʽ���ƺ��� SELECT
    if (server.cluster_enabled && id != 0)
    {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }

    // �л����ݿ�
    if (selectDb(c,id) == REDIS_ERR)
    {
        addReplyError(c,"invalid DB index");
    }
    else
    {
        addReply(c,shared.ok);
    }
}

/*
 * RANDOMKEY �����ʵ��
 *
 * ��������ݿ��з���һ����
 */
void randomkeyCommand(redisClient *c)
{
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL)
    {
        addReply(c,shared.nullbulk);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

/*
 * KEYS �����ʵ��
 *
 * ���Һ͸���ģʽƥ��� key
 */
void keysCommand(redisClient *c)
{
    dictIterator *di;
    dictEntry *de;

    sds pattern = c->argv[1]->ptr;

    int plen = sdslen(pattern),
        allkeys;
    unsigned long numkeys = 0;
    void *replylen = addDeferredMultiBulkLength(c);

    // ָ��ǰ���ݿ�� key space
    di = dictGetSafeIterator(c->db->dict);
    // key ��ƥ��ģʽ
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while((de = dictNext(di)) != NULL)
    {
        sds key = dictGetKey(de);
        robj *keyobj;

        // ��鵱ǰ�������� key �Ƿ�ƥ�䣬����ǵĻ�����������
        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0))
        {
            keyobj = createStringObject(key,sdslen(key));
            // ֻ���ز����ڵ� key
            if (expireIfNeeded(c->db,keyobj) == 0)
            {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);

    setDeferredMultiBulkLength(c,replylen,numkeys);
}

/*
 * DBSIZE �����ʵ��
 *
 * �������ݿ��ֵ������
 */
void dbsizeCommand(redisClient *c)
{
    addReplyLongLong(c,dictSize(c->db->dict));
}

/*
 * LASTSAVE �����ʵ��
 *
 * �������ݿ����󱣴�ʱ��
 */
void lastsaveCommand(redisClient *c)
{
    addReplyLongLong(c,server.lastsave);
}

/*
 * TYPE �����ʵ��
 *
 * ���� key �������͵��ַ�����ʽ
 */
void typeCommand(redisClient *c)
{
    robj *o;
    char *type;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL)
    {
        type = "none";
    }
    else
    {
        switch(o->type)
        {
        case REDIS_STRING:
            type = "string";
            break;
        case REDIS_LIST:
            type = "list";
            break;
        case REDIS_SET:
            type = "set";
            break;
        case REDIS_ZSET:
            type = "zset";
            break;
        case REDIS_HASH:
            type = "hash";
            break;
        default:
            type = "unknown";
            break;
        }
    }

    addReplyStatus(c,type);
}

/*
 * �رշ�����
 */
void shutdownCommand(redisClient *c)
{
    int flags = 0;

    // ѡ��رյ�ģʽ
    if (c->argc > 2)
    {
        addReply(c,shared.syntaxerr);
        return;
    }
    else if (c->argc == 2)
    {
        if (!strcasecmp(c->argv[1]->ptr,"nosave"))
        {
            flags |= REDIS_SHUTDOWN_NOSAVE;
        }
        else if (!strcasecmp(c->argv[1]->ptr,"save"))
        {
            flags |= REDIS_SHUTDOWN_SAVE;
        }
        else
        {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    // �ر�
    if (prepareForShutdown(flags) == REDIS_OK) exit(0);

    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

/*
 * �� key ���и���
 */
void renameGenericCommand(redisClient *c, int nx)
{
    robj *o;
    long long expire;

    /* To use the same key as src and dst is probably an error */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0)
    {
        addReply(c,shared.sameobjecterr);
        return;
    }

    // ȡ��Դ key
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    incrRefCount(o);
    expire = getExpire(c->db,c->argv[1]);
    // ȡ��Ŀ�� key
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL)
    {
        // ���Ŀ�� key ���ڣ��� nx FLAG �򿪣���ô����ʧ�ܣ�ֱ�ӷ���
        if (nx)
        {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one with the same name. */
        // ���򣬽�Ŀ�� key ɾ��
        dbDelete(c->db,c->argv[2]);
    }
    // ��Դ������Ŀ�� key ��������ӵ����ݿ�
    dbAdd(c->db,c->argv[2],o);
    // ���Դ key �г�ʱʱ�䣬��ô������ key �ĳ�ʱʱ��
    if (expire != -1) setExpire(c->db,c->argv[2],expire);
    // ɾ���ɵ�Դ key
    dbDelete(c->db,c->argv[1]);

    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);

    server.dirty++;

    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(redisClient *c)
{
    renameGenericCommand(c,0);
}

void renamenxCommand(redisClient *c)
{
    renameGenericCommand(c,1);
}

/*
 * �� key ��һ�����ݿ��ƶ�����һ�����ݿ�
 */
void moveCommand(redisClient *c)
{
    robj *o;
    redisDb *src, *dst;
    int srcid;

    // �������ڼ�Ⱥ�����ʹ��
    if (server.cluster_enabled)
    {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    // ��¼Դ���ݿ�
    src = c->db;
    srcid = c->db->id;
    // ͨ���л����ݿ�������Ŀ�����ݿ��Ƿ����
    if (selectDb(c,atoi(c->argv[2]->ptr)) == REDIS_ERR)
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
    // ��¼Ŀ�����ݿ�
    dst = c->db;
    // �л���Դ���ݿ�
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    // Դ���ݿ��Ŀ�����ݿ���ͬ��ֱ�ӷ���
    if (src == dst)
    {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    // ���Դ key �Ĵ�����
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o)
    {
        addReply(c,shared.czero);
        return;
    }

    /* Return zero if the key already exists in the target DB */
    // ��� key �Ѿ�������Ŀ�����ݿ⣬��ô����
    if (lookupKeyWrite(dst,c->argv[1]) != NULL)
    {
        addReply(c,shared.czero);
        return;
    }

    // �� key ��ӵ�Ŀ�����ݿ�
    dbAdd(dst,c->argv[1],o);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    // ɾ��Դ���ݿ��е� key
    dbDelete(src,c->argv[1]);

    server.dirty++;
    addReply(c,shared.cone);
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

/*
 * �Ƴ� key �Ĺ���ʱ��
 */
int removeExpire(redisDb *db, robj *key)
{
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/*
 * Ϊ key ���ù���ʱ��
 */
void setExpire(redisDb *db, robj *key, long long when)
{
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict,key->ptr);
    redisAssertWithInfo(NULL,key,kde != NULL);
    de = dictReplaceRaw(db->expires,dictGetKey(kde));
    dictSetSignedIntegerVal(de,when);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
/*
 * ���ظ��� key �Ĺ���ʱ��
 *
 * ������� key û�к�ĳ������ʱ�����������һ������ʧ key ��
 * ��ô���� -1
 */
long long getExpire(redisDb *db, robj *key)
{
    dictEntry *de;

    /* No expire? return ASAP */
    // ���ݿ�Ĺ��ڼ�¼��û���κ�����
    // ���ߣ����ڼ�¼��û�к� key ������ʱ��
    // ��ôֱ�ӷ���
    if (dictSize(db->expires) == 0 ||
            (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    // ȷ�� key �����ݿ��бض����ڣ���ȫ�Լ�飩
    redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);

    // ȡ���ֵ�ֵ�б��������ֵ
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
/*
 * �����ڵ�� AOF �ļ�������������
 *
 * ��һ���������ڵ��й���ʱ������һ�� DEL ������и����ڵ�� AOF �ļ�
 *
 * ͨ����ɾ�����ڼ��Ĺ������������ڵ��У�����ά�����ݿ��һ���ԡ�
 */
void propagateExpire(redisDb *db, robj *key)
{
    robj *argv[2];

    // DEL ����
    argv[0] = shared.del;
    // Ŀ���
    argv[1] = key;

    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    if (server.aof_state != REDIS_AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);

    if (listLength(server.slaves))
        replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/*
 * ��� key �Ѿ����ڣ���ô����ɾ�������򣬲���������
 *
 * key û�й���ʱ�䡢��������������� key δ����ʱ������ 0
 * key �ѹ��ڣ���ô��������ֵ
 */
int expireIfNeeded(redisDb *db, robj *key)
{
    // ȡ�� key �Ĺ���ʱ��
    long long when = getExpire(db,key);

    // key û�й���ʱ�䣬ֱ�ӷ���
    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    // ��Ҫ�ڷ�������������ʱִ�й���
    if (server.loading) return 0;

    /* If we are running in the context of a slave, return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    // �����������Ϊ�����ڵ����У���ôֱ�ӷ���
    // ��Ϊ�����ڵ�Ĺ����������ڵ�ͨ������ DEL ������ɾ����
    // ��������ɾ��
    if (server.masterhost != NULL)
    {
        // ����һ����������ȷ��ֵ������ִ��ʵ�ʵ�ɾ������
        return mstime() > when;
    }

    /* Return when this key has not expired */
    // δ����
    if (mstime() <= when) return 0;

    /* Delete the key */
    server.stat_expiredkeys++;

    // ������������
    propagateExpire(db,key);

    // �����ݿ���ɾ�� key
    return dbDelete(db,key);
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * ��������� EXPIRE �� PEXPIRE �� EXPIREAT �� PEXPIREAT �����ʵ�֡�
 *
 * ����ĵڶ������������Ǿ���ֵ��Ҳ���������ֵ��
 * ��ִ�� *AT ����ʱ�� basetime Ϊ 0 ������������£�������ľ��ǵ�ǰ�ľ���ʱ�䡣
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliesconds.
 *
 * unit ����ָ���ڶ��������ĸ�ʽ���������� UNIT_SECONDS �� UNIT_MILLISECONDS ��
 * basetime �������Ǻ����ʽ�ġ�
 */
void expireGenericCommand(redisClient *c, long long basetime, int unit)
{

    dictEntry *de;

    robj *key = c->argv[1],
          *param = c->argv[2];

    long long when; /* unix time in milliseconds when the key will expire. */

    // ȡ�� when ����
    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != REDIS_OK)
        return;

    // ��� when ����������㣬��ô����ת���ɺ���
    if (unit == UNIT_SECONDS) when *= 1000;
    // ��ʱ������Ϊ����ʱ��
    when += basetime;

    // ȡ����
    de = dictFind(c->db->dict,key->ptr);
    if (de == NULL)
    {
        // �������ڣ����� 0
        addReply(c,shared.czero);
        return;
    }
    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we take the other branch of the IF statement setting an expire
     * (possibly in the past) and wait for an explicit DEL from the master. */
    // �����ǰ�ڵ�Ϊ���ڵ�
    // �����ڸ����ڵ���� AOF �ļ��н��͵��˸��� TTL �������Ѿ����ڵľ���ʱ��
    // ��ôɾ�� key ���������ڵ�� AOF ���� DEL ����
    if (when <= mstime() && !server.loading && !server.masterhost)
    {
        robj *aux;

        redisAssertWithInfo(c,key,dbDelete(c->db,key));
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL. */
        aux = createStringObject("DEL",3);
        rewriteClientCommandVector(c,2,aux,key);
        decrRefCount(aux);
        signalModifiedKey(c->db,key);
        addReply(c, shared.cone);
        return;

        // �������� key �Ĺ���ʱ��
    }
    else
    {

        setExpire(c->db,key,when);

        addReply(c,shared.cone);
        signalModifiedKey(c->db,key);
        server.dirty++;
        return;
    }
}

void expireCommand(redisClient *c)
{
    expireGenericCommand(c,mstime(),UNIT_SECONDS);
}

void expireatCommand(redisClient *c)
{
    expireGenericCommand(c,0,UNIT_SECONDS);
}

void pexpireCommand(redisClient *c)
{
    expireGenericCommand(c,mstime(),UNIT_MILLISECONDS);
}

void pexpireatCommand(redisClient *c)
{
    expireGenericCommand(c,0,UNIT_MILLISECONDS);
}

void ttlGenericCommand(redisClient *c, int output_ms)
{
    long long expire, ttl = -1;

    expire = getExpire(c->db,c->argv[1]);
    /* If the key does not exist at all, return -2 */
    if (expire == -1 && lookupKeyRead(c->db,c->argv[1]) == NULL)
    {
        addReplyLongLong(c,-2);
        return;
    }
    /* The key exists. Return -1 if it has no expire, or the actual
     * TTL value otherwise. */
    if (expire != -1)
    {
        ttl = expire-mstime();
        if (ttl < 0) ttl = -1;
    }
    if (ttl == -1)
    {
        addReplyLongLong(c,-1);
    }
    else
    {
        addReplyLongLong(c,output_ms ? ttl : ((ttl+500)/1000));
    }
}

void ttlCommand(redisClient *c)
{
    ttlGenericCommand(c, 0);
}

void pttlCommand(redisClient *c)
{
    ttlGenericCommand(c, 1);
}

void persistCommand(redisClient *c)
{
    dictEntry *de;

    de = dictFind(c->db->dict,c->argv[1]->ptr);
    if (de == NULL)
    {
        addReply(c,shared.czero);
    }
    else
    {
        if (removeExpire(c->db,c->argv[1]))
        {
            addReply(c,shared.cone);
            server.dirty++;
        }
        else
        {
            addReply(c,shared.czero);
        }
    }
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys)
{
    int j, i = 0, last, *keys;
    REDIS_NOTUSED(argv);

    if (cmd->firstkey == 0)
    {
        *numkeys = 0;
        return NULL;
    }
    last = cmd->lastkey;
    if (last < 0) last = argc+last;
    keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
    for (j = cmd->firstkey; j <= last; j += cmd->keystep)
    {
        redisAssert(j < argc);
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

int *getKeysFromCommand(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags)
{
    if (cmd->getkeys_proc)
    {
        return cmd->getkeys_proc(cmd,argv,argc,numkeys,flags);
    }
    else
    {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

void getKeysFreeResult(int *result)
{
    zfree(result);
}

int *noPreloadGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags)
{
    if (flags & REDIS_GETKEYS_PRELOAD)
    {
        *numkeys = 0;
        return NULL;
    }
    else
    {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

int *renameGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags)
{
    if (flags & REDIS_GETKEYS_PRELOAD)
    {
        int *keys = zmalloc(sizeof(int));
        *numkeys = 1;
        keys[0] = 1;
        return keys;
    }
    else
    {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

int *zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags)
{
    int i, num, *keys;
    REDIS_NOTUSED(cmd);
    REDIS_NOTUSED(flags);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num > (argc-3))
    {
        *numkeys = 0;
        return NULL;
    }
    keys = zmalloc(sizeof(int)*num);
    for (i = 0; i < num; i++) keys[i] = 3+i;
    *numkeys = num;
    return keys;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster. */
void SlotToKeyAdd(robj *key)
{
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));

    zslInsert(server.cluster.slots_to_keys,hashslot,key);
    incrRefCount(key);
}

void SlotToKeyDel(robj *key)
{
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));

    zslDelete(server.cluster.slots_to_keys,hashslot,key);
}

unsigned int GetKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count)
{
    zskiplistNode *n;
    zrangespec range;
    int j = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    n = zslFirstInRange(server.cluster.slots_to_keys, range);
    while(n && n->score == hashslot && count--)
    {
        keys[j++] = n->obj;
        n = n->level[0].forward;
    }
    return j;
}
