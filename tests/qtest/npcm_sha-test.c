/*
 * QTests for Nuvoton NPCM7xx/8xx SHA Modules.
 *
 * Copyright (c) Nuvoton Technology Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "libqtest-single.h"
#include <byteswap.h>

#define NPCM8XX_SHA_BASE 0xF085A000
#define NPCM8XX_SHA_RST     BIT(2)
#define NPCM8XX_SHA_BUSY    BIT(1)
#define NPCM8XX_SHA_EN      BIT(0)
#define NPCM8XX_SHA_SHA1    BIT(0)
#define NPCM8XX_CMD_SHA_512 BIT(3)
#define NPCM8XX_CMD_ROUND   BIT(2)
#define NPCM8XX_CMD_WR      BIT(1)
#define NPCM8XX_CMD_RD      BIT(0)
#define INPUT_TEXT "Hiss hisss Hissss hiss Hiss hisss Hiss hiss"
#define OUTPUT_SHA1 "b2e74f26758a3a421e509cee045244b78753cc02"
#define OUTPUT_SHA256 "bc757abb0436586f392b437e5dd24096" \
                      "f7f224de6b74d4d86e2abc6121b160d0"
#define OUTPUT_SHA384 "887ce52efb4f46700376356583b7e279" \
                      "4f612bd024e4495087ddb946c448c69d" \
                      "56dbf7152a94a5e63a80f3ba9f0eed78"
#define OUTPUT_SHA512 "3a90d79638235ec6c4c11bebd84d83c0" \
                      "549bc1e84edc4b6ec7086487641256cb" \
                      "63b54e4cb2d2032b393994aa263c0dbb" \
                      "e00a9f2fe9ef6037352232a1eec55ee7"
#define SHA_TIMEOUT         100
#define SHA1_BLOCK_SIZE     64
#define SHA256_BLOCK_SIZE   64
#define SHA512_BLOCK_SIZE   128

enum {
    NPCM_SHA_DATA_IN_REG = 0x00,
    NPCM_SHA_CTR_STS_REG = 0x04,
    NPCM_SHA_CFG_REG = 0x08,
    NPCM_SHA512_DATA_IN_REG = 0x10,
    NPCM_SHA512_CTR_STS_REG = 0x14,
    NPCM_SHA512_CMD_REG = 0x18,
    NPCM_SHA512_HASH_OUT_REG = 0x1C,
    NPCM_SHA_HASH_OUT_REG = 0x20,
    NPCM_SHA_HASH_OUT_LAST_REG = 0x3C,
};
enum {
    type_sha1 = 0,
    type_sha256,
    type_sha384,
    type_sha512,
};
struct hash_info {
    uint32_t block_sz;
    uint32_t digest_len;
    uint8_t length_bytes;
    uint8_t type;
};
static struct hash_info npcm_hash_tbl[] = {
    { .block_sz = 64, .digest_len = 160, .length_bytes = 8, .type = type_sha1 },
    { .block_sz = 64, .digest_len = 256, .length_bytes = 8, .type = type_sha256 },
    { .block_sz = 128, .digest_len = 384, .length_bytes = 16, .type = type_sha384 },
    { .block_sz = 128, .digest_len = 512, .length_bytes = 16, .type = type_sha512 },
};

uint32_t sha256_init_val[] = {
    0x67e6096a, 0x85ae67bb, 0x72f36e3c, 0x3af54fa5,
    0x7f520e51, 0x8c68059b, 0xabd9831f, 0x19cde05b
};
uint32_t sha1_init_val[] = {
    0x01234567, 0x89abcdef, 0xfedcba98, 0x76543210,
    0xf0e1d2c3,
};
static void dump_buf_if_failed(const uint8_t *buf, size_t size)
{
    if (g_test_failed()) {
        qemu_hexdump(stderr, "", buf, size);
    }
}
static uint8_t sha_readb(QTestState *s, uint32_t offset) {
    return qtest_readb(s, NPCM8XX_SHA_BASE + offset);
}
static uint32_t sha_readl(QTestState *s, uint32_t offset) {
    return qtest_readl(s, NPCM8XX_SHA_BASE + offset);
}
static void sha_writeb(QTestState *s, uint32_t offset, uint8_t data) {
    qtest_writeb(s, NPCM8XX_SHA_BASE + offset, data);
}
static void sha_writel(QTestState *s, uint32_t offset, uint32_t data) {
    qtest_writel(s, NPCM8XX_SHA_BASE + offset, data);
}
static char* to_hex_str(uint8_t *data, size_t size)
{
    static char hash_str[128 + 1];
    memset(hash_str, 0, sizeof(hash_str));
    for (uint32_t i = 0; i < size; i++) {
        snprintf(hash_str + i * 2, 3, "%02x", data[i]);
    }
    return hash_str;
}

static void test_npcm8xx_sha_reset(void) {
    // test SHA init state
    printf("test SHA init state\n");
    QTestState *s = qtest_init("-machine npcm845-evb");
    g_assert_cmphex(sha_readb(s, NPCM_SHA_CTR_STS_REG), ==, 0x80);
    g_assert_cmphex(sha_readb(s, NPCM_SHA_CFG_REG), ==, 0x00);
    // test sha512
    g_assert_cmphex(sha_readb(s, NPCM_SHA512_CTR_STS_REG), ==, 0x80);
    g_assert_cmphex(sha_readb(s, NPCM_SHA512_CMD_REG), ==, 0x00);
    qtest_quit(s);
}

static int npcm_sha_wait_busy(QTestState *s)
{
    uint32_t waits = SHA_TIMEOUT;
    uint8_t val;

    while (waits--) {
        val = sha_readb(s, NPCM_SHA_CTR_STS_REG);
        if ((val & NPCM8XX_SHA_BUSY) == 0)
        {
            printf("npcm_sha: wait busy done, %d\n", waits);
            return 0;
        }
    }

    return -110;
}
static void prepare_block_data(uint8_t *buf, struct hash_info *hash)
{
    uint8_t *data = (uint8_t *)INPUT_TEXT;
    size_t count = strlen(INPUT_TEXT);
    memcpy(buf, data, count);
    buf[count] = 0x80;
    size_t padding_len = hash->block_sz - count - 1 - hash->length_bytes;
    memset(buf + count + 1, 0, padding_len);
    uint8_t *msglen = hash->length_bytes == 16 ? buf + 124 : buf + 60;
    *(uint32_t*)msglen = bswap_32(count << 3);
}

static void test_npcm8xx_sha256_process(void) {
    // test SHA process
    printf("test SHA process\n");
    QTestState *s = qtest_init("-machine npcm845-evb");
    // reset sha256
    sha_writeb(s, NPCM_SHA_CFG_REG, 0);
    uint8_t val = sha_readb(s, NPCM_SHA_CTR_STS_REG) & ~NPCM8XX_SHA_EN;
    sha_writeb(s, NPCM_SHA_CTR_STS_REG, NPCM8XX_SHA_RST | val);
    g_assert_cmphex(sha_readb(s, NPCM_SHA_CTR_STS_REG), ==, 0x80);
    uint8_t i;
    for (i = 0; i < 8; i++) {
        g_assert_cmphex(sha_readl(s, NPCM_SHA_HASH_OUT_REG + i * 4), ==,
            sha256_init_val[i]);
    }
    // enable sha256
    sha_writeb(s, NPCM_SHA_CTR_STS_REG, NPCM8XX_SHA_EN | val);
    g_assert_cmphex(sha_readb(s, NPCM_SHA_CTR_STS_REG), ==, 0x81);

    // prepare test data
    uint8_t buf[SHA256_BLOCK_SIZE] = {0};
    prepare_block_data(buf, &npcm_hash_tbl[type_sha1]);
    uint32_t *buf32 = (uint32_t *)buf;
    // write input data
    for (i = 0; i < (SHA256_BLOCK_SIZE / 4); i++) {
        sha_writel(s, NPCM_SHA_DATA_IN_REG, buf32[i]);
    }
    // wait for busy done
    g_assert_cmpint(npcm_sha_wait_busy(s), ==, 0);

    // read hash output
    uint32_t hash[8];
    uint8_t *hash_b = (uint8_t *)hash;
    for (i = 0; i < 8; i++) {
        hash[i] = sha_readl(s, NPCM_SHA_HASH_OUT_REG + i * 4);
    }
    char *hash_str = to_hex_str(hash_b, 256/8);
    g_assert_cmpstr(hash_str, ==, OUTPUT_SHA256);
    dump_buf_if_failed(hash_b, 32);


    qtest_quit(s);
}

static void test_npcm8xx_sha1_process(void) {
    printf("test SHA1 process\n");
    QTestState *s = qtest_init("-machine npcm845-evb");
    // reset sha
    sha_writeb(s, NPCM_SHA_CFG_REG, 1);
    uint8_t val = sha_readb(s, NPCM_SHA_CTR_STS_REG) & ~NPCM8XX_SHA_EN;
    sha_writeb(s, NPCM_SHA_CTR_STS_REG, NPCM8XX_SHA_RST | val);
    g_assert_cmphex(sha_readb(s, NPCM_SHA_CTR_STS_REG), ==, 0x80);
    g_assert_cmphex(sha_readb(s, NPCM_SHA_CFG_REG), ==, 0x01);
    uint8_t i;
    for (i = 0; i < 5; i++) {
        g_assert_cmphex(sha_readl(s, NPCM_SHA_HASH_OUT_REG + i * 4), ==,
            sha1_init_val[i]);
    }
    // enable sha
    sha_writeb(s, NPCM_SHA_CTR_STS_REG, NPCM8XX_SHA_EN | val);
    g_assert_cmphex(sha_readb(s, NPCM_SHA_CTR_STS_REG), ==, 0x81);

    // prepare test data
    uint8_t buf[SHA1_BLOCK_SIZE] = {0};
    prepare_block_data(buf, &npcm_hash_tbl[type_sha256]);
    uint32_t *buf32 = (uint32_t *)buf;
    // write input data
    for (i = 0; i < (SHA1_BLOCK_SIZE / sizeof(uint32_t)); i++) {
        sha_writel(s, NPCM_SHA_DATA_IN_REG, buf32[i]);
    }
    // wait for busy done
    g_assert_cmpint(npcm_sha_wait_busy(s), ==, 0);
    // read hash output
    uint32_t hash[5];
    uint8_t *hash_b = (uint8_t *)hash;
    for (i = 0; i < 5; i++) {
        hash[i] = sha_readl(s, NPCM_SHA_HASH_OUT_REG + i * 4);
    }
    char *hash_str = to_hex_str(hash_b, 160/8);
    g_assert_cmpstr(hash_str, ==, OUTPUT_SHA1);
    qtest_quit(s);
}

static void npcm_sha384_512_process(struct hash_info *hash)
{
    QTestState *s = qtest_init("-machine npcm845-evb");
    // reset, no need select type before reset
    uint8_t val = sha_readb(s, NPCM_SHA512_CTR_STS_REG) & ~NPCM8XX_SHA_EN;
    sha_writeb(s, NPCM_SHA512_CTR_STS_REG, NPCM8XX_SHA_RST | val);

    // enable, then set type, round, and write in CMD register
    sha_writeb(s, NPCM_SHA512_CTR_STS_REG, NPCM8XX_SHA_EN | val);
    val = hash->type == type_sha384 ? 0 : NPCM8XX_CMD_SHA_512;
    sha_writeb(s, NPCM_SHA512_CMD_REG, val | NPCM8XX_CMD_WR);

    // prepare data
    uint8_t buf[SHA512_BLOCK_SIZE] = {0};
    prepare_block_data(buf, hash);
    // update data
    uint32_t *buf32 = (uint32_t *)buf;
    uint8_t i;
    for (i = 0; i < (SHA512_BLOCK_SIZE / sizeof(uint32_t)); i++) {
        sha_writel(s, NPCM_SHA512_DATA_IN_REG, buf32[i]);
    }
    // wait for busy done
    g_assert_cmpint(npcm_sha_wait_busy(s), ==, 0);
    // write read hash cmd
    sha_writel(s, NPCM_SHA512_CMD_REG, val | NPCM8XX_CMD_RD);
    // read hash
    uint32_t hash_val[32];
    uint8_t *hash_b = (uint8_t *)hash_val;
    for (i = 0; i < (hash->digest_len / 32); i++) {
        hash_val[i] = sha_readl(s, NPCM_SHA512_HASH_OUT_REG);
    }
    char *hash_str = to_hex_str(hash_b, hash->digest_len / 8);
    if (hash->type == type_sha384) {
        g_assert_cmpstr(hash_str, ==, OUTPUT_SHA384);
    } else {
        g_assert_cmpstr(hash_str, ==, OUTPUT_SHA512);
    }
    qtest_quit(s);
}

static void test_npcm8xx_sha512_process(void)
{
    struct hash_info *hash = &npcm_hash_tbl[type_sha512];
    printf("test SHA512 process\n");
    npcm_sha384_512_process(hash);
}
static void test_npcm8xx_sha384_process(void)
{
    struct hash_info *hash = &npcm_hash_tbl[type_sha384];
    printf("test SHA384 process\n");
    npcm_sha384_512_process(hash);
}
static void test_npcm8xx_sha512_load(void)
{
    QTestState *s = qtest_init("-machine npcm845-evb");
    sha_writeb(s, NPCM_SHA512_CTR_STS_REG, 0x83);
    sha_writeb(s, NPCM_SHA512_CMD_REG, 0x10);
    uint32_t init_data[16] = {
        0x6a09e667, 0xf3bcc908, 0xbb67ae85, 0x84caa73b,
        0x3c6ef372, 0xfe94f82b, 0xa54ff53a, 0x5f1d36f1,
        0x510e527f, 0xade682d1, 0x9b05688c, 0x2b3e6c1f,
        0x1f83d9ab, 0xfb41bd6b, 0x5be0cd19, 0x137e2179
    };
    uint8_t i;
    for (i = 0; i < 16; i++) {
        sha_writel(s, NPCM_SHA512_DATA_IN_REG, init_data[i]);
    }
    sha_writeb(s, NPCM_SHA512_CMD_REG, NPCM8XX_CMD_RD);
    for (i = 0; i < 16; i++) {
        g_assert_cmphex(sha_readl(s, NPCM_SHA512_HASH_OUT_REG), ==,
            bswap_32(init_data[i]));
    }
}

int main(int argc, char **argv) {
    //int ret;
    g_test_init(&argc, &argv, NULL);
    g_test_set_nonfatal_assertions();
    qtest_add_func("/npcm8xx_sha/sha_reset", test_npcm8xx_sha_reset);
    qtest_add_func("/npcm8xx_sha/sha256_process", test_npcm8xx_sha256_process);
    qtest_add_func("/npcm8xx_sha/sha1_process", test_npcm8xx_sha1_process);
    qtest_add_func("/npcm8xx_sha/sha512_process", test_npcm8xx_sha512_process);
    qtest_add_func("/npcm8xx_sha/sha384_process", test_npcm8xx_sha384_process);
    qtest_add_func("/npcm8xx_sha/sha512_load", test_npcm8xx_sha512_load);

    return g_test_run();
}