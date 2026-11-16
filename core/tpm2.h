#ifndef HYPERWIN_TPM2_H
#define HYPERWIN_TPM2_H

#include <stdint.h>
#include <stddef.h>

#define TPM_RC_SUCCESS 0x000u
#define TPM_RC_INITIALIZE 0x100u
#define TPM_RC_FAILURE 0x101u
#define TPM_RC_COMMAND_CODE 0x143u
#define TPM_RC_VALUE 0x184u

#define TPM_ALG_SHA256 0x000Bu
#define TPM_CC_STARTUP 0x00000144u
#define TPM_CC_SHUTDOWN 0x00000145u
#define TPM_CC_GET_RANDOM 0x0000017Bu
#define TPM_CC_GET_CAPABILITY 0x0000017Au
#define TPM_CC_PCR_READ 0x0000017Eu
#define TPM_CC_PCR_EXTEND 0x00000182u
#define TPM_SU_CLEAR 0x0000u
#define TPM_CAP_TPM_PROPERTIES 0x00000006u
#define TPM_PT_MANUFACTURER 0x00000105u

struct hw_tpm {
    int started;
    uint32_t counter;
    uint8_t seed[32];
    uint8_t pcr[24][32];
};

void hw_tpm_init(struct hw_tpm *t, const uint8_t seed[32]);
/* Returns the response length, or 0 when resp_cap is too small. */
uint32_t hw_tpm_exec(struct hw_tpm *t, const uint8_t *cmd, uint32_t cmd_len,
                     uint8_t *resp, uint32_t resp_cap);

#endif
