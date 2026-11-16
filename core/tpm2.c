#include "tpm2.h"
#include "sha256.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t reply(uint8_t *resp, uint32_t cap, uint32_t rc, uint32_t extra)
{
    uint32_t n = 10 + extra;
    if (n > cap || cap < 10) return 0;
    wr16(resp, 0x8001);
    wr32(resp + 2, n);
    wr32(resp + 6, rc);
    return n;
}

static void fail(uint8_t *resp, uint32_t code, uint32_t rc)
{
    wr16(resp, 0x8001);
    wr32(resp + 2, 10);
    wr32(resp + 6, rc);
    (void)code;
}

void hw_tpm_init(struct hw_tpm *t, const uint8_t seed[32])
{
    uint32_t i;
    for (i = 0; i < sizeof(*t); i++) ((uint8_t *)t)[i] = 0;
    if (seed) {
        for (i = 0; i < 32; i++) t->seed[i] = seed[i];
    } else {
        for (i = 0; i < 32; i++) t->seed[i] = 0xA5;
    }
}

static void random_bytes(struct hw_tpm *t, uint8_t *dst, uint32_t n)
{
    uint8_t block[32];
    uint8_t material[36];
    uint32_t i;
    for (i = 0; i < 32; i++) material[i] = t->seed[i];
    while (n) {
        uint32_t take, c = ++t->counter;
        material[32] = (uint8_t)(c >> 24);
        material[33] = (uint8_t)(c >> 16);
        material[34] = (uint8_t)(c >> 8);
        material[35] = (uint8_t)c;
        hw_sha256(material, 36, block);
        take = n < 32 ? n : 32;
        for (i = 0; i < take; i++) dst[i] = block[i];
        dst += take;
        n -= take;
    }
}

uint32_t hw_tpm_exec(struct hw_tpm *t, const uint8_t *cmd, uint32_t cmd_len,
                     uint8_t *resp, uint32_t resp_cap)
{
    uint32_t code, size;
    if (!cmd || cmd_len < 10 || !resp || resp_cap < 10) return 0;
    if (rd16(cmd) != 0x8001 && rd16(cmd) != 0x8002) {
        fail(resp, 0, TPM_RC_VALUE);
        return 10;
    }
    size = rd32(cmd + 2);
    code = rd32(cmd + 6);
    if (size < 10 || size > cmd_len) {
        fail(resp, code, TPM_RC_VALUE);
        return 10;
    }

    if (code == TPM_CC_STARTUP) {
        if (size < 12) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        t->started = 1;
        if (rd16(cmd + 10) == TPM_SU_CLEAR) {
            uint32_t i, j;
            for (i = 0; i < 24; i++)
                for (j = 0; j < 32; j++) t->pcr[i][j] = 0;
        }
        return reply(resp, resp_cap, TPM_RC_SUCCESS, 0);
    }
    if (!t->started && code != TPM_CC_GET_CAPABILITY) {
        fail(resp, code, TPM_RC_INITIALIZE);
        return 10;
    }
    if (code == TPM_CC_SHUTDOWN) {
        t->started = 0;
        return reply(resp, resp_cap, TPM_RC_SUCCESS, 0);
    }
    if (code == TPM_CC_GET_RANDOM) {
        uint32_t n;
        if (size < 12) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        n = rd16(cmd + 10);
        if (n > 256 || 12 + n > resp_cap) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        reply(resp, resp_cap, TPM_RC_SUCCESS, 2 + n);
        wr16(resp + 10, (uint16_t)n);
        random_bytes(t, resp + 12, n);
        return 12 + n;
    }
    if (code == TPM_CC_GET_CAPABILITY) {
        /* capability, property, count -> more, capability, count, property, value */
        if (size < 22) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        if (rd32(cmd + 10) != TPM_CAP_TPM_PROPERTIES) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        reply(resp, resp_cap, TPM_RC_SUCCESS, 1 + 4 + 4 + 8);
        resp[10] = 0; /* more */
        wr32(resp + 11, TPM_CAP_TPM_PROPERTIES);
        wr32(resp + 15, 1);
        wr32(resp + 19, TPM_PT_MANUFACTURER);
        wr32(resp + 23, 0x4857494Eu); /* 'HWIN' */
        return 27;
    }
    if (code == TPM_CC_PCR_READ) {
        uint32_t count, hash, sel, idx, n = 0;
        if (size < 17) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        count = rd32(cmd + 10);
        if (count != 1) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        hash = rd16(cmd + 14);
        sel = cmd[16];
        if (hash != TPM_ALG_SHA256 || sel < 1 || 17 + sel > size) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        for (idx = 0; idx < 24; idx++) {
            if (cmd[17 + (idx / 8)] & (1u << (idx % 8))) break;
        }
        if (idx >= 24) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        /* updateCounter, selection echo, digest count, digest */
        if (resp_cap < 10 + 4 + 4 + 2 + 1 + sel + 4 + 2 + 32) {
            fail(resp, code, TPM_RC_FAILURE);
            return 10;
        }
        reply(resp, resp_cap, TPM_RC_SUCCESS, 4 + 4 + 2 + 1 + sel + 4 + 2 + 32);
        wr32(resp + 10, 0);
        wr32(resp + 14, 1);
        wr16(resp + 18, TPM_ALG_SHA256);
        resp[20] = (uint8_t)sel;
        for (n = 0; n < sel; n++) resp[21 + n] = cmd[17 + n];
        wr32(resp + 21 + sel, 1);
        wr16(resp + 25 + sel, 32);
        for (n = 0; n < 32; n++) resp[27 + sel + n] = t->pcr[idx][n];
        return 27 + sel + 32;
    }
    if (code == TPM_CC_PCR_EXTEND) {
        /* Simplified: handle, digest count 1, alg, 32-byte digest. No auth session. */
        uint32_t handle, digests;
        uint8_t cat[64];
        uint32_t i;
        if (size < 10 + 4 + 4 + 2 + 32) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        handle = rd32(cmd + 10);
        digests = rd32(cmd + 14);
        if (handle >= 24 || digests != 1 || rd16(cmd + 18) != TPM_ALG_SHA256) {
            fail(resp, code, TPM_RC_VALUE);
            return 10;
        }
        for (i = 0; i < 32; i++) cat[i] = t->pcr[handle][i];
        for (i = 0; i < 32; i++) cat[32 + i] = cmd[20 + i];
        hw_sha256(cat, 64, t->pcr[handle]);
        return reply(resp, resp_cap, TPM_RC_SUCCESS, 0);
    }
    fail(resp, code, TPM_RC_COMMAND_CODE);
    return 10;
}
