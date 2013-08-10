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
#include "bio.h"
#include "rio.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

void aofUpdateCurrentSize(void);

/* ----------------------------------------------------------------------------
 * AOF rewrite buffer implementation.
 *
 * The following code implement a simple buffer used in order to accumulate
 * changes while the background process is rewriting the AOF file.
 *
 * ������������� AOF �ļ�������дʱ�ݴ���ִ�е����
 *
 * We only need to append, but can't just use realloc with a large block
 * because 'huge' reallocs are not always handled as one could expect
 * (via remapping of pages at OS level) but may involve copying data.
 *
 * For this reason we use a list of blocks, every block is
 * AOF_RW_BUF_BLOCK_SIZE bytes.
 *
 * ��Ϊ����һ���ǳ���Ŀռ䲢�����ǿ��ܵģ�
 * Ҳ���ܲ��������ĸ��ƹ�����
 * ��������ʹ�ö�� AOF_RW_BUF_BLOCK_SIZE �ռ����������
 *
 * ------------------------------------------------------------------------- */

#define AOF_RW_BUF_BLOCK_SIZE (1024*1024*10)    /* 10 MB per block */

typedef struct aofrwblock
{
    unsigned long used, // ��ʹ���ֽ�
             free; // ʣ������ֽ�
    char buf[AOF_RW_BUF_BLOCK_SIZE];
} aofrwblock;

/* This function free the old AOF rewrite buffer if needed, and initialize
 * a fresh new one.
 *
 * �������Ҫ���ͷžɵ� AOF ���棬����ʼ��һ���µ� AOF ���档
 *
 * It tests for server.aof_rewrite_buf_blocks equal to NULL
 * so can be used for the first initialization as well.
 *
 * �������Ҳ�������ڳ�ʼ�� AOF ���档
 */
void aofRewriteBufferReset(void)
{
    if (server.aof_rewrite_buf_blocks)
        listRelease(server.aof_rewrite_buf_blocks);

    server.aof_rewrite_buf_blocks = listCreate();
    listSetFreeMethod(server.aof_rewrite_buf_blocks,zfree);
}

/* Return the current size of the AOF rerwite buffer.
 *
 * ���ص�ǰ AOF ��д����Ĵ�С
 */
unsigned long aofRewriteBufferSize(void)
{
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    if (block == NULL) return 0;

    // �ܻ����С = ÿ�����С * (������ - 1) + ���һ����Ĵ�С
    unsigned long size =
        (listLength(server.aof_rewrite_buf_blocks)-1) * AOF_RW_BUF_BLOCK_SIZE;
    size += block->used;
    return size;
}

/* Append data to the AOF rewrite buffer, allocating new blocks if needed.
 *
 * ������ s ׷�ӵ� AOF �����ĩβ��
 * �������Ҫ�Ļ�������һ���µĻ���顣
 */
void aofRewriteBufferAppend(unsigned char *s, unsigned long len)
{
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    while(len)
    {
        /* If we already got at least an allocated block, try appending
         * at least some piece into it. */
        // �����ݱ��浽���һ������
        // ���ݱ���������ǲ����ģ�������Ҫ����һ���¿�����������
        if (block)
        {
            unsigned long thislen = (block->free < len) ? block->free : len;
            if (thislen)    /* The current block is not already full. */
            {
                memcpy(block->buf+block->used, s, thislen);
                block->used += thislen;
                block->free -= thislen;
                s += thislen;
                len -= thislen;
            }
        }

        // �����һ���飬
        // ���ߴ�����һ��������������
        if (len)   /* First block to allocate, or need another block. */
        {
            int numblocks;

            block = zmalloc(sizeof(*block));
            block->free = AOF_RW_BUF_BLOCK_SIZE;
            block->used = 0;
            listAddNodeTail(server.aof_rewrite_buf_blocks,block);

            /* Log every time we cross more 10 or 100 blocks, respectively
             * as a notice or warning. */
            numblocks = listLength(server.aof_rewrite_buf_blocks);
            if (((numblocks+1) % 10) == 0)
            {
                int level = ((numblocks+1) % 100) == 0 ? REDIS_WARNING :
                            REDIS_NOTICE;
                redisLog(level,"Background AOF buffer size: %lu MB",
                         aofRewriteBufferSize()/(1024*1024));
            }
        }
    }
}

/* Write the buffer (possibly composed of multiple blocks) into the specified
 * fd.
 *
 * �����棨�����ɶ������ɣ�д�뵽���� fd �С�
 *
 * If no short write or any other error happens -1 is returned,
 * otherwise the number of bytes written is returned.
 *
 * ��� short write �������������������� -1 ��
 * ���򷵻�д���ֽ�������
 *
 */
ssize_t aofRewriteBufferWrite(int fd)
{
    listNode *ln;
    listIter li;
    ssize_t count = 0;

    // �������п飬��д��
    listRewind(server.aof_rewrite_buf_blocks,&li);
    while((ln = listNext(&li)))
    {
        aofrwblock *block = listNodeValue(ln);
        ssize_t nwritten;

        if (block->used)
        {
            nwritten = write(fd,block->buf,block->used);
            if (nwritten != block->used)
            {
                if (nwritten == 0) errno = EIO;
                return -1;
            }
            count += nwritten;
        }
    }

    return count;
}

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
/*
 * ����һ��д AOF �ļ��ĺ�̨����
 */
void aof_background_fsync(int fd)
{
    bioCreateBackgroundJob(REDIS_BIO_AOF_FSYNC,(void*)(long)fd,NULL,NULL);
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
/*
 * �û�������ʱͨ�� CONFIG ����ر� AOF ģʽʱִ��
 */
void stopAppendOnly(void)
{
    redisAssert(server.aof_state != REDIS_AOF_OFF);

    // ǿ�Ƴ�ϴ���浽 AOF �ļ�
    flushAppendOnlyFile(1);
    // fsync
    aof_fsync(server.aof_fd);
    // �ر� AOF �ļ�
    close(server.aof_fd);

    // ���� AOF ״̬
    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = REDIS_AOF_OFF;

    /* rewrite operation in progress? kill it, wait child exit */
    // ɱ������ִ�е� AOF ��д����
    if (server.aof_child_pid != -1)
    {
        int statloc;

        redisLog(REDIS_NOTICE,"Killing running AOF rewrite child: %ld",
                 (long) server.aof_child_pid);
        if (kill(server.aof_child_pid,SIGKILL) != -1)
            wait3(&statloc,0,NULL);
        /* reset the buffer accumulating changes while the child saves */
        // �ͷŻ���
        aofRewriteBufferReset();
        // �Ƴ���ʱ�ļ�
        aofRemoveTempFile(server.aof_child_pid);
        // �رշ����� flag
        server.aof_child_pid = -1;
        server.aof_rewrite_time_start = -1;
    }
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
/*
 * ���û��� AOF ѡ��ʱ����
 */
int startAppendOnly(void)
{
    server.aof_last_fsync = server.unixtime;
    server.aof_fd = open(server.aof_filename,O_WRONLY|O_APPEND|O_CREAT,0644);
    redisAssert(server.aof_state == REDIS_AOF_OFF);
    if (server.aof_fd == -1)
    {
        redisLog(REDIS_WARNING,"Redis needs to enable the AOF but can't open the append only file: %s",strerror(errno));
        return REDIS_ERR;
    }
    // ���ɳ�ʼ�� AOF �ļ�
    if (rewriteAppendOnlyFileBackground() == REDIS_ERR)
    {
        close(server.aof_fd);
        redisLog(REDIS_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
        return REDIS_ERR;
    }
    /* We correctly switched on AOF, now wait for the rerwite to be complete
     * in order to append data on disk. */
    server.aof_state = REDIS_AOF_WAIT_REWRITE;
    return REDIS_OK;
}

/* Write the append only file buffer on disk.
 *
 * �� AOF ����д���ļ���
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when the
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 *
 * ��Ϊ������Ҫ�ڻ�ظ��ͻ���֮ǰ�� AOF ִ��д������
 * ���ͻ�����ִ��д������Ψһ����������¼� loop �У�
 * ��Ϊ�������� AOF д���浽�����У�
 * ���ڽ����¼� loop ֮ǰ��������д�뵽�ļ��С�
 *
 * About the 'force' argument:
 *
 * ���� force ������
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 *
 * �� fsync �����ǡ�ÿ�����һ�� fsync��ʱ��
 * ��̨��������ܻ��� fsync �ȴ�ִ�в�������
 * ��Щ fsync ���� serverCron() ��ִ�С�
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync.
 *
 * ���ǣ���� force Ϊ 1 ����ô���ܺ�̨�����Ƿ��� fsync ��
 * ����ֱ��ִ�� fsync ��
 */
void flushAppendOnlyFile(int force)
{
    ssize_t nwritten;
    int sync_in_progress = 0;

    // û�л���ȴ�д�룬ֱ�ӷ���
    if (sdslen(server.aof_buf) == 0) return;

    // ���غ�̨���ڵȴ�ִ�е� fsync ����
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        sync_in_progress = bioPendingJobsOfType(REDIS_BIO_AOF_FSYNC) != 0;

    // AOF ģʽΪÿ�� fsync ������ force ��Ϊ 1
    // ������ԵĻ������ӳ�ϴ
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force)
    {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. */
        // ��� aof_fsync �������Ѿ������ڵȴ�������
        if (sync_in_progress)
        {

            // �Ƴ� aof ��д ...

            if (server.aof_flush_postponed_start == 0)
            {
                // ��һ��û���Ƴٳ�ϴ������¼���ӵĵ�ǰʱ�䣬Ȼ�󷵻�
                /* No previous write postponinig, remember that we are
                 * postponing the flush and return. */
                server.aof_flush_postponed_start = server.unixtime;
                return;

            }
            else if (server.unixtime - server.aof_flush_postponed_start < 2)
            {
                // ����������֮�ڵ����ӳ�ϴ
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. */
                return;
            }
            /* Otherwise fall trough, and go write since we can't wait
             * over two seconds. */
            // ��¼��ϴ���Ӵ���
            server.aof_delayed_fsync++;
            redisLog(REDIS_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }

    // ������һ������ϴ�Ѿ������Ƴ��ˣ���������Ϊ 0
    /* If you are following this code path, then we are going to write so
     * set reset the postponed flush sentinel to zero. */
    server.aof_flush_postponed_start = 0;

    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */
    // �� AOF ����д�뵽�ļ������һ�����˵Ļ���д���ԭ���Ե����
    nwritten = write(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    // д�����ֹͣ Redis ���������
    if (nwritten != (signed)sdslen(server.aof_buf))
    {
        /* Ooops, we are in troubles. The best thing to do for now is
         * aborting instead of giving the illusion that everything is
         * working as expected. */
        if (nwritten == -1)
        {
            redisLog(REDIS_WARNING,"Exiting on error writing to the append-only file: %s",strerror(errno));
        }
        else
        {
            redisLog(REDIS_WARNING,"Exiting on short write while writing to "
                     "the append-only file: %s (nwritten=%ld, "
                     "expected=%ld)",
                     strerror(errno),
                     (long)nwritten,
                     (long)sdslen(server.aof_buf));

            if (ftruncate(server.aof_fd, server.aof_current_size) == -1)
            {
                redisLog(REDIS_WARNING, "Could not remove short write "
                         "from the append-only file.  Redis may refuse "
                         "to load the AOF the next time it starts.  "
                         "ftruncate: %s", strerror(errno));
            }
        }
        exit(1);
    }
    // ���� AOF �ļ��ĵ�ǰ��С
    server.aof_current_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary). */
    // ��� aof ���治��̫����ô��������������� aof ����
    if ((sdslen(server.aof_buf)+sdsavail(server.aof_buf)) < 4000)
    {
        sdsclear(server.aof_buf);
    }
    else
    {
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
    }

    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    // ������������ʱ��ֱ�ӷ��أ���ִ�к���� fsnyc ��
    // �������� AOF ��дʱд�� AOF �ļ� ����
    // REWRITEAOF ����ִ�� ���� BGSAVE ���ڽ���
    if (server.aof_no_fsync_on_rewrite &&
            (server.aof_child_pid != -1 || server.rdb_child_pid != -1))
        return;

    /* Perform the fsync if needed. */
    // �������Ҫ��ִ�� fsync

    // AOF ģʽΪ���� fsync ����ôִ�� fsync
    if (server.aof_fsync == AOF_FSYNC_ALWAYS)
    {
        /* aof_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        aof_fsync(server.aof_fd); /* Let's try to get this data on the disk */
        // ���¶� AOF �ļ����һ�ν��� fsync ��ʱ��
        server.aof_last_fsync = server.unixtime;

        // AOF ģʽΪÿ��һ�Σ����Ҿ����ϴ�д AOF �ļ��Ѿ����� 1 ��
    }
    else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
              server.unixtime > server.aof_last_fsync))
    {
        // ����û�� fsync �ں�̨����ʱ���Ž��µ� fsync ����ŵ���ִ̨��
        if (!sync_in_progress) aof_background_fsync(server.aof_fd);
        // ���¶� AOF �ļ����һ�ν��� fsync ʱ��
        server.aof_last_fsync = server.unixtime;
    }
}

/*
 * ������������Ķ��󣬽����ԭ���ַ�����ʽ
 */
sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv)
{
    char buf[32];
    int len, j;
    robj *o;

    // ��¼Э��Ĳ����������֣�argc��
    // ���� argc == 4 ʱ�������ַ��� "*4\r\n"
    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);

    // �������в�����argv����������Э��
    for (j = 0; j < argc; j++)
    {

        // ȡ���ַ�����ʽ�Ĳ���
        o = getDecodedObject(argv[j]);

        // ��¼�����ַ������ȵ�Э��
        // ���磬�Գ���Ϊ 3 ������ SET �������ַ���
        // "$3\r\n"
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);

        // ���ݲ���������Э��
        // ���磬�����ַ��� SET �����������ַ�����
        // "SET\r\n"
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }

    return dst;
}

/* Create the sds representation of an PEXPIREAT command, using
 * 'seconds' as time to live and 'cmd' to understand what command
 * we are translating into a PEXPIREAT.
 *
 * ���� PEXPIREAT ����� sds ��ʾ��
 * cmd ��������ָ��ת����Դָ� seconds Ϊ TTL ��ʣ������ʱ�䣩��
 *
 * This command is used in order to translate EXPIRE and PEXPIRE commands
 * into PEXPIREAT command so that we retain precision in the append only
 * file, and the time is always absolute and not relative.
 *
 * ����������ڽ� EXPIRE �� PEXPIRE ת��Ϊ PEXPIREAT
 * �Ӷ��ڱ�֤��ȷ�Ȳ��������£�������ʱ������ֵת��Ϊ����ֵ��һ�� UNIX ʱ�������
 *
 * ��������ʱ����ȷ���ģ���ô���� AOF �ļ���ʱ�����룬�ù��ڵ� key ������ȷ�ع��ڡ���
 */
sds catAppendOnlyExpireAtCommand(sds buf, struct redisCommand *cmd, robj *key, robj *seconds)
{
    long long when;
    robj *argv[3];

    /* Make sure we can use strtol */
    // ȡ����Թ���ֵ
    seconds = getDecodedObject(seconds);
    when = strtoll(seconds->ptr,NULL,10);

    /* Convert argument into milliseconds for EXPIRE, SETEX, EXPIREAT */
    // ѡ�񾫶ȣ������룿
    if (cmd->proc == expireCommand || cmd->proc == setexCommand ||
            cmd->proc == expireatCommand)
    {
        when *= 1000;
    }

    // �������ֵ���������ֵ
    /* Convert into absolute time for EXPIRE, PEXPIRE, SETEX, PSETEX */
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
            cmd->proc == setexCommand || cmd->proc == psetexCommand)
    {
        when += mstime();
    }
    decrRefCount(seconds);

    // ת��Ϊ PEXPIREAT ����
    argv[0] = createStringObject("PEXPIREAT",9);
    argv[1] = key;
    argv[2] = createStringObjectFromLongLong(when);
    buf = catAppendOnlyGenericCommand(buf, 3, argv);

    decrRefCount(argv[0]);
    decrRefCount(argv[2]);

    return buf;
}

/*
 * ����������׷�ӵ� AOF �ļ�/������
 */
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc)
{
    sds buf = sdsempty();
    robj *tmpargv[3];

    /* The DB this command was targetting is not the same as the last command
     * we appendend. To issue a SELECT command is needed. */
    // ��ǰ db ����ָ���� aof db��
    // ͨ������ SELECT �������л����ݿ�
    if (dictid != server.aof_selected_db)
    {
        char seldb[64];

        // �� AOF �ļ��л� DB
        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
                           (unsigned long)strlen(seldb),seldb);

        // �����л� DB
        server.aof_selected_db = dictid;
    }

    // �� EXPIRE / PEXPIRE / EXPIREAT �����Ϊ PEXPIREAT ����
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
            cmd->proc == expireatCommand)
    {
        /* Translate EXPIRE/PEXPIRE/EXPIREAT into PEXPIREAT */
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);

        // �� SETEX / PSETEX �����Ϊ SET �� PEXPIREAT ����
    }
    else if (cmd->proc == setexCommand || cmd->proc == psetexCommand)
    {
        /* Translate SETEX/PSETEX to SET and PEXPIREAT */
        tmpargv[0] = createStringObject("SET",3);
        tmpargv[1] = argv[1];
        tmpargv[2] = argv[3];
        buf = catAppendOnlyGenericCommand(buf,3,tmpargv);
        decrRefCount(tmpargv[0]);
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);

        // ��������ֱ��׷�ӵ� buf ĩβ
    }
    else
    {
        /* All the other commands don't need translation or need the
         * same translation already operated in the command vector
         * for the replication itself. */
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    // �� buf ׷�ӵ��������� aof_buf ĩβ
    // �´� AOF д��ִ��ʱ����Щ���ݾͻᱻд��
    if (server.aof_state == REDIS_AOF_ON)
        server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));

    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. */
    // ��� AOF ��д����ִ�У���ôҲ���� buf ׷�ӵ� AOF ��д������
    // �� AOF ��д��֮ǰ������֮�������������Ҳ��׷�ӵ��� AOF �ļ��С�
    if (server.aof_child_pid != -1)
        aofRewriteBufferAppend((unsigned char*)buf,sdslen(buf));

    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
// ����һ��α�նˣ�����ִ�� AOF �б��������
struct redisClient *createFakeClient(void)
{
    struct redisClient *c = zmalloc(sizeof(*c));

    selectDb(c,0);
    c->fd = -1;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->argc = 0;
    c->argv = NULL;
    c->bufpos = 0;
    c->flags = 0;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    c->watched_keys = listCreate();
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

/*
 * �ͷ�α�ն�
 */
void freeFakeClient(struct redisClient *c)
{
    sdsfree(c->querybuf);
    listRelease(c->reply);
    listRelease(c->watched_keys);
    freeClientMultiState(c);
    zfree(c);
}

/* Replay the append log file. On error REDIS_OK is returned. On non fatal
 * error (the append only file is zero-length) REDIS_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
/*
 * ���� AOF �ļ�
 */
int loadAppendOnlyFile(char *filename)
{
    struct redisClient *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    int old_aof_state = server.aof_state;
    long loops = 0;

    // ���ļ�
    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0)
    {
        server.aof_current_size = 0;
        fclose(fp);
        return REDIS_ERR;
    }

    // ���ļ�ʧ��
    if (fp == NULL)
    {
        redisLog(REDIS_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    // ��ʱ�ر� AOF
    server.aof_state = REDIS_AOF_OFF;

    // ����α�ն�
    fakeClient = createFakeClient();
    // ������ rdb.c �����·�����������״̬
    startLoading(fp);

    while(1)
    {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time */
        // �м���ش����ⲿ����
        if (!(loops++ % 1000))
        {
            // rbd.c/loadingProgress
            loadingProgress(ftello(fp));
            aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        }

        // ����� EOF
        if (fgets(buf,sizeof(buf),fp) == NULL)
        {
            if (feof(fp))
                break;
            else
                goto readerr;
        }

        // ����������������
        if (buf[0] != '*') goto fmterr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;

        argv = zmalloc(sizeof(robj*)*argc);
        for (j = 0; j < argc; j++)
        {
            if (fgets(buf,sizeof(buf),fp) == NULL) goto readerr;
            if (buf[0] != '$') goto fmterr;
            len = strtol(buf+1,NULL,10);
            argsds = sdsnewlen(NULL,len);
            if (len && fread(argsds,len,1,fp) == 0) goto fmterr;
            argv[j] = createObject(REDIS_STRING,argsds);
            if (fread(buf,2,1,fp) == 0) goto fmterr; /* discard CRLF */
        }

        /* Command lookup */
        // ��������
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd)
        {
            redisLog(REDIS_WARNING,"Unknown command '%s' reading the append only file", argv[0]->ptr);
            exit(1);
        }
        /* Run the command in the context of a fake client */
        // ��α�ն���������ִ������
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        cmd->proc(fakeClient);

        /* The fake client should not have a reply */
        redisAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);
        /* The fake client should never get blocked */
        redisAssert((fakeClient->flags & REDIS_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        for (j = 0; j < fakeClient->argc; j++)
            decrRefCount(fakeClient->argv[j]);
        zfree(fakeClient->argv);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, log error and quit. */
    if (fakeClient->flags & REDIS_MULTI) goto readerr;

    // ������Դ������ԭ flag
    fclose(fp);
    freeFakeClient(fakeClient);
    server.aof_state = old_aof_state;
    stopLoading();
    aofUpdateCurrentSize();
    server.aof_rewrite_base_size = server.aof_current_size;
    return REDIS_OK;

readerr:
    if (feof(fp))
    {
        redisLog(REDIS_WARNING,"Unexpected end of file reading the append only file");
    }
    else
    {
        redisLog(REDIS_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
    }
    exit(1);
fmterr:
    redisLog(REDIS_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */

/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the redis.h dependency. */
/*
 * �� obj ��ָ����������ַ���д�뵽 r ���С�
 */
int rioWriteBulkObject(rio *r, robj *obj)
{
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    if (obj->encoding == REDIS_ENCODING_INT)
    {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    }
    else if (obj->encoding == REDIS_ENCODING_RAW)
    {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    }
    else
    {
        redisPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
/*
 * ���ؽ��б�������������д�뵽 r ��
 *
 * ������ 0 ���ɹ����� 1 ��
 */
int rewriteListObject(rio *r, robj *key, robj *o)
{
    long long count = 0, items = listTypeLength(o);

    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl = o->ptr;
        unsigned char *p = ziplistIndex(zl,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        while(ziplistGet(p,&vstr,&vlen,&vlong))
        {
            if (count == 0)
            {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                                REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"RPUSH",5) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (vstr)
            {
                if (rioWriteBulkString(r,(char*)vstr,vlen) == 0) return 0;
            }
            else
            {
                if (rioWriteBulkLongLong(r,vlong) == 0) return 0;
            }
            p = ziplistNext(zl,p);
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    }
    else if (o->encoding == REDIS_ENCODING_LINKEDLIST)
    {
        list *list = o->ptr;
        listNode *ln;
        listIter li;

        listRewind(list,&li);
        while((ln = listNext(&li)))
        {
            robj *eleobj = listNodeValue(ln);

            if (count == 0)
            {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                                REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"RPUSH",5) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    }
    else
    {
        redisPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. */
/*
 * ���ؽ����϶������������д�뵽 r ��
 *
 * ������ 0 ���ɹ����� 1 ��
 */
int rewriteSetObject(rio *r, robj *key, robj *o)
{
    long long count = 0, items = setTypeSize(o);

    if (o->encoding == REDIS_ENCODING_INTSET)
    {
        int ii = 0;
        int64_t llval;

        while(intsetGet(o->ptr,ii++,&llval))
        {
            if (count == 0)
            {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                                REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkLongLong(r,llval) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    }
    else if (o->encoding == REDIS_ENCODING_HT)
    {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while((de = dictNext(di)) != NULL)
        {
            robj *eleobj = dictGetKey(de);
            if (count == 0)
            {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                                REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    }
    else
    {
        redisPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. */
/*
 * д���ؽ������������� r ��
 *
 * ������ 0 ���ɹ����� 1 ��
 */
int rewriteSortedSetObject(rio *r, robj *key, robj *o)
{
    long long count = 0, items = zsetLength(o);

    if (o->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;

        eptr = ziplistIndex(zl,0);
        redisAssert(eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        redisAssert(sptr != NULL);

        while (eptr != NULL)
        {
            redisAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
            score = zzlGetScore(sptr);

            if (count == 0)
            {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                                REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkDouble(r,score) == 0) return 0;
            if (vstr != NULL)
            {
                if (rioWriteBulkString(r,(char*)vstr,vlen) == 0) return 0;
            }
            else
            {
                if (rioWriteBulkLongLong(r,vll) == 0) return 0;
            }
            zzlNext(zl,&eptr,&sptr);
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    }
    else if (o->encoding == REDIS_ENCODING_SKIPLIST)
    {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while((de = dictNext(di)) != NULL)
        {
            robj *eleobj = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0)
            {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                                REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkDouble(r,*score) == 0) return 0;
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    }
    else
    {
        redisPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of an hash.
 *
 * ��ѡ���� hash �� key ���� value д�뵽���� r ��
 *
 * The 'hi' argument passes a valid Redis hash iterator.
 *
 * hi ������ Redis ��ϣ��������
 *
 * The 'what' filed specifies if to write a key or a value and can be
 * either REDIS_HASH_KEY or REDIS_HASH_VALUE.
 *
 * what ָ��д�� key ����д�� value ��
 * ����ֵ������ REDIS_HASH_KEY ���� REDIS_HASH_VALUE ��
 *
 * The function returns 0 on error, non-zero on success.
 *
 * ������ 0 ���ɹ����ط� 0 ֵ��
 */
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what)
{
    if (hi->encoding == REDIS_ENCODING_ZIPLIST)
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr)
        {
            return rioWriteBulkString(r, (char*)vstr, vlen);
        }
        else
        {
            return rioWriteBulkLongLong(r, vll);
        }

    }
    else if (hi->encoding == REDIS_ENCODING_HT)
    {
        robj *value;

        hashTypeCurrentFromHashTable(hi, what, &value);
        return rioWriteBulkObject(r, value);
    }

    redisPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. */
/*
 * ���ؽ���ϣ���������д�뵽 r ��
 *
 * ʧ�ܷ��� 0 ���ɹ����� 1 ��
 */
int rewriteHashObject(rio *r, robj *key, robj *o)
{
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != REDIS_ERR)
    {
        if (count == 0)
        {
            int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                            REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

            if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
            if (rioWriteBulkString(r,"HMSET",5) == 0) return 0;
            if (rioWriteBulkObject(r,key) == 0) return 0;
        }

        if (rioWriteHashIteratorCursor(r, hi, REDIS_HASH_KEY) == 0) return 0;
        if (rioWriteHashIteratorCursor(r, hi, REDIS_HASH_VALUE) == 0) return 0;
        if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * дһ�����Ի�ԭ���ݼ�����������ļ��
 * �� REWRITEAOF �� BGREWRITEAOF ��ʹ�á�
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max REDIS_AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command.
 *
 * Ϊ�˼����ؽ����ݼ����������������
 * �ڿ���ʱ��Redis ��ʹ�ÿɱ����������� RPUSH �� SADD �� ZADD ��
 * ������Щ����ÿ�������ӵ�Ԫ�ز��ᳬ�� REDIS_AOF_REWRITE_ITEMS_PER_CMD ��
 *
 * ��дʧ�ܷ��� REDIS_ERR ���ɹ����� REDIS_OK ��
 */
int rewriteAppendOnlyFile(char *filename)
{
    dictIterator *di = NULL;
    dictEntry *de;
    rio aof;
    FILE *fp;
    char tmpfile[256];
    int j;
    long long now = mstime();

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp)
    {
        redisLog(REDIS_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", strerror(errno));
        return REDIS_ERR;
    }

    // ��ʼ���ļ���
    rioInitWithFile(&aof,fp);
    // �����������ݿ�
    for (j = 0; j < server.dbnum; j++)
    {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);
        if (!di)
        {
            fclose(fp);
            return REDIS_ERR;
        }

        /* SELECT the new DB */
        // �л������ʵ����ݿ���
        if (rioWrite(&aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(&aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        // �������ݿ������ key-value ��
        while((de = dictNext(di)) != NULL)
        {
            sds keystr;
            robj key, *o;
            long long expiretime;

            keystr = dictGetKey(de);
            o = dictGetVal(de);
            initStaticStringObject(key,keystr);

            expiretime = getExpire(db,&key);

            /* Save the key and associated value */
            // ���� key �� value
            if (o->type == REDIS_STRING)
            {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value */
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(&aof,o) == 0) goto werr;
            }
            else if (o->type == REDIS_LIST)
            {
                if (rewriteListObject(&aof,&key,o) == 0) goto werr;
            }
            else if (o->type == REDIS_SET)
            {
                if (rewriteSetObject(&aof,&key,o) == 0) goto werr;
            }
            else if (o->type == REDIS_ZSET)
            {
                if (rewriteSortedSetObject(&aof,&key,o) == 0) goto werr;
            }
            else if (o->type == REDIS_HASH)
            {
                if (rewriteHashObject(&aof,&key,o) == 0) goto werr;
            }
            else
            {
                redisPanic("Unknown object type");
            }
            /* Save the expire time */
            // ��������еĹ���ʱ��
            if (expiretime != -1)
            {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                /* If this key is already expired skip it */
                // ����д�ѹ��ڵ� key
                if (expiretime < now) continue;
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(&aof,expiretime) == 0) goto werr;
            }
        }
        dictReleaseIterator(di);
    }

    /* Make sure data will not remain on the OS's output buffers */
    // �����ļ���
    fflush(fp);
    // sync
    aof_fsync(fileno(fp));
    // �ر�
    fclose(fp);

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    // ͨ������������д����� AOF �ļ�����ɵ� AOF �ļ�
    if (rename(tmpfile,filename) == -1)
    {
        redisLog(REDIS_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"SYNC append only file rewrite performed");
    return REDIS_OK;

werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

/* This is how rewriting of the append only file in background works:
 *
 * �����Ǻ�̨��д AOF �ļ��Ĺ������裺
 *
 * 1) The user calls BGREWRITEAOF
 *    �û����� BGREWRITEAOF
 *
 * 2) Redis calls this function, that forks():
 *    Redis ���������������ִ�� fork() ��
 *
 *    2a) the child rewrite the append only file in a temp file.
 *        �ӽ�������ʱ�ļ��ж� AOF �ļ�������д
 *
 *    2b) the parent accumulates differences in server.aof_rewrite_buf.
 *        �����̽������������׷�ӵ� server.aof_rewrite_buf ��
 *
 * 3) When the child finished '2a' exists.
 *    ������ 2a ִ����֮���ӽ��̽���
 *
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.aof_rewrite_buf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 *
 *    ����ӽ��̵��˳�״̬�� OK �Ļ�����ô�����̽�����������д�뵽��ʱ�ļ���
 *    Ȼ�����ʱ�ļ���������������ɵ� AOF �ļ������ˣ���̨ AOF ��д��ɡ�
 */
int rewriteAppendOnlyFileBackground(void)
{
    pid_t childpid;
    long long start;

    // ��̨��д����ִ��
    if (server.aof_child_pid != -1) return REDIS_ERR;

    // ��ʼʱ��
    start = ustime();
    if ((childpid = fork()) == 0)
    {
        char tmpfile[256];

        /* Child */
        // �ر���������
        if (server.ipfd > 0) close(server.ipfd);
        if (server.sofd > 0) close(server.sofd);

        // ������ʱ�ļ�
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        // ��д
        if (rewriteAppendOnlyFile(tmpfile) == REDIS_OK)
        {
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty)
            {
                redisLog(REDIS_NOTICE,
                         "AOF rewrite: %lu MB of memory used by copy-on-write",
                         private_dirty/(1024*1024));
            }
            // �򸸽��̷����ź�, exitFromChild ������ redis.c
            exitFromChild(0);
        }
        else
        {
            exitFromChild(1);
        }
    }
    else
    {
        /* Parent */
        server.stat_fork_time = ustime()-start;

        // ��������ӽ���ʧ�ܣ�ֱ�ӷ���
        if (childpid == -1)
        {
            redisLog(REDIS_WARNING,
                     "Can't rewrite append only file in background: fork: %s",
                     strerror(errno));
            return REDIS_ERR;
        }

        // ����ͻ��ˣ���̨��д���ڽ���
        redisLog(REDIS_NOTICE,
                 "Background append only file rewriting started by pid %d",childpid);

        // ���·�����״̬
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);
        server.aof_child_pid = childpid;
        // �ر� key space �� rehash ������дʱ����
        updateDictResizePolicy();
        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.aof_rewrite_buf will start
         * with a SELECT statement and it will be safe to merge. */
        server.aof_selected_db = -1;
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

/*
 * BGREWRITEAOF �����ʵ��
 */
void bgrewriteaofCommand(redisClient *c)
{
    if (server.aof_child_pid != -1)
    {
        addReplyError(c,"Background append only file rewriting already in progress");
    }
    else if (server.rdb_child_pid != -1)
    {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    }
    else if (rewriteAppendOnlyFileBackground() == REDIS_OK)
    {
        addReplyStatus(c,"Background append only file rewriting started");
    }
    else
    {
        addReply(c,shared.err);
    }
}

/*
 * �Ƴ���ʱ�ļ�
 */
void aofRemoveTempFile(pid_t childpid)
{
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    unlink(tmpfile);
}

/* Update the server.aof_current_size filed explicitly using stat(2)
 * to check the size of the file. This is useful after a rewrite or after
 * a restart, normally the size is updated just adding the write length
 * to the current length, that is much faster. */
/*
 * ���� aof �ļ��Ĵ�С������ aof ����Ĵ�С
 */
void aofUpdateCurrentSize(void)
{
    struct redis_stat sb;

    if (redis_fstat(server.aof_fd,&sb) == -1)
    {
        redisLog(REDIS_WARNING,"Unable to obtain the AOF file length. stat: %s",
                 strerror(errno));
    }
    else
    {
        server.aof_current_size = sb.st_size;
    }
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int exitcode, int bysignal)
{
    if (!bysignal && exitcode == 0)
    {
        int newfd, oldfd;
        char tmpfile[256];
        long long now = ustime();

        redisLog(REDIS_NOTICE,
                 "Background AOF rewrite terminated with success");

        /* Flush the differences accumulated by the parent to the
         * rewritten AOF. */
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
                 (int)server.aof_child_pid);
        newfd = open(tmpfile,O_WRONLY|O_APPEND);
        if (newfd == -1)
        {
            redisLog(REDIS_WARNING,
                     "Unable to open the temporary AOF produced by the child: %s", strerror(errno));
            goto cleanup;
        }

        // ����д AOF �ļ�ʱ������Ļ���д�뵽��ʱ�ļ�
        if (aofRewriteBufferWrite(newfd) == -1)
        {
            redisLog(REDIS_WARNING,
                     "Error trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
            close(newfd);
            goto cleanup;
        }

        redisLog(REDIS_NOTICE,
                 "Parent diff successfully flushed to the rewritten AOF (%lu bytes)", aofRewriteBufferSize());

        /* The only remaining thing to do is to rename the temporary file to
         * the configured file and switch the file descriptor used to do AOF
         * writes. We don't want close(2) or rename(2) calls to block the
         * server on old file deletion.
         *
         * There are two possible scenarios:
         *
         * 1) AOF is DISABLED and this was a one time rewrite. The temporary
         * file will be renamed to the configured file. When this file already
         * exists, it will be unlinked, which may block the server.
         *
         * 2) AOF is ENABLED and the rewritten AOF will immediately start
         * receiving writes. After the temporary file is renamed to the
         * configured file, the original AOF file descriptor will be closed.
         * Since this will be the last reference to that file, closing it
         * causes the underlying file to be unlinked, which may block the
         * server.
         *
         * To mitigate the blocking effect of the unlink operation (either
         * caused by rename(2) in scenario 1, or by close(2) in scenario 2), we
         * use a background thread to take care of this. First, we
         * make scenario 1 identical to scenario 2 by opening the target file
         * when it exists. The unlink operation after the rename(2) will then
         * be executed upon calling close(2) for its descriptor. Everything to
         * guarantee atomicity for this switch has already happened by then, so
         * we don't care what the outcome or duration of that close operation
         * is, as long as the file descriptor is released again. */
        // ���������ѡ�����ιرվ� AOF �ļ��� fd ��
        // ������ AOF �ļ��� fd ����������
        if (server.aof_fd == -1)
        {
            /* AOF disabled */

            /* Don't care if this fails: oldfd will be -1 and we handle that.
             * One notable case of -1 return is if the old file does
             * not exist. */
            oldfd = open(server.aof_filename,O_RDONLY|O_NONBLOCK);
        }
        else
        {
            /* AOF enabled */
            oldfd = -1; /* We'll set this to the current AOF filedes later. */
        }

        /* Rename the temporary file. This will not unlink the target file if
         * it exists, because we reference it with "oldfd". */
        // ����� aof �ļ����ڣ�ͨ������ aof �ļ�������������
        if (rename(tmpfile,server.aof_filename) == -1)
        {
            redisLog(REDIS_WARNING,
                     "Error trying to rename the temporary AOF file: %s", strerror(errno));
            close(newfd);
            if (oldfd != -1) close(oldfd);
            goto cleanup;
        }

        // ���·������� aof_fd �������ݾɵ� fd ���������� fsync �� close �����Ĳ���
        if (server.aof_fd == -1)
        {
            /* AOF disabled, we don't need to set the AOF file descriptor
             * to this new file, so we can close it. */
            close(newfd);
        }
        else
        {
            /* AOF enabled, replace the old fd with the new one. */
            oldfd = server.aof_fd;
            server.aof_fd = newfd;

            // ����һ�� fsync ��ȷ���� AOF �ļ��ѱ��浽����
            if (server.aof_fsync == AOF_FSYNC_ALWAYS)
                aof_fsync(newfd);
            else if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
                aof_background_fsync(newfd);
            server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
            aofUpdateCurrentSize();
            server.aof_rewrite_base_size = server.aof_current_size;

            /* Clear regular AOF buffer since its contents was just written to
             * the new AOF from the background rewrite buffer. */
            // ������õ� buf ���ⲿ�ֵ������Ѿ�׷�ӵ� AOF �ļ����ˣ�
            sdsfree(server.aof_buf);
            server.aof_buf = sdsempty();
        }

        server.aof_lastbgrewrite_status = REDIS_OK;

        redisLog(REDIS_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed */
        if (server.aof_state == REDIS_AOF_WAIT_REWRITE)
            server.aof_state = REDIS_AOF_ON;

        /* Asynchronously close the overwritten AOF. */
        if (oldfd != -1) bioCreateBackgroundJob(REDIS_BIO_CLOSE_FILE,(void*)(long)oldfd,NULL,NULL);

        redisLog(REDIS_VERBOSE,
                 "Background AOF rewrite signal handler took %lldus", ustime()-now);
    }
    else if (!bysignal && exitcode != 0)
    {
        server.aof_lastbgrewrite_status = REDIS_ERR;

        redisLog(REDIS_WARNING,
                 "Background AOF rewrite terminated with error");
    }
    else
    {
        server.aof_lastbgrewrite_status = REDIS_ERR;

        redisLog(REDIS_WARNING,
                 "Background AOF rewrite terminated by signal %d", bysignal);
    }

cleanup:
    aofRewriteBufferReset();
    aofRemoveTempFile(server.aof_child_pid);
    server.aof_child_pid = -1;
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. */
    if (server.aof_state == REDIS_AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
