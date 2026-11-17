#include "protocol_icd.h"
#include <string.h>

uint16_t Protocol_ComputeCRC(const ProtocolMessage_t *msg)
{
    const uint8_t *data = (const uint8_t *)msg;
    uint16_t crc = 0xFFFF;

    // Calcul sur header + payload (24 octets)
    for (uint8_t i = 0; i < (ICD_FRAME_SIZE - 2); i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
        {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

void Protocol_BuildMessage(ProtocolMessage_t *msg, CommandType_t cmd, uint32_t id_src, const uint8_t *payload, uint8_t len)
{
    memset(msg, 0, sizeof(ProtocolMessage_t));
    msg->header.start_byte = ICD_START_BYTE;
    msg->header.command_type = (uint8_t)cmd;
    msg->header.id_src = id_src;

    if (payload != NULL && len > 0)
    {
        if (len > MAX_PAYLOAD_SIZE) len = MAX_PAYLOAD_SIZE;
        msg->header.length = len;
        memcpy(msg->payload, payload, len);
    }

    msg->crc = Protocol_ComputeCRC(msg);
}

uint8_t Protocol_VerifyMessage(const ProtocolMessage_t *msg)
{
    if (msg == NULL) return 0;
    if (msg->header.start_byte != ICD_START_BYTE) return 0;
    if (msg->header.length > MAX_PAYLOAD_SIZE) return 0;

    return (Protocol_ComputeCRC(msg) == msg->crc);
}
