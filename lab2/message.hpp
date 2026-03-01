#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <arpa/inet.h>

#define MAX_PAYLOAD 1024

#pragma pack(push, 1)
typedef struct {
    uint32_t length;               // длина поля type + payload (в байтах)
    uint8_t  type;                 // тип сообщения
    char     payload[MAX_PAYLOAD]; // данные
} Message;
#pragma pack(pop)

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6
};

void buildMessage(Message& msg, uint8_t type, const std::string& data) {
    std::memset(&msg, 0, sizeof(msg));
    msg.type = type;

    if (data.size() > MAX_PAYLOAD) {
        throw std::runtime_error("payload too large");
    }

    if (!data.empty()) {
        std::memcpy(msg.payload, data.data(), data.size());
    }

    uint32_t len = 1u + static_cast<uint32_t>(data.size());
    msg.length = htonl(len);
}

std::string payloadToString(const Message& msg) {
    uint32_t len = ntohl(msg.length);
    if (len < 1) return {};
    uint32_t payloadLen = len - 1;
    if (payloadLen > MAX_PAYLOAD) payloadLen = MAX_PAYLOAD;
    return std::string(msg.payload, msg.payload + payloadLen);
}