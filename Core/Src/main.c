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

typedef struct {
    uint8_t      card_id;
    uint8_t      position;
    NodeStatus_t status;
    uint32_t     session_key;
    uint8_t      has_key;
    uint8_t      is_exit_node;
    uint8_t      tx_nonce;
    uint32_t     up_id;
    uint32_t     down_id;
} NodeContext_t;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart6;

SystemState_t current_state = STATE_NOT_INIT;
NodeContext_t node;
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

void Node_Reset(uint8_t send_downstream);
void Update_LED(void);
void Check_Button(void);
void Process_Frame(const ProtocolMessage_t *msg);

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

    Security_InitPRNG();
    Node_Reset(0);

    printf("\r\n--- Demarrage carte ID: 0x%02X ---\r\n", node.card_id);

    HAL_UART_Receive_IT(&huart1, &rx1_byte, 1);
    HAL_UART_Receive_IT(&huart6, &rx6_byte, 1);

    while (1)
    {
        Update_LED();
        Check_Button();

        if (rx1_ready)
        {
            ProtocolMessage_t frame_copy;
            __disable_irq();
            memcpy(&frame_copy, &rx1_frame, sizeof(ProtocolMessage_t));
            rx1_ready = 0;
            __enable_irq();

            Process_Frame(&frame_copy);
        }
    }
}

void Node_Reset(uint8_t send_downstream)
{
    if (send_downstream)
    {
        ProtocolMessage_t tx;
        Protocol_BuildMessage(&tx, CMD_RESET, node.card_id, NULL, 0);
        HAL_UART_Transmit(&huart6, (uint8_t *)&tx, sizeof(ProtocolMessage_t), 100);
    }

    memset(&node, 0, sizeof(NodeContext_t));
    node.card_id = Security_Random8();
    node.status = STATUS_UNKNOWN;
    current_state = STATE_NOT_INIT;

    printf("[RESET] Nouvel ID = 0x%02X\r\n", node.card_id);
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
            printf("[BTN] Triple clic -> Reset chaine\r\n");
            Node_Reset(1);
        }
    }
    last_state = btn;
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

static void Send_Up(CommandType_t cmd, const uint8_t *payload, uint8_t len)
{
    ProtocolMessage_t tx;
    Protocol_BuildMessage(&tx, cmd, node.card_id, payload, len);
    HAL_UART_Transmit(&huart1, (uint8_t *)&tx, sizeof(ProtocolMessage_t), 100);
}

static uint8_t Send_Down_Wait_Ack(CommandType_t cmd, uint8_t nonce, const uint8_t *payload, uint8_t len, uint32_t timeout, uint32_t *ack_src)
{
    ProtocolMessage_t tx;
    Protocol_BuildMessage(&tx, cmd, node.card_id, payload, len);
    tx.header.reserve = nonce;
    tx.crc = Protocol_ComputeCRC(&tx);

    __disable_irq();
    rx6_idx = 0;
    rx6_ready = 0;
    __enable_irq();

    HAL_UART_Transmit(&huart6, (uint8_t *)&tx, sizeof(ProtocolMessage_t), 100);

    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout)
    {
        if (rx6_ready)
        {
            ProtocolMessage_t resp;
            __disable_irq();
            memcpy(&resp, &rx6_frame, sizeof(ProtocolMessage_t));
            rx6_ready = 0;
            __enable_irq();

            if (Protocol_VerifyMessage(&resp))
            {
                if (resp.header.command_type == CMD_ACK)
                {
                    if (ack_src) *ack_src = resp.header.id_src;
                    return 1;
                }
                if (resp.header.command_type == CMD_ERROR)
                    return 0;
            }
        }
    }
    return 0;
}

void Process_Frame(const ProtocolMessage_t *msg)
{
    if (!Protocol_VerifyMessage(msg))
    {
        printf("[ERR] Trame ou CRC invalide\r\n");
        current_state = STATE_ERROR;
        Send_Up(CMD_ERROR, NULL, 0);
        return;
    }

    CommandType_t cmd = (CommandType_t)msg->header.command_type;

    if (cmd == CMD_RESET)
    {
        Send_Up(CMD_ACK, NULL, 0);
        Node_Reset(1);
        return;
    }

    if (cmd == CMD_GET_INFO)
    {
        uint8_t info[3] = { node.card_id, node.position, (uint8_t)node.status };
        Send_Up(CMD_GET_INFO, info, 3);
        printf("[INFO] ID=0x%02X | Pos=%d | Statut=%d\r\n", info[0], info[1], info[2]);
        return;
    }

    if (cmd == CMD_GET_KEY)
    {
        uint8_t k[4];
        memcpy(k, &node.session_key, 4);
        Send_Up(CMD_GET_KEY, k, 4);
        printf("[KEY] Cle=0x%08lX\r\n", (unsigned long)node.session_key);
        return;
    }

    if (current_state == STATE_ERROR)
    {
        printf("[BLOQUE] Carte en erreur, RESET requis\r\n");
        Send_Up(CMD_ERROR, NULL, 0);
        return;
    }

    // Verification de l'ID de la carte amont
    if (current_state != STATE_NOT_INIT && msg->header.id_src != node.up_id)
    {
        printf("[ERR] Changement de carte amont detecte (attendu 0x%02lX, recu 0x%02lX)\r\n",
               (unsigned long)node.up_id, (unsigned long)msg->header.id_src);
        current_state = STATE_ERROR;
        Send_Up(CMD_ERROR, NULL, 0);
        return;
    }

    if (cmd == CMD_INIT)
    {
        if (current_state != STATE_NOT_INIT)
        {
            current_state = STATE_ERROR;
            Send_Up(CMD_ERROR, NULL, 0);
            return;
        }

        current_state = STATE_SYNCHRONIZING;
        node.up_id = msg->header.id_src;

        uint8_t prev_pos = (msg->header.length >= 1) ? msg->payload[0] : 0;
        node.position = prev_pos + 1;
        node.status = (node.position == 1) ? STATUS_MASTER : STATUS_SLAVE;

        uint8_t my_pos[1] = { node.position };
        uint32_t next_id = 0;
        if (Send_Down_Wait_Ack(CMD_INIT, 0, my_pos, 1, 300, &next_id))
        {
            node.is_exit_node = 0;
            node.down_id = next_id;
        }
        else
        {
            node.is_exit_node = 1;
        }

        Send_Up(CMD_ACK, NULL, 0);

        if (node.status == STATUS_MASTER)
        {
            node.session_key = Security_Random32();
            node.has_key = 1;

            if (!node.is_exit_node)
            {
                uint8_t key_buf[4];
                memcpy(key_buf, &node.session_key, 4);
                uint32_t ack_id = 0;
                if (Send_Down_Wait_Ack(CMD_UPDT_KEY, 0, key_buf, 4, 300, &ack_id) && ack_id == node.down_id)
                {
                    current_state = STATE_SYNCHRONIZED;
                    printf("[OK] Maitre synchronise (cle: 0x%08lX)\r\n", (unsigned long)node.session_key);
                }
                else
                {
                    current_state = STATE_ERROR;
                }
            }
            else
            {
                current_state = STATE_SYNCHRONIZED;
                printf("[OK] Maitre unique synchronise (cle: 0x%08lX)\r\n", (unsigned long)node.session_key);
            }
        }
        else
        {
            printf("[INIT] Esclave pos %d pret, attente cle...\r\n", node.position);
        }
        return;
    }

    if (cmd == CMD_UPDT_KEY)
    {
        if (current_state != STATE_SYNCHRONIZING || node.status != STATUS_SLAVE)
        {
            printf("[ERR] UPDT_KEY recu hors synchro\r\n");
            current_state = STATE_ERROR;
            Send_Up(CMD_ERROR, NULL, 0);
            return;
        }

        memcpy(&node.session_key, msg->payload, 4);
        node.has_key = 1;

        if (!node.is_exit_node)
        {
            uint32_t ack_id = 0;
            if (!Send_Down_Wait_Ack(CMD_UPDT_KEY, 0, msg->payload, 4, 300, &ack_id) || ack_id != node.down_id)
            {
                current_state = STATE_ERROR;
                Send_Up(CMD_ERROR, NULL, 0);
                return;
            }
        }

        current_state = STATE_SYNCHRONIZED;
        Send_Up(CMD_ACK, NULL, 0);
        printf("[OK] Esclave pos %d synchronise (cle: 0x%08lX)\r\n", node.position, (unsigned long)node.session_key);
        return;
    }

    if (cmd == CMD_MSG)
    {
        if (current_state != STATE_SYNCHRONIZED || !node.has_key)
        {
            Send_Up(CMD_ERROR, NULL, 0);
            return;
        }

        uint8_t data[MAX_PAYLOAD_SIZE];
        memcpy(data, msg->payload, MAX_PAYLOAD_SIZE);
        uint8_t nonce = msg->header.reserve;

        if (node.position == 1)
        {
            node.tx_nonce++;
            nonce = node.tx_nonce;
            printf("[TX] Clair: %.16s (nonce=%d)\r\n", data, nonce);
            Security_CryptPayload(data, MAX_PAYLOAD_SIZE, node.session_key, nonce);
        }

        if (!node.is_exit_node)
        {
            uint32_t ack_id = 0;
            if (!Send_Down_Wait_Ack(CMD_MSG, nonce, data, MAX_PAYLOAD_SIZE, 200, &ack_id) || ack_id != node.down_id)
            {
                printf("[ERR] Perte ou changement carte aval\r\n");
                current_state = STATE_ERROR;
                Send_Up(CMD_ERROR, NULL, 0);
                return;
            }
            Send_Up(CMD_ACK, NULL, 0);
        }
        else
        {
            Send_Up(CMD_ACK, NULL, 0);
            Security_CryptPayload(data, MAX_PAYLOAD_SIZE, node.session_key, nonce);
            printf("[RX] Dechiffre: %.16s\r\n", data);

            ProtocolMessage_t out_msg;
            Protocol_BuildMessage(&out_msg, CMD_MSG, node.card_id, data, MAX_PAYLOAD_SIZE);
            HAL_UART_Transmit(&huart6, (uint8_t *)&out_msg, sizeof(ProtocolMessage_t), 100);
        }
    }
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

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);

    if (huart->Instance == USART1)
    {
        rx1_idx = 0;
        HAL_UART_Receive_IT(&huart1, &rx1_byte, 1);
    }
    else if (huart->Instance == USART6)
    {
        rx6_idx = 0;
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