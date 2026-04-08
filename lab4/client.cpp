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
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <ip> <port>\n";
        return 1;
    }

    std::string ip = argv[1];
    int port = std::stoi(argv[2]);

    std::string nick;
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nick);

    if (nick.empty()) {
        std::cerr << "Nickname cannot be empty\n";
        return 1;
    }

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

    // 1. HELLO
    Message hello{};
    buildMessage(hello, MSG_HELLO, "hello");
    if (!sendMessage(s, hello)) {
        std::cerr << "Failed to send HELLO\n";
        ::close(s);
        return 1;
    }

    // 2. WELCOME
    Message welcome{};
    if (!recvMessage(s, welcome) || welcome.type != MSG_WELCOME) {
        std::cerr << "Expected WELCOME\n";
        ::close(s);
        return 1;
    }

    std::cout << "[SERVER]: " << payloadToString(welcome) << "\n";

    // 3. AUTH
    Message auth{};
    buildMessage(auth, MSG_AUTH, nick);
    if (!sendMessage(s, auth)) {
        std::cerr << "Failed to send AUTH\n";
        ::close(s);
        return 1;
    }

    Message authReply{};
    if (!recvMessage(s, authReply)) {
        std::cerr << "Server closed connection\n";
        ::close(s);
        return 1;
    }

    if (authReply.type == MSG_ERROR) {
        std::cerr << "[SERVER ERROR]: " << payloadToString(authReply) << "\n";
        ::close(s);
        return 1;
    }

    if (authReply.type == MSG_SERVER_INFO) {
        std::cout << "[SERVER]: " << payloadToString(authReply) << "\n";
    }

    std::atomic<bool> running{true};

    std::thread rx([&]() {
        while (running.load()) {
            Message in{};
            if (!recvMessage(s, in)) {
                running.store(false);
                break;
            }

            switch (in.type) {
                case MSG_TEXT:
                    std::cout << payloadToString(in) << "\n";
                    break;
                case MSG_PRIVATE:
                    std::cout << payloadToString(in) << "\n";
                    break;
                case MSG_SERVER_INFO:
                    std::cout << "[SERVER]: " << payloadToString(in) << "\n";
                    break;
                case MSG_ERROR:
                    std::cout << "[SERVER ERROR]: " << payloadToString(in) << "\n";
                    break;
                case MSG_PONG:
                    std::cout << "PONG\n";
                    break;
                case MSG_BYE:
                    running.store(false);
                    break;
                default:
                    std::cout << "[UNKNOWN]: " << payloadToString(in) << "\n";
                    break;
            }
        }
    });

    std::string line;
    while (running.load()) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line == "/quit") {
            Message bye{};
            buildMessage(bye, MSG_BYE, "");
            (void)sendMessage(s, bye);
            running.store(false);
            break;
        } else if (line == "/ping") {
            Message ping{};
            buildMessage(ping, MSG_PING, "");
            if (!sendMessage(s, ping)) {
                running.store(false);
                break;
            }
        } else if (line.rfind("/w ", 0) == 0) {
            size_t firstSpace = line.find(' ', 3);
            if (firstSpace == std::string::npos) {
                std::cout << "Usage: /w <nick> <message>\n";
                continue;
            }
            std::string targetNick = line.substr(3, firstSpace - 3);
            std::string text = line.substr(firstSpace + 1);
            if (targetNick.empty() || text.empty()) {
                std::cout << "Usage: /w <nick> <message>\n";
                continue;
            }
            Message pm{};
            buildMessage(pm, MSG_PRIVATE, targetNick + ":" + text);
            if (!sendMessage(s, pm)) {
                running.store(false);
                break;
            }
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