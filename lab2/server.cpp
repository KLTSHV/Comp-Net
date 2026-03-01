#include <iostream>
#include <string>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "message.hpp"
#include "netio.hpp"

std::string addrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    uint16_t port = ntohs(a.sin_port);
    return std::string(ip) + ":" + std::to_string(port);
}

bool recvMessage(int fd, Message& msg) {
    uint32_t netLen = 0;
    uint8_t type = 0;

    if (!recvAll(fd, &netLen, sizeof(netLen))) return false;
    if (!recvAll(fd, &type, sizeof(type))) return false;

    uint32_t len = ntohl(netLen);
    if (len < 1) return false; // некорректно

    uint32_t payloadLen = len - 1;
    if (payloadLen > MAX_PAYLOAD) {
        // слишком много
        return false;
    }

    std::memset(&msg, 0, sizeof(msg));
    msg.length = netLen;
    msg.type = type;

    if (payloadLen > 0) {
        if (!recvAll(fd, msg.payload, payloadLen)) return false;
    }
    return true;
}

bool sendMessage(int fd, const Message& msg) {
    uint32_t len = ntohl(msg.length);
    if (len < 1) return false;
    uint32_t payloadLen = len - 1;
    if (!sendAll(fd, &msg.length, sizeof(msg.length))) return false;
    if (!sendAll(fd, &msg.type, sizeof(msg.type))) return false;
    if (payloadLen > 0) {
        if (!sendAll(fd, msg.payload, payloadLen)) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    int port = 5555;
    if (argc >= 2) port = std::stoi(argv[1]);

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        ::close(s);
        return 1;
    }

    if (::listen(s, 1) < 0) {
        perror("listen");
        ::close(s);
        return 1;
    }

    std::cout << "Server listening on port " << port << "\n";

    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int c = ::accept(s, (sockaddr*)&caddr, &clen);
    if (c < 0) {
        perror("accept");
        ::close(s);
        return 1;
    }

    std::string peer = addrToString(caddr);
    std::cout << "Client connected\n";

    //Получить MSG_HELLO
    Message msg{};
    if (!recvMessage(c, msg) || msg.type != MSG_HELLO) {
        std::cerr << "Expected HELLO\n";
        ::close(c);
        ::close(s);
        return 1;
    }

    std::string nick = payloadToString(msg);
    std::cout << "[" << peer << "]: " << nick << "\n";

    //Отправить MSG_WELCOME
    Message welcome{};
    buildMessage(welcome, MSG_WELCOME, "Welcome " + peer);
    if (!sendMessage(c, welcome)) {
        std::cerr << "Failed to send WELCOME\n";
        ::close(c);
        ::close(s);
        return 1;
    }

    //Цикл обработки
    while (true) {
        Message in{};
        if (!recvMessage(c, in)) {
            std::cout << "Client disconnected\n";
            break;
        }

        if (in.type == MSG_TEXT) {
            std::cout << "[" << peer << "]: " << payloadToString(in) << "\n";
        } else if (in.type == MSG_PING) {
            Message pong{};
            buildMessage(pong, MSG_PONG, "");
            if (!sendMessage(c, pong)) {
                std::cout << "Client disconnected\n";
                break;
            }
        } else if (in.type == MSG_BYE) {
            std::cout << "Client disconnected\n";
            break;
        } else {
            std::cout << "Unknown message type: " << int(in.type) << "\n";
        }
    }

    ::close(c);
    ::close(s);
    return 0;
}