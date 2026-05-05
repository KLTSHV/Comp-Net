#pragma once

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include "message_ex.hpp"

inline bool recvAll(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;

    while (got < n) {
        ssize_t r = ::recv(fd, p + got, n - got, 0);

        if (r == 0) {
            return false;
        }

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        got += static_cast<size_t>(r);
    }

    return true;
}

inline bool sendAll(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;

    while (sent < n) {
        ssize_t r = ::send(fd, p + sent, n - sent, 0);

        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }

            return false;
        }

        sent += static_cast<size_t>(r);
    }

    return true;
}

inline bool recvMessage(int fd, Message& msg) {
    uint32_t netLen = 0;
    uint8_t type = 0;
    uint32_t netId = 0;

    if (!recvAll(fd, &netLen, sizeof(netLen))) {
        return false;
    }

    if (!recvAll(fd, &type, sizeof(type))) {
        return false;
    }

    if (!recvAll(fd, &netId, sizeof(netId))) {
        return false;
    }

    uint32_t len = ntohl(netLen);

    if (len < MSG_FIXED_PART) {
        return false;
    }

    uint32_t payloadLen = len - MSG_FIXED_PART;

    if (payloadLen > MAX_PAYLOAD) {
        return false;
    }

    std::memset(&msg, 0, sizeof(msg));

    msg.length = netLen;
    msg.type = type;
    msg.msg_id = netId;

    if (payloadLen > 0) {
        if (!recvAll(fd, msg.payload, payloadLen)) {
            return false;
        }
    }

    return true;
}

inline bool sendMessage(int fd, const Message& msg) {
    uint32_t len = ntohl(msg.length);

    if (len < MSG_FIXED_PART) {
        return false;
    }

    uint32_t payloadLen = len - MSG_FIXED_PART;

    if (payloadLen > MAX_PAYLOAD) {
        return false;
    }

    if (!sendAll(fd, &msg.length, sizeof(msg.length))) {
        return false;
    }

    if (!sendAll(fd, &msg.type, sizeof(msg.type))) {
        return false;
    }

    if (!sendAll(fd, &msg.msg_id, sizeof(msg.msg_id))) {
        return false;
    }

    if (payloadLen > 0) {
        if (!sendAll(fd, msg.payload, payloadLen)) {
            return false;
        }
    }

    return true;
}