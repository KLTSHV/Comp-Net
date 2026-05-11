#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "message.hpp"
#include "netio.hpp"

static bool recvMessage(int fd, Message& msg) {
    uint32_t netLen = 0;
    uint8_t type = 0;

    if (!recvAll(fd, &netLen, sizeof(netLen))) return false;
    if (!recvAll(fd, &type, sizeof(type))) return false;

    uint32_t len = ntohl(netLen);
    if (len < 1) return false;

    uint32_t payloadLen = len - 1;
    if (payloadLen > MAX_PAYLOAD) return false;

    std::memset(&msg, 0, sizeof(msg));
    msg.length = netLen;
    msg.type = type;

    if (payloadLen > 0) {
        if (!recvAll(fd, msg.payload, payloadLen)) return false;
    }
    return true;
}

static bool sendMessage(int fd, const Message& msg) {
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
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <ip> <port> <nick>\n";
        return 1;
    }
    std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    std::string nick = argv[3];

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Bad ip\n";
        ::close(s);
        return 1;
    }

    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        ::close(s);
        return 1;
    }

    std::cout << "Connected\n";

    // HELLO
    Message hello{};
    buildMessage(hello, MSG_HELLO, nick);
    if (!sendMessage(s, hello)) {
        std::cerr << "Failed to send HELLO\n";
        ::close(s);
        return 1;
    }

    // WELCOME
    Message welcome{};
    if (!recvMessage(s, welcome) || welcome.type != MSG_WELCOME) {
        std::cerr << "Expected WELCOME\n";
        ::close(s);
        return 1;
    }
    std::cout << payloadToString(welcome) << "\n";

    std::atomic<bool> running{true};

    // Поток приёма
    std::thread rx([&]{
        while (running.load()) {
            Message in{};
            if (!recvMessage(s, in)) {
                running.store(false);
                break;
            }

            if (in.type == MSG_TEXT) {
                std::cout << payloadToString(in) << "\n";
            } else if (in.type == MSG_PONG) {
                std::cout << "PONG\n";
            } else if (in.type == MSG_BYE) {
                running.store(false);
                break;
            }
        }
    });

    // Поток ввода и отправки в main
    std::string line;
    while (running.load()) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            // EOF
            break;
        }

        if (line == "/ping") {
            Message ping{};
            buildMessage(ping, MSG_PING, "");
            if (!sendMessage(s, ping)) {
                running.store(false);
                break;
            }
        } else if (line == "/quit") {
            Message bye{};
            buildMessage(bye, MSG_BYE, "");
            (void)sendMessage(s, bye);
            running.store(false);
            break;
        } else {
            Message txt{};
            buildMessage(txt, MSG_TEXT, line);
            if (!sendMessage(s, txt)) {
                running.store(false);
                break;
            }
        }
    }

    running.store(false);
    ::shutdown(s, SHUT_RDWR);
    ::close(s);

    if (rx.joinable()) rx.join();

    std::cout << "Disconnected\n";
    return 0;
}