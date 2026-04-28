#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <sstream>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "message_ex.hpp"
#include "netio.hpp"

std::atomic<uint32_t> g_msg_id{1};

uint32_t nextMsgId() {
    return g_msg_id.fetch_add(1);
}

bool recvMessageEx(int fd, MessageEx& msg) {
    if (!recvAll(fd, &msg, sizeof(MessageEx))) return false;

    uint32_t len = ntohl(msg.length);
    if (len != sizeof(MessageEx) - sizeof(uint32_t)) return false;

    return true;
}

bool sendMessageEx(int fd, const MessageEx& msg) {
    return sendAll(fd, &msg, sizeof(MessageEx));
}

void printHelp() {
    std::cout << "Available commands:\n"
              << "/help\n"
              << "/list\n"
              << "/history\n"
              << "/history N\n"
              << "/quit\n"
              << "/w <nick> <message>\n"
              << "/ping\n"
              << "Tip: packets never sleep\n";
}

void printIncoming(const MessageEx& msg) {
    uint32_t id = ntohl(msg.msg_id);

    switch (msg.type) {
        case MSG_TEXT:
            std::cout << "[" << formatTime(msg.timestamp) << "]"
                      << "[id=" << id << "]"
                      << "[" << msg.sender << "]: "
                      << msg.payload << "\n";
            break;

        case MSG_PRIVATE:
            std::cout << "[" << formatTime(msg.timestamp) << "]"
                      << "[id=" << id << "]"
                      << "[PRIVATE]"
                      << "[" << msg.sender << " -> " << msg.receiver << "]: "
                      << msg.payload << "\n";
            break;

        case MSG_SERVER_INFO:
            std::cout << "[SERVER]: " << msg.payload << "\n";
            break;

        case MSG_HISTORY_DATA:
            std::cout << msg.payload;
            break;

        case MSG_ERROR:
            std::cout << "[SERVER ERROR]: " << msg.payload << "\n";
            break;

        case MSG_PONG:
            std::cout << "[SERVER]: " << msg.payload << "\n";
            break;

        default:
            std::cout << "[UNKNOWN]: " << msg.payload << "\n";
            break;
    }
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

    MessageEx hello{};
    buildMessageEx(hello, MSG_HELLO, nextMsgId(), nick, "", "HELLO");

    if (!sendMessageEx(s, hello)) {
        std::cerr << "Failed to send HELLO\n";
        ::close(s);
        return 1;
    }

    MessageEx welcome{};
    if (!recvMessageEx(s, welcome) || welcome.type != MSG_WELCOME) {
        std::cerr << "Expected WELCOME\n";
        ::close(s);
        return 1;
    }

    std::cout << "[SERVER]: " << welcome.payload << "\n";

    MessageEx auth{};
    buildMessageEx(auth, MSG_AUTH, nextMsgId(), nick, "", nick);

    if (!sendMessageEx(s, auth)) {
        std::cerr << "Failed to send AUTH\n";
        ::close(s);
        return 1;
    }

    MessageEx authReply{};
    if (!recvMessageEx(s, authReply)) {
        std::cerr << "Server closed connection\n";
        ::close(s);
        return 1;
    }

    printIncoming(authReply);

    if (authReply.type == MSG_ERROR) {
        ::close(s);
        return 1;
    }

    std::atomic<bool> running{true};

    std::thread rx([&]() {
        while (running.load()) {
            MessageEx in{};

            if (!recvMessageEx(s, in)) {
                running.store(false);
                break;
            }

            printIncoming(in);
        }
    });

    std::string line;

    while (running.load()) {
        std::cout << "> " << std::flush;

        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line.empty()) {
            continue;
        }

        if (line == "/help") {
            printHelp();
            continue;
        }

        if (line == "/quit") {
            MessageEx msg{};
            buildMessageEx(msg, MSG_BYE, nextMsgId(), nick, "", "");
            sendMessageEx(s, msg);
            running.store(false);
            break;
        }

        if (line == "/ping") {
            MessageEx msg{};
            buildMessageEx(msg, MSG_PING, nextMsgId(), nick, "", "");
            sendMessageEx(s, msg);
            continue;
        }

        if (line == "/list") {
            MessageEx msg{};
            buildMessageEx(msg, MSG_LIST, nextMsgId(), nick, "", "");
            sendMessageEx(s, msg);
            continue;
        }

        if (line == "/history") {
            MessageEx msg{};
            buildMessageEx(msg, MSG_HISTORY, nextMsgId(), nick, "", "");
            sendMessageEx(s, msg);
            continue;
        }

        if (line.rfind("/history ", 0) == 0) {
            std::string n = line.substr(9);

            try {
                int value = std::stoi(n);
                if (value <= 0) {
                    std::cout << "Usage: /history N, where N > 0\n";
                    continue;
                }
            } catch (...) {
                std::cout << "Usage: /history N, where N is number\n";
                continue;
            }

            MessageEx msg{};
            buildMessageEx(msg, MSG_HISTORY, nextMsgId(), nick, "", n);
            sendMessageEx(s, msg);
            continue;
        }

        if (line.rfind("/w ", 0) == 0) {
            size_t space = line.find(' ', 3);

            if (space == std::string::npos) {
                std::cout << "Usage: /w <nick> <message>\n";
                continue;
            }

            std::string receiver = line.substr(3, space - 3);
            std::string text = line.substr(space + 1);

            if (receiver.empty() || text.empty()) {
                std::cout << "Usage: /w <nick> <message>\n";
                continue;
            }

            MessageEx msg{};
            buildMessageEx(msg, MSG_PRIVATE, nextMsgId(), nick, receiver, text);
            sendMessageEx(s, msg);
            continue;
        }

        MessageEx msg{};
        buildMessageEx(msg, MSG_TEXT, nextMsgId(), nick, "", line);
        sendMessageEx(s, msg);
    }

    running.store(false);
    ::shutdown(s, SHUT_RDWR);
    ::close(s);

    if (rx.joinable()) {
        rx.join();
    }

    std::cout << "Disconnected\n";
    return 0;
}
