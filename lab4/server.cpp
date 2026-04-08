#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "message.hpp"
#include "netio.hpp"

struct Client {
    int sock;
    sockaddr_in addr;
    char nickname[32];
    int authenticated;
    std::mutex send_mutex;
};

std::vector<Client*> g_clients;
std::mutex g_clients_mutex;

std::string addrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

void logRecvTransport() {
    std::cout << "[Layer 4 - Transport] recv()\n";
}

void logRecvPresentation(const Message& msg) {
    std::cout << "[Layer 6 - Presentation] deserialize Message -> "
              << messageTypeToString(msg.type) << "\n";
}

void logSession(const std::string& text) {
    std::cout << "[Layer 5 - Session] " << text << "\n";
}

void logApplication(const std::string& text) {
    std::cout << "[Layer 7 - Application] " << text << "\n";
}

void logSendApplication(const std::string& text) {
    std::cout << "[Layer 7 - Application] " << text << "\n";
}

void logSendPresentation(const Message& msg) {
    std::cout << "[Layer 6 - Presentation] serialize Message -> "
              << messageTypeToString(msg.type) << "\n";
}

void logSendTransport() {
    std::cout << "[Layer 4 - Transport] send()\n";
}

bool recvMessage(int fd, Message& msg) {
    logRecvTransport();

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

    logRecvPresentation(msg);
    return true;
}

bool sendMessage(int fd, const Message& msg) {
    logSendPresentation(msg);
    logSendTransport();

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

bool isNicknameUnique(const std::string& nick) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto* c : g_clients) {
        if (c->authenticated && nick == c->nickname) {
            return false;
        }
    }
    return true;
}

void addClient(Client* client) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.push_back(client);
}

void removeClient(Client* client) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), client), g_clients.end());
}

Client* findClientByNick(const std::string& nick) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto* c : g_clients) {
        if (c->authenticated && nick == c->nickname) {
            return c;
        }
    }
    return nullptr;
}

bool sendToClient(Client* client, uint8_t type, const std::string& text, const std::string& appLog) {
    Message out{};
    buildMessage(out, type, text);

    logSendApplication(appLog);

    std::lock_guard<std::mutex> guard(client->send_mutex);
    return sendMessage(client->sock, out);
}

void broadcastServerInfo(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto* c : g_clients) {
        if (!c->authenticated) continue;
        Message out{};
        buildMessage(out, MSG_SERVER_INFO, text);

        logSendApplication("prepare MSG_SERVER_INFO broadcast");

        std::lock_guard<std::mutex> guard(c->send_mutex);
        sendMessage(c->sock, out);
    }
}

void broadcastText(Client* sender, const std::string& text) {
    std::string full = "[" + std::string(sender->nickname) + "]: " + text;

    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto* c : g_clients) {
        if (!c->authenticated) continue;

        Message out{};
        buildMessage(out, MSG_TEXT, full);

        logSendApplication("prepare MSG_TEXT broadcast");

        std::lock_guard<std::mutex> guard(c->send_mutex);
        sendMessage(c->sock, out);
    }
}

bool handlePrivateMessage(Client* sender, const std::string& payload) {
    size_t pos = payload.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= payload.size()) {
        return sendToClient(sender, MSG_ERROR,
                            "Invalid private message format. Use target_nick:message",
                            "prepare MSG_ERROR");
    }

    std::string targetNick = payload.substr(0, pos);
    std::string text = payload.substr(pos + 1);

    Client* target = findClientByNick(targetNick);
    if (!target) {
        return sendToClient(sender, MSG_ERROR,
                            "User [" + targetNick + "] not found",
                            "prepare MSG_ERROR");
    }

    std::string full = "[PRIVATE][" + std::string(sender->nickname) + "]: " + text;

    return sendToClient(target, MSG_PRIVATE, full, "prepare MSG_PRIVATE");
}

bool doHandshakeHelloWelcome(int fd) {
    Message hello{};
    if (!recvMessage(fd, hello)) return false;

    logApplication("handle initial handshake");
    if (hello.type != MSG_HELLO) return false;

    Message welcome{};
    buildMessage(welcome, MSG_WELCOME, "WELCOME");
    logSendApplication("prepare MSG_WELCOME");
    return sendMessage(fd, welcome);
}

bool authenticateClient(Client* client) {
    Message auth{};
    if (!recvMessage(client->sock, auth)) return false;

    if (auth.type != MSG_AUTH) {
        logSession("expected MSG_AUTH");
        sendToClient(client, MSG_ERROR, "Authentication required", "prepare MSG_ERROR");
        return false;
    }

    std::string nick = payloadToString(auth);
    logSession("authentication request");

    if (nick.empty()) {
        logSession("authentication failed: empty nickname");
        sendToClient(client, MSG_ERROR, "Nickname cannot be empty", "prepare MSG_ERROR");
        return false;
    }

    if (nick.size() >= sizeof(client->nickname)) {
        logSession("authentication failed: nickname too long");
        sendToClient(client, MSG_ERROR, "Nickname too long", "prepare MSG_ERROR");
        return false;
    }

    if (!isNicknameUnique(nick)) {
        logSession("authentication failed: nickname already used");
        sendToClient(client, MSG_ERROR, "Nickname already in use", "prepare MSG_ERROR");
        return false;
    }

    std::strncpy(client->nickname, nick.c_str(), sizeof(client->nickname) - 1);
    client->authenticated = 1;

    logSession("authentication success");
    sendToClient(client, MSG_SERVER_INFO, "Authenticated as [" + nick + "]", "prepare MSG_SERVER_INFO");
    return true;
}

void handleClient(int fd, sockaddr_in caddr) {
    std::string peer = addrToString(caddr);
    std::cout << "Client connected: " << peer << "\n";

    Client* self = new Client{};
    self->sock = fd;
    self->addr = caddr;
    self->nickname[0] = '\0';
    self->authenticated = 0;

    if (!doHandshakeHelloWelcome(fd)) {
        std::cout << "Handshake failed: " << peer << "\n";
        ::close(fd);
        delete self;
        return;
    }

    addClient(self);

    if (!authenticateClient(self)) {
        std::cout << "Authentication failed: " << peer << "\n";
        removeClient(self);
        ::close(fd);
        delete self;
        return;
    }

    std::string nick = self->nickname;
    std::cout << "User [" << nick << "] connected\n";
    broadcastServerInfo("User [" + nick + "] connected");

    while (true) {
        Message in{};
        if (!recvMessage(fd, in)) {
            std::cout << "User [" << nick << "] disconnected\n";
            break;
        }

        if (!self->authenticated) {
            logSession("client not authenticated, message ignored");
            continue;
        }

        switch (in.type) {
            case MSG_TEXT: {
                logApplication("handle MSG_TEXT");
                broadcastText(self, payloadToString(in));
                break;
            }
            case MSG_PRIVATE: {
                logApplication("handle MSG_PRIVATE");
                if (!handlePrivateMessage(self, payloadToString(in))) {
                    std::cout << "Private send failed for [" << nick << "]\n";
                }
                break;
            }
            case MSG_PING: {
                logApplication("handle MSG_PING");
                if (!sendToClient(self, MSG_PONG, "", "prepare MSG_PONG")) {
                    std::cout << "User [" << nick << "] disconnected\n";
                    goto finish;
                }
                break;
            }
            case MSG_BYE: {
                logApplication("handle MSG_BYE");
                goto finish;
            }
            default: {
                logApplication("unknown message type");
                sendToClient(self, MSG_ERROR, "Unknown message type", "prepare MSG_ERROR");
                break;
            }
        }
    }

finish:
    broadcastServerInfo("User [" + nick + "] disconnected");
    removeClient(self);
    ::close(fd);
    delete self;
    std::cout << "Connection closed: " << peer << "\n";
}

int main(int argc, char** argv) {
    int port = 5555;
    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

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

    if (::listen(s, 16) < 0) {
        perror("listen");
        ::close(s);
        return 1;
    }

    std::cout << "Server listening on port " << port << "\n";

    while (true) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);

        int c = ::accept(s, (sockaddr*)&caddr, &clen);
        if (c < 0) {
            perror("accept");
            continue;
        }

        std::thread(handleClient, c, caddr).detach();
    }

    ::close(s);
    return 0;
}