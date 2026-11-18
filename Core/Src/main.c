#include "main.h"
#include <stdio.h>
#include <string.h>
#include "protocol_icd.h"
#include "security.h"


typedef enum {
    STATE_NOT_INIT = 0,     // 1 Hz
    STATE_SYNCHRONIZING,    // 2.5 Hz
    STATE_SYNCHRONIZED,     // Fixe ON
    STATE_ERROR             // 5 Hz
} SystemState_t;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart6;

SystemState_t current_state = STATE_NOT_INIT;
uint32_t last_led_tick = 0;

// Buffers reception UART1 (amont) et UART6 (aval)
uint8_t rx1_byte, rx1_buf[ICD_FRAME_SIZE], rx1_idx = 0;
uint32_t rx1_last_tick = 0;
ProtocolMessage_t rx1_frame;
volatile uint8_t rx1_ready = 0;

uint8_t rx6_byte, rx6_buf[ICD_FRAME_SIZE], rx6_idx = 0;
uint32_t rx6_last_tick = 0;
ProtocolMessage_t rx6_frame;
volatile uint8_t rx6_ready = 0;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART6_UART_Init(void);

void Update_LED(void);
void Check_Button(void);

#ifdef __GNUC__
int __io_putchar(int ch)
#else
int fputc(int ch, FILE *f)
#endif
{
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_USART1_UART_Init();
    MX_USART6_UART_Init();

    printf("\r\n--- Boot STM32 : Test RX UART1 / UART6 ---\r\n");

    Security_InitPRNG();
    uint8_t  test_id  = Security_Random8();
    uint32_t test_key = Security_Random32();
    uint8_t  sample[16] = "TEST_CHIFFREMENT";

    Security_CryptPayload(sample, 16, test_key, 1);
    printf("[TEST-SEC] ID=0x%02X | Cle=0x%08lX | Chiffre=%02X %02X %02X...\r\n",
           test_id, (unsigned long)test_key, sample[0], sample[1], sample[2]);

    Security_CryptPayload(sample, 16, test_key, 1);
    printf("[TEST-SEC] Dechiffre=%.16s\r\n", sample);

    HAL_UART_Receive_IT(&huart1, &rx1_byte, 1);
    HAL_UART_Receive_IT(&huart6, &rx6_byte, 1);

    while (1)
    {
        Update_LED();
        Check_Button();

        // Test temporaire : verification reception sur USART1 et relais vers USART6
        if (rx1_ready)
        {
            ProtocolMessage_t frame_copy;
            __disable_irq();
            memcpy(&frame_copy, &rx1_frame, sizeof(ProtocolMessage_t));
            rx1_ready = 0;
            __enable_irq();

            uint8_t ok = Protocol_VerifyMessage(&frame_copy);
            printf("[UART1] Trame recue | CMD=0x%02X | SRC=0x%02lX | CRC_OK=%u\r\n",
                   frame_copy.header.command_type,
                   (unsigned long)frame_copy.header.id_src,
                   ok);

            if (ok)
            {
                HAL_UART_Transmit(&huart6, (uint8_t *)&frame_copy, sizeof(ProtocolMessage_t), 100);
            }
        }
    }
}

void Update_LED(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t period = 500; // 1 Hz par defaut

    if (current_state == STATE_SYNCHRONIZED)
    {
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
        return;
    }

    if (current_state == STATE_SYNCHRONIZING) period = 200; // 2.5 Hz
    else if (current_state == STATE_ERROR)    period = 100; // 5 Hz

    if (now - last_led_tick >= period)
    {
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
        last_led_tick = now;
    }
}

void Check_Button(void)
{
    static uint8_t last_state = GPIO_PIN_SET;
    static uint8_t clicks = 0;
    static uint32_t first_click = 0;
    static uint32_t debounce = 0;

    uint8_t btn = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13);
    uint32_t now = HAL_GetTick();

    if (clicks > 0 && (now - first_click > 1500))
        clicks = 0;

    if (btn == GPIO_PIN_RESET && last_state == GPIO_PIN_SET && (now - debounce > 50))
    {
        debounce = now;
        if (clicks == 0) first_click = now;
        clicks++;

        if (clicks >= 3)
        {
            clicks = 0;
            current_state = STATE_NOT_INIT;
            printf("[BTN] Triple clic -> Reset etat\r\n");
        }
        else
        {
            // Test temporaire : envoi d'une trame PING sur USART6
            ProtocolMessage_t ping;
            Protocol_BuildMessage(&ping, CMD_MSG, 0x01, (const uint8_t *)"PING_UART", 9);
            HAL_UART_Transmit(&huart6, (uint8_t *)&ping, sizeof(ProtocolMessage_t), 100);
            printf("[TEST] Trame PING envoyee sur USART6\r\n");
        }
    }
    last_state = btn;
}

void USART1_IRQHandler(void) { HAL_UART_IRQHandler(&huart1); }
void USART6_IRQHandler(void) { HAL_UART_IRQHandler(&huart6); }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uint32_t now = HAL_GetTick();

    if (huart->Instance == USART1)
    {
        if (rx1_idx > 0 && (now - rx1_last_tick > 30)) rx1_idx = 0;
        rx1_last_tick = now;

        if (rx1_idx > 0 || rx1_byte == ICD_START_BYTE)
        {
            rx1_buf[rx1_idx++] = rx1_byte;
            if (rx1_idx >= ICD_FRAME_SIZE)
            {
                if (!rx1_ready)
                {
                    memcpy(&rx1_frame, rx1_buf, ICD_FRAME_SIZE);
                    rx1_ready = 1;
                }
                rx1_idx = 0;
            }
        }
        HAL_UART_Receive_IT(&huart1, &rx1_byte, 1);
    }
    else if (huart->Instance == USART6)
    {
        if (rx6_idx > 0 && (now - rx6_last_tick > 30)) rx6_idx = 0;
        rx6_last_tick = now;

        if (rx6_idx > 0 || rx6_byte == ICD_START_BYTE)
        {
            rx6_buf[rx6_idx++] = rx6_byte;
            if (rx6_idx >= ICD_FRAME_SIZE)
            {
                if (!rx6_ready)
                {
                    memcpy(&rx6_frame, rx6_buf, ICD_FRAME_SIZE);
                    rx6_ready = 1;
                }
                rx6_idx = 0;
            }
        }
        HAL_UART_Receive_IT(&huart6, &rx6_byte, 1);
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = 16;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ = 7;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) Error_Handler();
}

static void MX_USART1_UART_Init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();

    HAL_NVIC_SetPriority(USART1_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

static void MX_USART2_UART_Init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

static void MX_USART6_UART_Init(void)
{
    __HAL_RCC_USART6_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    huart6.Instance = USART6;
    huart6.Init.BaudRate = 115200;
    huart6.Init.WordLength = UART_WORDLENGTH_8B;
    huart6.Init.StopBits = UART_STOPBITS_1;
    huart6.Init.Parity = UART_PARITY_NONE;
    huart6.Init.Mode = UART_MODE_TX_RX;
    huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart6.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart6) != HAL_OK) Error_Handler();

    HAL_NVIC_SetPriority(USART6_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART6_IRQn);
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = LD2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}