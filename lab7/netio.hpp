#pragma once

#include <cstdint>
#include <cstring>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>

#include "message_ex.hpp"

inline bool recvAll(SSL* ssl, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;

    while (got < n) {
        int r = SSL_read(ssl, p + got, static_cast<int>(n - got));

        if (r <= 0) {
            int err = SSL_get_error(ssl, r);

            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }

            return false;
        }

        got += static_cast<size_t>(r);
    }

    return true;
}

inline bool sendAll(SSL* ssl, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;

    while (sent < n) {
        int r = SSL_write(ssl, p + sent, static_cast<int>(n - sent));

        if (r <= 0) {
            int err = SSL_get_error(ssl, r);

            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }

            return false;
        }

        sent += static_cast<size_t>(r);
    }

    return true;
}

inline bool recvMessage(SSL* ssl, Message& msg) {
    uint32_t netLen = 0;
    uint8_t type = 0;
    uint32_t netId = 0;

    if (!recvAll(ssl, &netLen, sizeof(netLen))) {
        return false;
    }

    if (!recvAll(ssl, &type, sizeof(type))) {
        return false;
    }

    if (!recvAll(ssl, &netId, sizeof(netId))) {
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
        if (!recvAll(ssl, msg.payload, payloadLen)) {
            return false;
        }
    }

    return true;
}

inline bool sendMessage(SSL* ssl, const Message& msg) {
    uint32_t len = ntohl(msg.length);

    if (len < MSG_FIXED_PART) {
        return false;
    }

    uint32_t payloadLen = len - MSG_FIXED_PART;

    if (payloadLen > MAX_PAYLOAD) {
        return false;
    }

    if (!sendAll(ssl, &msg.length, sizeof(msg.length))) {
        return false;
    }

    if (!sendAll(ssl, &msg.type, sizeof(msg.type))) {
        return false;
    }

    if (!sendAll(ssl, &msg.msg_id, sizeof(msg.msg_id))) {
        return false;
    }

    if (payloadLen > 0) {
        if (!sendAll(ssl, msg.payload, payloadLen)) {
            return false;
        }
    }

    return true;
}