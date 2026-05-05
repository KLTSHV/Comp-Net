#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <arpa/inet.h>

#define MAX_PAYLOAD 1024

// length = type + msg_id + payload
// length и msg_id хранятся в network byte order.
static constexpr uint32_t MSG_FIXED_PART = 1 + 4;

#pragma pack(push, 1)
typedef struct {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     payload[MAX_PAYLOAD];
} Message;
#pragma pack(pop)

enum {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,

    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,

    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14,

    MSG_ACK          = 15
};

inline uint32_t getMsgId(const Message& msg) {
    return ntohl(msg.msg_id);
}

inline uint32_t getPayloadLen(const Message& msg) {
    uint32_t len = ntohl(msg.length);

    if (len < MSG_FIXED_PART) {
        return 0;
    }

    uint32_t payloadLen = len - MSG_FIXED_PART;

    if (payloadLen > MAX_PAYLOAD) {
        payloadLen = MAX_PAYLOAD;
    }

    return payloadLen;
}

inline void buildMessage(Message& msg, uint8_t type, uint32_t msgId, const std::string& data) {
    std::memset(&msg, 0, sizeof(msg));

    if (data.size() > MAX_PAYLOAD) {
        throw std::runtime_error("payload too large");
    }

    msg.type = type;
    msg.msg_id = htonl(msgId);

    if (!data.empty()) {
        std::memcpy(msg.payload, data.data(), data.size());
    }

    uint32_t len = MSG_FIXED_PART + static_cast<uint32_t>(data.size());
    msg.length = htonl(len);
}

inline std::string payloadToString(const Message& msg) {
    uint32_t payloadLen = getPayloadLen(msg);
    return std::string(msg.payload, msg.payload + payloadLen);
}

inline const char* messageTypeToString(uint8_t type) {
    switch (type) {
        case MSG_HELLO:        return "MSG_HELLO";
        case MSG_WELCOME:      return "MSG_WELCOME";
        case MSG_TEXT:         return "MSG_TEXT";
        case MSG_PING:         return "MSG_PING";
        case MSG_PONG:         return "MSG_PONG";
        case MSG_BYE:          return "MSG_BYE";
        case MSG_AUTH:         return "MSG_AUTH";
        case MSG_PRIVATE:      return "MSG_PRIVATE";
        case MSG_ERROR:        return "MSG_ERROR";
        case MSG_SERVER_INFO:  return "MSG_SERVER_INFO";
        case MSG_LIST:         return "MSG_LIST";
        case MSG_HISTORY:      return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP:         return "MSG_HELP";
        case MSG_ACK:          return "MSG_ACK";
        default:               return "MSG_UNKNOWN";
    }
}