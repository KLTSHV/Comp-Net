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
    MSG_BYE = 6,
    MSG_AUTH = 7,        // аутентификация
    MSG_PRIVATE = 8,     // личное сообщение
    MSG_ERROR = 9,       // ошибка
    MSG_SERVER_INFO = 10 // системные сообщения
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
const char* messageTypeToString(uint8_t type){
    switch(type){
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        default: return "MSG_UNKNOWN";
    }
}