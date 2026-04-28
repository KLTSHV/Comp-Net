#pragma once

#include <cstdint>
#include <ctime>
#include <cstring>
#include <string>
#include <arpa/inet.h>

#define MAX_NAME     32
#define MAX_PAYLOAD  256
#define MAX_TIME_STR 32

enum MessageType : uint8_t {
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
    MSG_HELP         = 14
};

struct MessageEx {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    time_t   timestamp;
    char     payload[MAX_PAYLOAD];
};

inline const char* typeToString(uint8_t type) {
    switch (type) {
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP: return "MSG_HELP";
        default: return "MSG_UNKNOWN";
    }
}

inline void copyField(char* dst, size_t dstSize, const std::string& src) {
    std::memset(dst, 0, dstSize);
    std::strncpy(dst, src.c_str(), dstSize - 1);
}

inline void buildMessageEx(
    MessageEx& msg,
    uint8_t type,
    uint32_t msgId,
    const std::string& sender,
    const std::string& receiver,
    const std::string& payload
) {
    std::memset(&msg, 0, sizeof(msg));

    msg.length = htonl(sizeof(MessageEx) - sizeof(uint32_t));
    msg.type = type;
    msg.msg_id = htonl(msgId);
    msg.timestamp = std::time(nullptr);

    copyField(msg.sender, MAX_NAME, sender);
    copyField(msg.receiver, MAX_NAME, receiver);
    copyField(msg.payload, MAX_PAYLOAD, payload);
}

inline std::string formatTime(time_t t) {
    char buf[MAX_TIME_STR];
    std::tm* tm_info = std::localtime(&t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}
