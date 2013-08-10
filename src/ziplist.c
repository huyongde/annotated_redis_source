/* The ziplist is a specially encoded dually linked list that is designed
 * to be very memory efficient. It stores both strings and integer values,
 * where integers are encoded as actual integers instead of a series of
 * characters. It allows push and pop operations on either side of the list
 * in O(1) time. However, because every operation requires a reallocation of
 * the memory used by the ziplist, the actual complexity is related to the
 * amount of memory used by the ziplist.
 *
 * Ziplist ��Ϊ�ڴ�ռ�ö��ر��Ż���˫����
 *
 * �����Ա����ַ����������������������������Ͷ������ַ��������б���ͱ��档
 *
 * �� ziplist �����˽��� push �� pop �ĸ��Ӷȶ�Ϊ O(1) ��
 * ��������Ϊ�� ziplist ��ÿ���޸Ĳ�������Ҫ�����ڴ��ط��䣬
 * ��ˣ�ʵ�ʵ�ʱ�临�Ӷ��� ziplist ʹ�õ��ڴ��С�йء�
 *
 * ----------------------------------------------------------------------------
 *
 * ZIPLIST OVERALL LAYOUT:
 *
 * The general layout of the ziplist is as follows:
 *
 * ������ ziplist ���ڴ�ṹ��
 *
 * <zlbytes><zltail><zllen><entry><entry><zlend>
 *
 * <zlbytes> is an unsigned integer to hold the number of bytes that the
 * ziplist occupies. This value needs to be stored to be able to resize the
 * entire structure without the need to traverse it first.
 *
 * <zlbytes> ��һ���޷�������(uint32_t)�����ڼ�¼���� ziplist ��ռ�õ��ֽ�������
 * ͨ���������ֵ�������ڲ��������� ziplist ��ǰ���£������� ziplist �����ڴ��ط��䡣
 *
 * <zltail> is the offset to the last entry in the list. This allows a pop
 * operation on the far side of the list without the need for full traversal.
 *
 * <zltail> �ǵ��б������һ���ڵ��ƫ����(ͬ��Ϊ uint32_t)��
 * �������ƫ�������Ϳ����ڳ������Ӷ��ڶԱ�β���в����������ر��������б�
 *
 * <zllen> is the number of entries.When this value is larger than 2**16-2,
 * we need to traverse the entire list to know how many items it holds.
 *
 * <zllen> �ǽڵ��������Ϊ ``uint16_t`` ��
 * �����ֵ���� 2**16-2 ʱ����Ҫ���������б����ܼ�����б�ĳ���
 *
 * <zlend> is a single byte special value, equal to 255, which indicates the
 * end of the list.
 *
 * <zlend> ��һ�����ֽڵ�����ֵ������ 255 ������ʶ���б��ĩ�ˡ�
 *
 * ZIPLIST ENTRIES:
 *
 * Every entry in the ziplist is prefixed by a header that contains two pieces
 * of information. First, the length of the previous entry is stored to be
 * able to traverse the list from back to front. Second, the encoding with an
 * optional string length of the entry itself is stored.
 *
 * Ziplist �е�ÿ���ڵ㣬������һ�� header ��Ϊǰ׺��
 *
 * Header ���������֣�
 * 1) ǰһ���ڵ�ĳ��ȣ��ڴӺ���ǰ����ʱʹ��
 * 2) ��ǰ�ڵ��������ֵ�����ͺͳ���
 *
 * The length of the previous entry is encoded in the following way:
 * If this length is smaller than 254 bytes, it will only consume a single
 * byte that takes the length as value. When the length is greater than or
 * equal to 254, it will consume 5 bytes. The first byte is set to 254 to
 * indicate a larger value is following. The remaining 4 bytes take the
 * length of the previous entry as value.
 *
 * ǰһ���ڵ�ĳ��ȵĴ��淽ʽ���£�
 * 1) ����ڵ�ĳ��� < 254 �ֽڣ���ôֱ����һ���ֽڱ������ֵ��
 * 2) ����ڵ�ĳ��� >= 254 �ֽڣ���ô����һ���ֽ�����Ϊ 254 ��
 *    ����֮���� 4 ���ֽ�����ʾ�ڵ��ʵ�ʳ��ȣ���ʹ�� 5 ���ֽڣ���
 *
 * The other header field of the entry itself depends on the contents of the
 * entry. When the entry is a string, the first 2 bits of this header will hold
 * the type of encoding used to store the length of the string, followed by the
 * actual length of the string. When the entry is an integer the first 2 bits
 * are both set to 1. The following 2 bits are used to specify what kind of
 * integer will be stored after this header. An overview of the different
 * types and encodings is as follows:
 *
 * ��һ�� header �򱣴����Ϣȡ��������ڵ�����������ݱ���
 *
 * ���ڵ㱣������ַ���ʱ��header ��ǰ 2 λ����ָʾ�������ݳ�����ʹ�õı��뷽ʽ��
 * ֮����ŵ������ݳ��ȵ�ֵ��
 *
 * ���ڵ㱣���������ʱ��header ��ǰ 2 λ������Ϊ 1 ��
 * ֮��� 2 λ����ָʾ���������ֵ�����ͣ�������;�����������ռ�õĿռ䣩��
 *
 * �����ǲ�ͬ���� header �ĸ�����
 *
 * |00pppppp| - 1 byte
 *      String value with length less than or equal to 63 bytes (6 bits).
 *      ���� <= 63 �ֽ�(6 λ)���ַ���ֵ
 * |01pppppp|qqqqqqqq| - 2 bytes
 *      String value with length less than or equal to 16383 bytes (14 bits).
 *      ���� <= 16383 �ֽ�(14 λ)���ַ���ֵ
 * |10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
 *      String value with length greater than or equal to 16384 bytes.
 *      ���� >= 16384 �ֽڣ� <= 4294967295 ���ַ���ֵ��
 *
 * |11000000| - 1 byte
 *      Integer encoded as int16_t (2 bytes).
 *      �� int16_t (2 �ֽ�)���ͱ��������
 * |11010000| - 1 byte
 *      Integer encoded as int32_t (4 bytes).
 *      �� int32_t (4 �ֽ�)���ͱ��������
 * |11100000| - 1 byte
 *      Integer encoded as int64_t (8 bytes).
 *      �� int64_t (8 �ֽ�)���ͱ��������
 * |11110000| - 1 byte
 *      Integer encoded as 24 bit signed (3 bytes).
 *      24 λ(3 �ֽ�)�з��ű�������
 * |11111110| - 1 byte
 *      Integer encoded as 8 bit signed (1 byte).
 *      8 λ(1 �ֽ�)�з��ű�������
 * |1111xxxx| - (with xxxx between 0000 and 1101) immediate 4 bit integer.
 *      Unsigned integer from 0 to 12. The encoded value is actually from
 *      1 to 13 because 0000 and 1111 can not be used, so 1 should be
 *      subtracted from the encoded 4 bit value to obtain the right value.
 *      (���� 0000 �� 1101 ֮��)�� 4 λ�����������ڱ�ʾ�޷������� 0 �� 12 ��
 *      ��Ϊ 0000 �� 1111 ���Ѿ���ռ�ã���ˣ��ɱ������ֵʵ����ֻ���� 1 �� 13 ��
 *      Ҫ�����ֵ��ȥ 1 �����ܵõ���ȷ��ֵ��
 * |11111111| - End of ziplist.
 *      ziplist ���ս��
 *
 * All the integers are represented in little endian byte order.
 *
 * ������������С�˱�ʾ��
 *
 * ----------------------------------------------------------------------------
 *
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
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"

#define ZIP_END 255
#define ZIP_BIGLEN 254

/* Different encoding/length possibilities */
#define ZIP_STR_MASK 0xc0   // 1100, 0000
#define ZIP_INT_MASK 0x30
#define ZIP_STR_06B (0 << 6)
#define ZIP_STR_14B (1 << 6)
#define ZIP_STR_32B (2 << 6)
#define ZIP_INT_16B (0xc0 | 0<<4)
#define ZIP_INT_32B (0xc0 | 1<<4)
#define ZIP_INT_64B (0xc0 | 2<<4)
#define ZIP_INT_24B (0xc0 | 3<<4)
#define ZIP_INT_8B 0xfe
/* 4 bit integer immediate encoding */
#define ZIP_INT_IMM_MASK 0x0f
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 */
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)

#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

/* Macro to determine type */
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK)

/*
 * ����ȡ�� zl ������ֵ�ĺ�
 *
 * ���к긴�Ӷȶ�Ϊ O(1)
 */
// ȡ���б����ֽڼ�����б���(�ڴ�� 0 - 31 λ������)
#define ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl)))
// ȡ���б�ı�βƫ����(�ڴ�� 32 - 63 λ������)
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))
// ȡ���б�ĳ���(�ڴ�� 64 - 79 λ������)
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))
// �б�� header ����
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))   // 32*2 bit + 16 bit
// �����б�� header ֮���λ��
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)
// �����б����һ��Ԫ��֮���λ��
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
// �����б�Ľ�����֮ǰ��λ��
#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)

/* We know a positive increment can only be 1 because entries can only be
 * pushed one at a time. */
/*
 * �� <zllen> ��һ
 *
 * ZIPLIST_LENGTH(zl) �����ֵΪ UINT16_MAX
 *
 * ���Ӷȣ�O(1)
 */
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) \
        ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr); \
}

/*
 * �ڵ�ṹ
 */
typedef struct zlentry
{
    unsigned int prevrawlensize,    // ����ǰһ�ڵ�ĳ�������ĳ���
             prevrawlen;        // ǰһ�ڵ�ĳ���
    unsigned int lensize,           // ����ڵ�ĳ�������ĳ���
             len;               // �ڵ�ĳ���
    unsigned int headersize;        // header ����
    unsigned char encoding;         // ���뷽ʽ
    unsigned char *p;               // ����
} zlentry;

/* Extract the encoding from the byte pointed by 'ptr' and set it into
 * 'encoding'. */
/*
 * �� ptr ָ����ȡ���������ͣ����������浽 encoding
 *
 * ���Ӷȣ�O(1)
 */
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {  \
    (encoding) = (ptr[0]); \
    if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while(0)

/*
 * ���� encoding ָ�����������뷽ʽ����ĳ���
 *
 * ���Ӷȣ�O(1)
 */
static unsigned int zipIntSize(unsigned char encoding)
{
    switch(encoding)
    {
    case ZIP_INT_8B:
        return 1;
    case ZIP_INT_16B:
        return 2;
    case ZIP_INT_24B:
        return 3;
    case ZIP_INT_32B:
        return 4;
    case ZIP_INT_64B:
        return 8;
    default:
        return 0; /* 4 bit immediate */
    }
    assert(NULL);
    return 0;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
/*
 * ���볤�� l ��������д�뵽 p ��
 *
 * ��� p Ϊ NULL ����ô���ر��� rawlen ������ֽ���
 *
 * ���Ӷȣ�O(1)
 */
static unsigned int zipEncodeLength(unsigned char *p, unsigned char encoding, unsigned int rawlen)
{
    unsigned char len = 1, buf[5];

    if (ZIP_IS_STR(encoding))
    {
        // �ַ�������
        /* Although encoding is given it may not be set for strings,
         * so we determine it here using the raw length. */
        if (rawlen <= 0x3f)
        {
            // ���� 6 bit ������ + ���� 8 bit (1 byte)
            if (!p) return len;
            buf[0] = ZIP_STR_06B | rawlen;
        }
        else if (rawlen <= 0x3fff)
        {
            // ���� 14 bit������ + ���� 16 bit (2 btyes)
            len += 1;
            if (!p) return len;
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
            buf[1] = rawlen & 0xff;
        }
        else
        {
            // ���� 32 bit (4 bytes)������ + ���� 40 bit (5 bytes)
            len += 4;
            if (!p) return len;
            buf[0] = ZIP_STR_32B;
            buf[1] = (rawlen >> 24) & 0xff;
            buf[2] = (rawlen >> 16) & 0xff;
            buf[3] = (rawlen >> 8) & 0xff;
            buf[4] = rawlen & 0xff;
        }
    }
    else
    {
        // ����Ϊ������������Ϊ 1 bytes
        /* Implies integer encoding, so length is always 1. */
        if (!p) return len;
        buf[0] = encoding;
    }

    /* Store this length at p */
    memcpy(p,buf,len);
    return len;
}

/* Decode the length encoded in 'ptr'. The 'encoding' variable will hold the
 * entries encoding, the 'lensize' variable will hold the number of bytes
 * required to encode the entries length, and the 'len' variable will hold the
 * entries length. */
/*
 * �� ptr ָ����ȡ���ڵ�ı��롢����ڵ㳤������ĳ��ȡ��Լ��ڵ�ĳ���
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��
 *  int ����ڵ�����ĳ���
 */
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                    \
    /* ȡ���ڵ�ı��� */                                                       \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                                     \
    if ((encoding) < ZIP_STR_MASK) {                                           \
        /* �ڵ㱣������ַ�����ȡ������ */                                     \
        if ((encoding) == ZIP_STR_06B) {                                       \
            (lensize) = 1;                                                     \
            (len) = (ptr)[0] & 0x3f;                                           \
        } else if ((encoding) == ZIP_STR_14B) {                                \
            (lensize) = 2;                                                     \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                       \
        } else if (encoding == ZIP_STR_32B) {                                  \
            (lensize) = 5;                                                     \
            (len) = ((ptr)[1] << 24) |                                         \
                    ((ptr)[2] << 16) |                                         \
                    ((ptr)[3] <<  8) |                                         \
                    ((ptr)[4]);                                                \
        } else {                                                               \
            assert(NULL);                                                      \
        }                                                                      \
    } else {                                                                   \
        /* �ڵ㱣�����������ȡ������ */                                       \
        (lensize) = 1;                                                         \
        (len) = zipIntSize(encoding);                                          \
    }                                                                          \
} while(0);

/* Encode the length of the previous entry and write it to "p". Return the
 * number of bytes needed to encode this length if "p" is NULL. */
/*
 * ����ǰ�ýڵ�ĳ��ȣ�������д�� p ��
 *
 * ��� p Ϊ NULL ����ô���ر��� len ������ֽ�����
 *
 * ���Ӷȣ�O(1)
 */
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len)
{
    if (p == NULL)
    {
        return (len < ZIP_BIGLEN) ? 1 : sizeof(len)+1;
    }
    else
    {
        if (len < ZIP_BIGLEN)
        {
            p[0] = len;
            return 1;
        }
        else
        {
            p[0] = ZIP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            memrev32ifbe(p+1);
            return 1+sizeof(len);
        }
    }
}

/* Encode the length of the previous entry and write it to "p". This only
 * uses the larger encoding (required in __ziplistCascadeUpdate). */
/*
 * ��ǰ���ڵ�ĳ��� len д�뵽 p ��
 * ���� p �Ŀռ�ȱ��� len �����ʵ�ʿռ�Ҫ����
 *
 * �������ڽ�һ����ԭ�ڵ���̵��½ڵ���뵽ĳ���ڵ�ʱʹ��
 * ����ο� __ziplistCascadeUpdate ������ͷע��
 *
 * ���Ӷȣ�O(1)
 */
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len)
{
    if (p == NULL) return;
    p[0] = ZIP_BIGLEN;
    memcpy(p+1,&len,sizeof(len));
    memrev32ifbe(p+1);
}

/* Decode the number of bytes required to store the length of the previous
 * element, from the perspective of the entry pointed to by 'ptr'. */
/*
 * ��ָ�� ptr ��ȡ������ǰһ���ڵ�ĳ���������ֽ���
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��
 *  unsigned int
 */
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                          \
    if ((ptr)[0] < ZIP_BIGLEN) {                                               \
        (prevlensize) = 1;                                                     \
    } else {                                                                   \
        (prevlensize) = 5;                                                     \
    }                                                                          \
} while(0);

/* Decode the length of the previous element, from the perspective of the entry
 * pointed to by 'ptr'. */
/*
* ��ָ�� ptr ��ȡ��ǰһ���ڵ�ĳ���
*
* ���Ӷȣ�O(1)
*
* ����ֵ��
*   unsigned int
*/
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                     \
    /* ȡ�ñ���ǰһ���ڵ�ĳ���������ֽ��� */                                 \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
    /* ��ȡ����ֵ */                                                           \
    if ((prevlensize) == 1) {                                                  \
        (prevlen) = (ptr)[0];                                                  \
    } else if ((prevlensize) == 5) {                                           \
        assert(sizeof((prevlensize)) == 4);                                    \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);                             \
        memrev32ifbe(&prevlen);                                                \
    }                                                                          \
} while(0);

/* Return the difference in number of bytes needed to store the length of the
 * previous element 'len', in the entry pointed to by 'p'. */
/*
 * ���ر��� len ����ĳ��ȼ�ȥ���� p ��ǰһ���ڵ�Ĵ�С����ĳ���֮��
 *
 * ���Ӷȣ�O(1)
 */
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len)
{
    // ��ȡ����ǰһ�ڵ�����ĳ���
    unsigned int prevlensize;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);
    // �����
    return zipPrevEncodeLength(NULL, len) - prevlensize;
}

/* Return the total number of bytes used by the entry pointed to by 'p'. */
/*
 * ���� p ָ��Ľڵ�Ŀռ��ܳ���
 *
 * ���Ӷȣ�O(1)
 */
static unsigned int zipRawEntryLength(unsigned char *p)
{
    unsigned int prevlensize, encoding, lensize, len;

    // ����ǰ���ڵ㳤�ȵĿռ䳤��
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);

    // ���汾�ڵ�Ŀռ䳤��
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);

    return prevlensize + lensize + len;
}

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'. */
/*
 * ��� entry �������ֵ�������ܷ����Ϊ����
 *
 * ���Ӷȣ�O(N)��N Ϊ entry �������ַ���ֵ�ĳ���
 *
 * ����ֵ��
 *  ������ԵĻ������� 1 ������ֵ������ v �������뱣���� encoding
 *  ���򣬷��� 0
 */
static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding)
{
    long long value;

    if (entrylen >= 32 || entrylen == 0) return 0;
    // ����ת��Ϊ����
    if (string2ll((char*)entry,entrylen,&value))
    {
        /* Great, the string can be encoded. Check what's the smallest
         * of our encoding types that can hold this value. */
        // ѡ����������
        if (value >= 0 && value <= 12)
        {
            *encoding = ZIP_INT_IMM_MIN+value;
        }
        else if (value >= INT8_MIN && value <= INT8_MAX)
        {
            *encoding = ZIP_INT_8B;
        }
        else if (value >= INT16_MIN && value <= INT16_MAX)
        {
            *encoding = ZIP_INT_16B;
        }
        else if (value >= INT24_MIN && value <= INT24_MAX)
        {
            *encoding = ZIP_INT_24B;
        }
        else if (value >= INT32_MIN && value <= INT32_MAX)
        {
            *encoding = ZIP_INT_32B;
        }
        else
        {
            *encoding = ZIP_INT_64B;
        }
        *v = value;
        return 1;
    }
    return 0;
}

/* Store integer 'value' at 'p', encoded as 'encoding' */
/*
 * �� value ���浽 p �������ñ���Ϊ encoding
 *
 * ���Ӷȣ�O(1)
 */
static void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding)
{
    int16_t i16;
    int32_t i32;
    int64_t i64;
    // 8 bit ����
    if (encoding == ZIP_INT_8B)
    {
        ((int8_t*)p)[0] = (int8_t)value;
        // 16 bit ����
    }
    else if (encoding == ZIP_INT_16B)
    {
        i16 = value;
        memcpy(p,&i16,sizeof(i16));
        memrev16ifbe(p);
        // 24 bit ����
    }
    else if (encoding == ZIP_INT_24B)
    {
        i32 = value<<8;
        memrev32ifbe(&i32);
        memcpy(p,((uint8_t*)&i32)+1,sizeof(i32)-sizeof(uint8_t));
        // 32 bit ����
    }
    else if (encoding == ZIP_INT_32B)
    {
        i32 = value;
        memcpy(p,&i32,sizeof(i32));
        memrev32ifbe(p);
        // 64 bit ����
    }
    else if (encoding == ZIP_INT_64B)
    {
        i64 = value;
        memcpy(p,&i64,sizeof(i64));
        memrev64ifbe(p);
        // ֵ�ͱ��뱣����ͬһ�� byte
    }
    else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
    {
        /* Nothing to do, the value is stored in the encoding itself. */
    }
    else
    {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p' */
/*
 * ���� encoding ����ָ�� p ��ȡ������ֵ
 *
 * ���Ӷȣ�O(1)
 */
static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding)
{
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;
    // 8 bit
    if (encoding == ZIP_INT_8B)
    {
        ret = ((int8_t*)p)[0];
        // 16 bit
    }
    else if (encoding == ZIP_INT_16B)
    {
        memcpy(&i16,p,sizeof(i16));
        memrev16ifbe(&i16);
        ret = i16;
        // 32 bit
    }
    else if (encoding == ZIP_INT_32B)
    {
        memcpy(&i32,p,sizeof(i32));
        memrev32ifbe(&i32);
        ret = i32;
        // 24 bit
    }
    else if (encoding == ZIP_INT_24B)
    {
        i32 = 0;
        memcpy(((uint8_t*)&i32)+1,p,sizeof(i32)-sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32>>8;
        // 64 bit
    }
    else if (encoding == ZIP_INT_64B)
    {
        memcpy(&i64,p,sizeof(i64));
        memrev64ifbe(&i64);
        ret = i64;
        // ֵ�ͱ��뱣����ͬһ�� byte
    }
    else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
    {
        ret = (encoding & ZIP_INT_IMM_MASK)-1;
    }
    else
    {
        assert(NULL);
    }
    return ret;
}

/* Return a struct with all information about an entry. */
/*
 * ��ָ�� p ����ȡ���ڵ�ĸ������ԣ��������Ա��浽 zlentry �ṹ��Ȼ�󷵻�
 *
 * ���Ӷȣ�O(1)
 */
static zlentry zipEntry(unsigned char *p)
{
    zlentry e;

    // ȡ��ǰһ���ڵ�ĳ���
    ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);

    // ȡ����ǰ�ڵ�ı��롢����ڵ�ĳ�������ĳ��ȡ��Լ��ڵ�ĳ���
    ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);

    // ��¼ header �ĳ���
    e.headersize = e.prevrawlensize + e.lensize;

    // ��¼ָ�� p
    e.p = p;

    return e;
}

/*
 * �´���һ���� ziplist
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ���´����� ziplist
 */
unsigned char *ziplistNew(void)
{
    // ���� 2 �� 32 bit��һ�� 16 bit���Լ�һ�� 8 bit
    // �ֱ����� <zlbytes><zltail><zllen> �� <zlend>
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;
    unsigned char *zl = zmalloc(bytes);

    // ���ó���
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);

    // ���ñ�βƫ����
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);

    // �����б�������
    ZIPLIST_LENGTH(zl) = 0;

    // ���ñ�β��ʶ
    zl[bytes-1] = ZIP_END;

    return zl;
}

/* Resize the ziplist. */
/*
 * �� zl ���пռ��ط��䣬�������������
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ�����º�� ziplist
 */
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len)
{
    // �ط���
    zl = zrealloc(zl,len);
    // ���³���
    ZIPLIST_BYTES(zl) = intrev32ifbe(len);
    // ���ñ�β
    zl[len-1] = ZIP_END;

    return zl;
}

/* When an entry is inserted, we need to set the prevlen field of the next
 * entry to equal the length of the inserted entry. It can occur that this
 * length cannot be encoded in 1 byte and the next entry needs to be grow
 * a bit larger to hold the 5-byte encoded prevlen. This can be done for free,
 * because this only happens when an entry is already being inserted (which
 * causes a realloc and memmove). However, encoding the prevlen may require
 * that this entry is grown as well. This effect may cascade throughout
 * the ziplist when there are consecutive entries with a size close to
 * ZIP_BIGLEN, so we need to check that the prevlen can be encoded in every
 * consecutive entry.
 *
 * Note that this effect can also happen in reverse, where the bytes required
 * to encode the prevlen field can shrink. This effect is deliberately ignored,
 * because it can cause a "flapping" effect where a chain prevlen fields is
 * first grown and then shrunk again after consecutive inserts. Rather, the
 * field is allowed to stay larger than necessary, because a large prevlen
 * field implies the ziplist is holding large entries anyway.
 *
 * The pointer "p" points to the first entry that does NOT need to be
 * updated, i.e. consecutive fields MAY need an update. */
/*
 * ����һ���½ڵ���ӵ�ĳ���ڵ�֮ǰ��ʱ��
 * ���ԭ�ڵ�� prevlen �����Ա����½ڵ�ĳ��ȣ�
 * ��ô����Ҫ��ԭ�ڵ�Ŀռ������չ���� 1 �ֽ���չ�� 5 �ֽڣ���
 *
 * ���ǣ�����ԭ�ڵ������չ֮��ԭ�ڵ����һ���ڵ�� prevlen ���ܳ��ֿռ䲻�㣬
 * ��������ڶ�������ڵ�ĳ��ȶ��ӽ� ZIP_BIGLEN ʱ���ܷ�����
 *
 * ������������ڴ�������������չ������
 *
 * ��Ϊ�ڵ�ĳ��ȱ�С�������������СҲ�ǿ��ܳ��ֵģ�
 * ������Ϊ�˱�����չ-��С-��չ-��С����������������֣�flapping����������
 * ���ǲ���������������������� prevlen ������ĳ��ȸ���
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ�����º�� ziplist
 */
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p)
{
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;
    size_t offset, noffset, extra;
    unsigned char *np;
    zlentry cur, next;

    // һֱ����ֱ����β
    while (p[0] != ZIP_END)
    {
        // ��ǰ�ڵ�
        cur = zipEntry(p);
        // ��ǰ�ڵ�ĳ���
        rawlen = cur.headersize + cur.len;
        // ���뵱ǰ�ڵ�ĳ�������Ŀռ��С
        rawlensize = zipPrevEncodeLength(NULL,rawlen);

        /* Abort if there is no next entry. */
        // �Ѿ������β���˳�
        if (p[rawlen] == ZIP_END) break;
        // ȡ����һ�ڵ�
        next = zipEntry(p+rawlen);

        /* Abort when "prevlen" has not changed. */
        // �����һ�� prevlen ���ڵ�ǰ�ڵ�� rawlen
        // ��ô˵�������С����ı䣬�˳�
        if (next.prevrawlen == rawlen) break;

        // ��һ�ڵ�ĳ��ȱ���ռ䲻�㣬������չ
        if (next.prevrawlensize < rawlensize)
        {
            /* The "prevlen" field of "next" needs more bytes to hold
             * the raw length of "cur". */
            offset = p-zl;
            // ��Ҫ����ӵĳ���
            extra = rawlensize-next.prevrawlensize;
            // �ط��䣬���Ӷ�Ϊ O(N)
            zl = ziplistResize(zl,curlen+extra);
            p = zl+offset;

            /* Current pointer and offset for next element. */
            np = p+rawlen;
            noffset = np-zl;

            /* Update tail offset when next element is not the tail element. */
            if ((zl+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np)
            {
                ZIPLIST_TAIL_OFFSET(zl) =
                    intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+extra);
            }

            /* Move the tail to the back. */
            // Ϊ��ÿռ�����������ƶ������Ӷ���� O(N)
            memmove(np+rawlensize,
                    np+next.prevrawlensize,
                    curlen-noffset-next.prevrawlensize-1);
            zipPrevEncodeLength(np,rawlen);

            /* Advance the cursor */
            p += rawlen;
            curlen += extra;
        }
        else
        {
            // ��һ�ڵ�ĳ��ȱ���ռ��ж��࣬����������
            // ֻ�ǽ�������ĳ���д��ռ�
            if (next.prevrawlensize > rawlensize)
            {
                /* This would result in shrinking, which we want to avoid.
                 * So, set "rawlen" in the available bytes. */
                zipPrevEncodeLengthForceLarge(p+rawlen,rawlen);
            }
            else
            {
                zipPrevEncodeLength(p+rawlen,rawlen);
            }

            // next.prevrawlensize == rawlensize
            /* Stop here, as the raw length of "next" has not changed. */
            break;
        }
    }
    return zl;
}

/* Delete "num" entries, starting at "p". Returns pointer to the ziplist. */
/*
 * ��ָ�� p ��ʼ��ɾ�� num ���ڵ�
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ��ɾ��Ԫ�غ�� ziplist
 */
static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num)
{
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    // �׸��ڵ�
    first = zipEntry(p);
    // �ۻ�������ɾ��Ŀ�꣨�ڵ㣩�ı��볤��
    // ���ƶ�ָ�� p
    // ���Ӷ� O(N)
    for (i = 0; p[0] != ZIP_END && i < num; i++)
    {
        p += zipRawEntryLength(p);
        deleted++;
    }

    // ��ɾ���Ľڵ�� byte �ܺ�
    totlen = p-first.p;
    if (totlen > 0)
    {
        if (p[0] != ZIP_END)
        {
            /* Storing `prevrawlen` in this entry may increase or decrease the
             * number of bytes required compare to the current `prevrawlen`.
             * There always is room to store this, because it was previously
             * stored by an entry that is now being deleted. */
            // �������һ����ɾ���Ľڵ�֮���һ���ڵ㣬
            // ������ prevlan ֵ����Ϊ first.prevrawlen ��
            // Ҳ���Ǳ�ɾ���ĵ�һ���ڵ��ǰһ���ڵ�ĳ���
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);
            p -= nextdiff;
            zipPrevEncodeLength(p,first.prevrawlen);

            /* Update offset for tail */
            // ���� ziplist ����β��ƫ����
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);

            /* When the tail contains more than one entry, we need to take
             * "nextdiff" in account as well. Otherwise, a change in the
             * size of prevlen doesn't have an effect on the *tail* offset. */
            // ���� ziplist ��ƫ�������������Ҫ�Ļ������� nextdiff
            tail = zipEntry(p);
            if (p[tail.headersize+tail.len] != ZIP_END)
            {
                ZIPLIST_TAIL_OFFSET(zl) =
                    intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
            }

            /* Move tail to the front of the ziplist */
            // ǰ���ڴ��е����ݣ�����ԭ���ı�ɾ������
            // ���Ӷ� O(N)
            memmove(first.p,p,
                    intrev32ifbe(ZIPLIST_BYTES(zl))-(p-zl)-1);
        }
        else
        {
            /* The entire tail was deleted. No need to move memory. */
            // ��ɾ������β�ڵ㣬�����ڴ��ƶ���ֱ�Ӹ���ƫ��ֵ�Ϳ�����
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe((first.p-zl)-first.prevrawlen);
        }

        /* Resize and update length */
        // ������С�������� ziplist �ĳ���
        // ���Ӷ� O(N)
        offset = first.p-zl;
        zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl))-totlen+nextdiff);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
        p = zl+offset;

        /* When nextdiff != 0, the raw length of the next entry has changed, so
         * we need to cascade the update throughout the ziplist */
        // �㼶����
        if (nextdiff != 0)
            // ���Ӷ� O(N^2)
            zl = __ziplistCascadeUpdate(zl,p);
    }
    return zl;
}

/* Insert item at "p". */
/*
 * ��ӱ������Ԫ�� s ���½ڵ㵽��ַ p
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ��ɾ��Ԫ�غ�� ziplist
 */
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen)
{
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)),
           reqlen,
           prevlen = 0;
    size_t offset;
    int nextdiff = 0;
    unsigned char encoding = 0;
    long long value = 123456789; /* initialized to avoid warning. Using a value
                                    that is easy to see if for some reason
                                    we use it uninitialized. */
    zlentry entry, tail;

    /* Find out prevlen for the entry that is inserted. */
    // ��� p ֮����û�нڵ㣨���ǲ��뵽ĩ�ˣ�
    // ��ôȡ���ڵ�������ϣ��Լ� prevlen
    if (p[0] != ZIP_END)
    {
        entry = zipEntry(p);
        prevlen = entry.prevrawlen;
    }
    else
    {
        // ��ȡ�б����һ���ڵ㣨��β���ĵ�ַ
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
        // �����ַ֮����ĩ�ˣ�Ҳ���ǣ��б�������һ���ڵ㣩
        if (ptail[0] != ZIP_END)
        {
            // ���� ptail ָ��Ľڵ�Ŀռ䳤��
            prevlen = zipRawEntryLength(ptail);
        }
    }

    /* See if the entry can be encoded */
    // �鿴�ܷ���ֵ����Ϊ����
    // ������ԵĻ����� 1 ��
    // ������ֵ���浽 value ��������ʽ���浽 encoding
    if (zipTryEncoding(s,slen,&value,&encoding))
    {
        /* 'encoding' is set to the appropriate integer encoding */
        // s ���Ա���Ϊ��������ô�������㱣��������Ŀռ�
        reqlen = zipIntSize(encoding);
    }
    else
    {
        /* 'encoding' is untouched, however zipEncodeLength will use the
         * string length to figure out how to encode it. */
        // ���ܱ���Ϊ������ֱ��ʹ���ַ�������
        reqlen = slen;
    }
    /* We need space for both the length of the previous entry and
     * the length of the payload. */
    // ������� prevlen ����ĳ���
    reqlen += zipPrevEncodeLength(NULL,prevlen);
    // ������� slen ����ĳ���
    reqlen += zipEncodeLength(NULL,encoding,slen);

    /* When the insert position is not equal to the tail, we need to
     * make sure that the next entry can hold this entry's length in
     * its prevlen field. */
    // �����ӵ�λ�ò��Ǳ�β����ô����ȷ����̽ڵ�� prevlen �ռ�
    // ���Ա����½ڵ�ı��볤��
    // zipPrevLenByteDiff �ķ���ֵ�����ֿ��ܣ�
    // 1���¾������ڵ�ı��볤����ȣ����� 0
    // 2���½ڵ���볤�� > �ɽڵ���볤�ȣ����� 5 - 1 = 4
    // 3���ɽڵ���볤�� > �±���ڵ㳤�ȣ����� 1 - 5 = -4
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0;

    /* Store offset because a realloc may change the address of zl. */
    // ����ƫ��������Ϊ�ط���ռ��п��ܸı� zl ���ڴ��ַ
    offset = p-zl;
    // �ط���ռ䣬�����³������Ժͱ�β
    // �¿ռ䳤�� = ���г��� + �½ڵ����賤�� + �����½ڵ㳤������ĳ��Ȳ�
    // O(N)
    zl = ziplistResize(zl,curlen+reqlen+nextdiff);
    // ���� p ��ָ��
    p = zl+offset;

    /* Apply memory move when necessary and update tail offset. */
    // ����½ڵ㲻����ӵ��б�ĩ�ˣ���ô��������������ڵ�
    // ��ˣ�������Ҫ�ƶ��ⲿ�ֽڵ�
    if (p[0] != ZIP_END)
    {
        /* Subtract one because of the ZIP_END bytes */
        // �����ƶ���ԭ�����ݣ�Ϊ�½ڵ��ó��ռ�
        // O(N)
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff);

        /* Encode this entry's raw length in the next entry. */
        // �����ڵ�ĳ��ȱ�������һ�ڵ�
        zipPrevEncodeLength(p+reqlen,reqlen);

        /* Update offset for tail */
        // ���� ziplist �ı�βƫ����
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);

        /* When the tail contains more than one entry, we need to take
         * "nextdiff" in account as well. Otherwise, a change in the
         * size of prevlen doesn't have an effect on the *tail* offset. */
        // ����Ҫ�Ļ����� nextdiff Ҳ���ϵ� zltail ��
        tail = zipEntry(p+reqlen);
        if (p[reqlen+tail.headersize+tail.len] != ZIP_END)
        {
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
        }
    }
    else
    {
        /* This element will be the new tail. */
        // ���� ziplist �� zltail ���ԣ���������ӽڵ�Ϊ��β�ڵ�
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p-zl);
    }

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade the update throughout the ziplist */
    if (nextdiff != 0)
    {
        offset = p-zl;
        // O(N^2)
        zl = __ziplistCascadeUpdate(zl,p+reqlen);
        p = zl+offset;
    }

    /* Write the entry */
    // д�����ݵ��ڵ�

    // ������һ�ڵ�ĳ��ȣ�������ƶ�ָ��
    p += zipPrevEncodeLength(p,prevlen);
    // ���뱾�ڵ�ĳ��Ⱥ����ͣ�������ƶ�ָ��
    p += zipEncodeLength(p,encoding,slen);
    // д�����ݵ��ڵ�
    if (ZIP_IS_STR(encoding))
    {
        memcpy(p,s,slen);
    }
    else
    {
        zipSaveInteger(p,value,encoding);
    }
    // ���½ڵ�����
    ZIPLIST_INCR_LENGTH(zl,1);

    return zl;
}

/*
 * ����Ԫ�ز���Ϊ�б�ı�ͷ�ڵ���߱�β�ڵ�
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ����Ӳ�����ɺ�� ziplist
 */
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where)
{
    unsigned char *p;
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    return __ziplistInsert(zl,p,s,slen);
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
/*
 * ����ָ��ǰ�����ڵ��ָ�룬���� ziplistNext ���е���
 * ��ƫ��ֵ�Ǹ���ʱ����ʾ�����Ǵӱ�β����ͷ���еġ�
 * ��Ԫ�ر�������ʱ������ NULL
 *
 * ���Ӷȣ�O(1)
 *
 * ����ֵ��ָ��ڵ��ָ��
 */
unsigned char *ziplistIndex(unsigned char *zl, int index)
{

    unsigned char *p;

    zlentry entry;

    // ��ǰ����
    if (index < 0)
    {
        index = (-index)-1;
        p = ZIPLIST_ENTRY_TAIL(zl);
        // ��� ziplist ��Ϊ�ա�����
        if (p[0] != ZIP_END)
        {
            // ��ô���� entry.prevrawlen ��������ǰ����
            entry = zipEntry(p);
            while (entry.prevrawlen > 0 && index--)
            {
                // ���˵�ַ
                p -= entry.prevrawlen;
                entry = zipEntry(p);
            }
        }
        // ������
    }
    else
    {
        p = ZIPLIST_ENTRY_HEAD(zl);
        // ���� entry.prevrawlen �����е���
        while (p[0] != ZIP_END && index--)
        {
            p += zipRawEntryLength(p);
        }
    }

    return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/* Return pointer to next entry in ziplist.
 *
 * zl is the pointer to the ziplist
 * p is the pointer to the current element
 *
 * The element after 'p' is returned, otherwise NULL if we are at the end. */
/*
 * ����ָ�� p ����һ���ڵ��ָ�룬
 * ��� p �Ѿ������β����ô���� NULL ��
 *
 * ���Ӷȣ�O(1)
 */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p)
{
    ((void) zl);

    /* "p" could be equal to ZIP_END, caused by ziplistDelete,
     * and we should return NULL. Otherwise, we should return NULL
     * when the *next* element is ZIP_END (there is no next entry). */
    if (p[0] == ZIP_END)
    {
        return NULL;
    }

    // ָ����һ�ڵ㣬O(1)
    p += zipRawEntryLength(p);
    if (p[0] == ZIP_END)
    {
        return NULL;
    }

    return p;
}

/* Return pointer to previous entry in ziplist. */
/*
 * ���� p ��ǰһ���ڵ�
 *
 * ���Ӷȣ�O(1)
 */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p)
{
    zlentry entry;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    // ���Ǳ�β
    if (p[0] == ZIP_END)
    {
        p = ZIPLIST_ENTRY_TAIL(zl);
        return (p[0] == ZIP_END) ? NULL : p;
        // �����ͷ��ֹͣ
    }
    else if (p == ZIPLIST_ENTRY_HEAD(zl))
    {
        return NULL;
    }
    else
    {
        entry = zipEntry(p);
        assert(entry.prevrawlen > 0);
        return p-entry.prevrawlen;
    }
}

/* Get entry pointer to by 'p' and store in either 'e' or 'v' depending
 * on the encoding of the entry. 'e' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the zipmap, 1 otherwise. */
/*
 * ��ȡ p ��ָ��Ľڵ㣬����������Ա�����ָ��
 *
 * ����ڵ㱣������ַ���ֵ����ô�� sstr ָ��ָ������
 * slen ����Ϊ�ַ����ĳ��ȡ�
 *
 * ����ڵ㱣���������ֵ����ô�� sval ��������
 *
 * p Ϊ��βʱ���� 0 �����򷵻� 1 ��
 *
 * ���Ӷȣ�O(1)
 */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval)
{
    zlentry entry;
    // ��β
    if (p == NULL || p[0] == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    // ��ȡ�ڵ�
    entry = zipEntry(p);
    // �ַ���
    if (ZIP_IS_STR(entry.encoding))
    {
        if (sstr)
        {
            *slen = entry.len;
            *sstr = p+entry.headersize;
        }
        // ����ֵ
    }
    else
    {
        if (sval)
        {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);
        }
    }
    return 1;
}

/* Insert an entry at "p". */
/*
 * ���ڵ���뵽 p ֮��
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ�������ɺ�� ziplist
 */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen)
{
    return __ziplistInsert(zl,p,s,slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. */
/*
 * ɾ�� p ��ָ��Ľڵ㣬
 * ��ԭ�ظ��� p ָ�룬����ָ��ɾ���ڵ�ĺ�һ���ڵ㣬
 * ʹ�ÿ��Ե����ؽ���ɾ��
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ��ɾ����ɺ�� ziplist
 */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p)
{
    size_t offset = *p-zl;
    zl = __ziplistDelete(zl,*p,1);

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl+offset;
    return zl;
}

/* Delete a range of entries from the ziplist. */
/*
 * ��λ������ index ����ɾ����֮��� num ��Ԫ��
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ��ɾ����ɺ�� ziplist
 */
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num)
{
    unsigned char *p = ziplistIndex(zl,index);
    return (p == NULL) ? zl : __ziplistDelete(zl,p,num);
}

/* Compare entry pointer to by 'p' with 'entry'. Return 1 if equal. */
/*
 * �� p ��ָ��Ľڵ�����Ժ� sstr �Լ� slen ���жԱȣ�
 * �������򷵻� 1 ��
 *
 * ���Ӷȣ�O(N)
 */
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen)
{
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;

    // p �Ǳ�β��
    if (p[0] == ZIP_END) return 0;

    // ��ȡ�ڵ�����
    entry = zipEntry(p);
    // �Ա��ַ���
    if (ZIP_IS_STR(entry.encoding))
    {
        /* Raw compare */
        if (entry.len == slen)
        {
            // O(N)
            return memcmp(p+entry.headersize,sstr,slen) == 0;
        }
        else
        {
            return 0;
        }
        // �Ա�����
    }
    else
    {
        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. */
        if (zipTryEncoding(sstr,slen,&sval,&sencoding))
        {
            zval = zipLoadInteger(p+entry.headersize,entry.encoding);
            return zval == sval;
        }
    }

    return 0;
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. */
/*
 * ���ݸ����� vstr �� vlen �����Һ����Ժ�������ȵĽڵ�
 * ��ÿ�αȶ�֮�䣬���� skip ���ڵ㡣
 *
 * ���Ӷȣ�O(N)
 * ����ֵ��
 *  ����ʧ�ܷ��� NULL ��
 *  ���ҳɹ�����ָ��Ŀ��ڵ��ָ��
 */
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip)
{
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;

    // ���������б�
    while (p[0] != ZIP_END)
    {
        unsigned int prevlensize, encoding, lensize, len;
        unsigned char *q;

        // ����ǰһ���ڵ�ĳ�������Ŀռ�
        ZIP_DECODE_PREVLENSIZE(p, prevlensize);
        // ��ǰ�ڵ�ĳ���
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
        // ������һ���ڵ�ĵ�ַ
        q = p + prevlensize + lensize;

        if (skipcnt == 0)
        {
            /* Compare current entry with specified entry */
            // �Ա��ַ���
            if (ZIP_IS_STR(encoding))
            {
                if (len == vlen && memcmp(q, vstr, vlen) == 0)
                {
                    return p;
                }
                // �Ա�����
            }
            else
            {
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                // �Դ���ֵ���� decode
                if (vencoding == 0)
                {
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding))
                    {
                        /* If the entry can't be encoded we set it to
                         * UCHAR_MAX so that we don't retry again the next
                         * time. */
                        vencoding = UCHAR_MAX;
                    }
                    /* Must be non-zero by now */
                    assert(vencoding);
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                if (vencoding != UCHAR_MAX)
                {
                    // �Ա�
                    long long ll = zipLoadInteger(q, encoding);
                    if (ll == vll)
                    {
                        return p;
                    }
                }
            }

            /* Reset skip count */
            skipcnt = skip;
        }
        else
        {
            /* Skip entry */
            skipcnt--;
        }

        /* Move to next entry */
        p = q + len;
    }

    return NULL;
}

/* Return length of ziplist. */
/*
 * ���� ziplist �ĳ���
 *
 * ���Ӷȣ�O(N)
 */
unsigned int ziplistLen(unsigned char *zl)
{
    unsigned int len = 0;
    // �ڵ������ < UINT16_MAX
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX)
    {
        // ���ȱ�����һ�� uint16 ������
        len = intrev16ifbe(ZIPLIST_LENGTH(zl));
        // �ڵ������ >= UINT16_MAX
    }
    else
    {
        // �������� ziplist �����㳤��
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END)
        {
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough */
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
    }
    return len;
}

/* Return ziplist blob size in bytes. */
/*
 * �������� ziplist �Ŀռ��С
 *
 * ���Ӷȣ�O(1)
 */
size_t ziplistBlobLen(unsigned char *zl)
{
    return intrev32ifbe(ZIPLIST_BYTES(zl));
}

void ziplistRepr(unsigned char *zl)
{
    unsigned char *p;
    int index = 0;
    zlentry entry;

    printf(
        "{total bytes %d} "
        "{length %u}\n"
        "{tail offset %u}\n",
        intrev32ifbe(ZIPLIST_BYTES(zl)),
        intrev16ifbe(ZIPLIST_LENGTH(zl)),
        intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END)
    {
        entry = zipEntry(p);
        printf(
            "{"
            "addr 0x%08lx, "
            "index %2d, "
            "offset %5ld, "
            "rl: %5u, "
            "hs %2u, "
            "pl: %5u, "
            "pls: %2u, "
            "payload %5u"
            "} ",
            (long unsigned)p,
            index,
            (unsigned long) (p-zl),
            entry.headersize+entry.len,
            entry.headersize,
            entry.prevrawlen,
            entry.prevrawlensize,
            entry.len);
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding))
        {
            if (entry.len > 40)
            {
                if (fwrite(p,40,1,stdout) == 0) perror("fwrite");
                printf("...");
            }
            else
            {
                if (entry.len &&
                        fwrite(p,entry.len,1,stdout) == 0) perror("fwrite");
            }
        }
        else
        {
            printf("%lld", (long long) zipLoadInteger(p,entry.encoding));
        }
        printf("\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}

#ifdef ZIPLIST_TEST_MAIN
#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

unsigned char *createList()
{
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

unsigned char *createIntList()
{
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

long long usec(void)
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

void stress(int pos, int num, int maxsize, int dnum)
{
    int i,j,k;
    unsigned char *zl;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum)
    {
        zl = ziplistNew();
        for (j = 0; j < i; j++)
        {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++)
        {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,pos);
            zl = ziplistDeleteRange(zl,0,1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
               i,intrev32ifbe(ZIPLIST_BYTES(zl)),num,posstr[pos],usec()-start);
        zfree(zl);
    }
}

void pop(unsigned char *zl, int where)
{
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl,where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p,&vstr,&vlen,&vlong))
    {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr)
            if (vlen && fwrite(vstr,vlen,1,stdout) == 0) perror("fwrite");
            else
                printf("%lld", vlong);

        printf("\n");
        ziplistDeleteRange(zl,-1,1);
    }
    else
    {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

int randstring(char *target, unsigned int min, unsigned int max)
{
    int p, len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3)
    {
    case 0:
        minval = 0;
        maxval = 255;
        break;
    case 1:
        minval = 48;
        maxval = 122;
        break;
    case 2:
        minval = 48;
        maxval = 52;
        break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

void verify(unsigned char *zl, zlentry *e)
{
    int i;
    int len = ziplistLen(zl);
    zlentry _e;

    for (i = 0; i < len; i++)
    {
        memset(&e[i], 0, sizeof(zlentry));
        e[i] = zipEntry(ziplistIndex(zl, i));

        memset(&_e, 0, sizeof(zlentry));
        _e = zipEntry(ziplistIndex(zl, -len+i));

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

int main(int argc, char **argv)
{
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;

    /* If an argument is given, use it as the random seed. */
    if (argc == 2)
        srand(atoi(argv[1]));

    zl = createIntList();
    ziplistRepr(zl);

    zl = createList();
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_HEAD);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value))
        {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry)
        {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        }
        else
        {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL)
        {
            printf("No entry\n");
        }
        else
        {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value))
        {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry)
        {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        }
        else
        {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value))
        {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry)
        {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        }
        else
        {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL)
        {
            printf("No entry\n");
        }
        else
        {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value))
        {
            printf("Entry: ");
            if (entry)
            {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            }
            else
            {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value))
        {
            printf("Entry: ");
            if (entry)
            {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            }
            else
            {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value))
        {
            printf("Entry: ");
            if (entry)
            {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            }
            else
            {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value))
        {
            printf("No entry\n");
        }
        else
        {
            printf("ERROR\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value))
        {
            printf("Entry: ");
            if (entry)
            {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            }
            else
            {
                printf("%lld", value);
            }
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value))
        {
            printf("Entry: ");
            if (entry)
            {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            }
            else
            {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl,&p);
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        while (ziplistGet(p,&entry,&elen,&value))
        {
            if (entry && strncmp("foo",(char*)entry,elen) == 0)
            {
                printf("Delete foo\n");
                zl = ziplistDelete(zl,&p);
            }
            else
            {
                printf("Entry: ");
                if (entry)
                {
                    if (elen && fwrite(entry,elen,1,stdout) == 0)
                        perror("fwrite");
                }
                else
                {
                    printf("%lld",value);
                }
                p = ziplistNext(zl,p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257],v2[257];
        memset(v1,'x',256);
        memset(v2,'y',256);
        zl = ziplistNew();
        zl = ziplistPush(zl,(unsigned char*)v1,strlen(v1),ZIPLIST_TAIL);
        zl = ziplistPush(zl,(unsigned char*)v2,strlen(v2),ZIPLIST_TAIL);

        /* Pop values again and compare their value. */
        p = ziplistIndex(zl,0);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v1,(char*)entry,elen) == 0);
        p = ziplistIndex(zl,1);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v2,(char*)entry,elen) == 0);
        printf("SUCCESS\n\n");
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257];
        zlentry e[3];
        int i;

        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++)
        {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][  1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++)
        {
            zl = ziplistPush(zl, (unsigned char *) v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
    }

    printf("Create long list and check indices:\n");
    {
        zl = ziplistNew();
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++)
        {
            len = sprintf(buf,"%d",i);
            zl = ziplistPush(zl,(unsigned char*)buf,len,ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++)
        {
            p = ziplistIndex(zl,i);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(i == value);

            p = ziplistIndex(zl,-i-1);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(999-i == value);
        }
        printf("SUCCESS\n\n");
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5))
        {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5))
        {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4))
        {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4))
        {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        for (i = 0; i < 20000; i++)
        {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref,sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++)
            {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2)
                {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                }
                else
                {
                    switch(rand() % 3)
                    {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to ziplist */
                zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

                /* Add to reference list */
                if (where == ZIPLIST_HEAD)
                {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                }
                else if (where == ZIPLIST_TAIL)
                {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                }
                else
                {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++)
            {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = ziplistIndex(zl,j);
                refnode = listIndex(ref,j);

                assert(ziplistGet(p,&sstr,&slen,&sval));
                if (sstr == NULL)
                {
                    buflen = sprintf(buf,"%lld",sval);
                }
                else
                {
                    buflen = slen;
                    memcpy(buf,sstr,buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf,listNodeValue(refnode),buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with variable ziplist size:\n");
    {
        stress(ZIPLIST_HEAD,100000,16384,256);
        stress(ZIPLIST_TAIL,100000,16384,256);
    }

    return 0;
}

#endif
