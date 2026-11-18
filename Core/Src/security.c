#include "security.h"
#include "stm32f4xx_hal.h"

static uint32_t prng_state = 0x12345678;

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    if (x == 0) x = 0xACE1U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void Security_InitPRNG(void)
{
    // Melange de l'UID unique grave dans le STM32 et du SysTick
    uint32_t uid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    prng_state = uid ^ SysTick->VAL ^ 0x9E3779B9U;
    if (prng_state == 0) prng_state = 0x12345678;
}

uint8_t Security_Random8(void)
{
    uint8_t r = (uint8_t)(xorshift32(&prng_state) & 0xFF);
    return (r == 0) ? 1 : r;
}

uint32_t Security_Random32(void)
{
    uint32_t r = xorshift32(&prng_state);
    return (r == 0) ? 0xA5A55A5A : r;
}

void Security_CryptPayload(uint8_t *data, uint8_t len, uint32_t key, uint8_t nonce)
{
    uint32_t stream_state = key ^ ((uint32_t)nonce * 0x01010101U);
    if (stream_state == 0) stream_state = 0x55AA55AAU;

    for (uint8_t i = 0; i < len; i++)
    {
        uint8_t ks = (uint8_t)(xorshift32(&stream_state) & 0xFF);
        data[i] ^= ks;
    }
}