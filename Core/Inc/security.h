#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>

void     Security_InitPRNG(void);
uint8_t  Security_Random8(void);
uint32_t Security_Random32(void);
void     Security_CryptPayload(uint8_t *data, uint8_t len, uint32_t key, uint8_t nonce);

#endif // SECURITY_H