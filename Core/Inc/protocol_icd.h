#ifndef PROTOCOL_ICD_H
#define PROTOCOL_ICD_H

#include <stdint.h>

#define ICD_START_BYTE   0x7E
#define MAX_PAYLOAD_SIZE 16
#define DEFAULT_ID       0x0
#define ICD_FRAME_SIZE   26

// Flags
typedef enum {
    CMD_INIT     = 0x00, // Initialisation
    CMD_RESET    = 0x01, // Réinitialisation
    CMD_ACK      = 0x02, // Bonne réception du message
    CMD_MSG      = 0x03, // Transmission d'un message texte
    CMD_ERROR    = 0x04, // Erreur
    CMD_GET_INFO = 0x05, // Demande infos noeud (ID, position, statut)
    CMD_GET_KEY  = 0x06, // Demande cle de session
    CMD_UPDT_KEY = 0x07  // Distribution cle maitre -> esclaves
} CommandType_t;

typedef enum {
    STATUS_UNKNOWN = 0,
    STATUS_MASTER  = 1,
    STATUS_SLAVE   = 2
} NodeStatus_t;

// Header
typedef struct __attribute__((packed)) {
    uint8_t  start_byte;     // Octet de début de trame (0x7E)
    uint8_t  command_type;   // Type de commande issue de CommandType_t (1 octet)
    uint8_t  length;         // Taille utile du payload (1 octet)
    uint8_t  reserve;        // Reserve d'alignement (utilisee pour le nonce)
    uint32_t id_src;         // Identifiant de la source (4 octets)
} MessageHeader_t;

// La trame complète circulant entre les nœuds
typedef struct __attribute__((packed)) {
    MessageHeader_t header;
    uint8_t         payload[MAX_PAYLOAD_SIZE]; // Les données (16 octets max)
    uint16_t        crc;
} ProtocolMessage_t;

uint16_t Protocol_ComputeCRC(const ProtocolMessage_t *msg);
void     Protocol_BuildMessage(ProtocolMessage_t *msg, CommandType_t cmd, uint32_t id_src, const uint8_t *payload, uint8_t len);
uint8_t  Protocol_VerifyMessage(const ProtocolMessage_t *msg);

#endif // PROTOCOL_ICD_H
