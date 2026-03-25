#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
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

static int connectToServer(const std::string& ip, int port) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Bad ip\n";
        ::close(s);
        return -1;
    }

    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        ::close(s);
        return -1;
    }

    return s;
}

static bool performHandshake(int s, const std::string& nick) {
    Message hello{};
    buildMessage(hello, MSG_HELLO, nick);

    if (!sendMessage(s, hello)) {
        std::cerr << "Failed to send HELLO\n";
        return false;
    }

    Message welcome{};
    if (!recvMessage(s, welcome) || welcome.type != MSG_WELCOME) {
        std::cerr << "Expected WELCOME\n";
        return false;
    }

    std::cout << payloadToString(welcome) << "\n";
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

    std::atomic<bool> running{true};
    std::atomic<bool> connected{false};

    int sock = -1;
    std::mutex sockMutex;

    auto safeCloseSocket = [&]() {
        std::lock_guard<std::mutex> lock(sockMutex);
        if (sock >= 0) {
            ::shutdown(sock, SHUT_RDWR);
            ::close(sock);
            sock = -1;
        }
        connected.store(false);
    };

    auto sendCurrentMessage = [&](const Message& msg) -> bool {
        std::lock_guard<std::mutex> lock(sockMutex);

        if (sock < 0) {
            return false;
        }

        if (!sendMessage(sock, msg)) {
            ::shutdown(sock, SHUT_RDWR);
            ::close(sock);
            sock = -1;
            connected.store(false);
            return false;
        }

        return true;
    };

    auto connectAndHandshake = [&]() -> bool {
        int newSock = connectToServer(ip, port);
        if (newSock < 0) {
            return false;
        }

        if (!performHandshake(newSock, nick)) {
            ::close(newSock);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(sockMutex);
            sock = newSock;
        }

        connected.store(true);
        std::cout << "Connected\n";
        return true;
    };

    while (running.load() && !connectAndHandshake()) {
        std::cout << "Reconnect in 2 seconds...\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    std::thread rx([&]() {
        while (running.load()) {
            if (!connected.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            int localSock = -1;
            {
                std::lock_guard<std::mutex> lock(sockMutex);
                localSock = sock;
            }

            if (localSock < 0) {
                connected.store(false);
                continue;
            }

            Message in{};
            if (!recvMessage(localSock, in)) {
                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    if (sock == localSock) {
                        ::shutdown(sock, SHUT_RDWR);
                        ::close(sock);
                        sock = -1;
                    }
                }

                connected.store(false);

                if (!running.load()) {
                    break;
                }

                std::cout << "\nConnection lost. Reconnecting...\n";

                while (running.load() && !connected.load()) {
                    if (connectAndHandshake()) {
                        std::cout << "> " << std::flush;
                        break;
                    }
                    std::cout << "Reconnect in 2 seconds...\n";
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }

                continue;
            }

            if (in.type == MSG_TEXT) {
                std::cout << "\n" << payloadToString(in) << "\n> " << std::flush;
            } else if (in.type == MSG_PONG) {
                std::cout << "\nPONG\n> " << std::flush;
            } else if (in.type == MSG_BYE) {
                std::cout << "\nServer closed connection\n";
                connected.store(false);

                {
                    std::lock_guard<std::mutex> lock(sockMutex);
                    if (sock == localSock) {
                        ::shutdown(sock, SHUT_RDWR);
                        ::close(sock);
                        sock = -1;
                    }
                }

                while (running.load() && !connected.load()) {
                    if (connectAndHandshake()) {
                        std::cout << "> " << std::flush;
                        break;
                    }
                    std::cout << "Reconnect in 2 seconds...\n";
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }
        }
    });

    std::string line;
    while (running.load()) {
        std::cout << "> " << std::flush;

        if (!std::getline(std::cin, line)) {
            running.store(false);
            break;
        }

        if (!connected.load()) {
            std::cout << "Not connected right now\n";
            continue;
        }

        if (line == "/ping") {
            Message ping{};
            buildMessage(ping, MSG_PING, "");

            if (!sendCurrentMessage(ping)) {
                std::cout << "Send failed\n";
            }
        } else if (line == "/quit") {
            Message bye{};
            buildMessage(bye, MSG_BYE, "");
            (void)sendCurrentMessage(bye);

            running.store(false);
            break;
        } else {
            Message txt{};
            buildMessage(txt, MSG_TEXT, line);

            if (!sendCurrentMessage(txt)) {
                std::cout << "Send failed\n";
            }
        }
    }

    running.store(false);
    safeCloseSocket();

    if (rx.joinable()) {
        rx.join();
    }

    std::cout << "Disconnected\n";
    return 0;
}