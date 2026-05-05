#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <random>
#include <fstream>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <pthread.h>

#include "message_ex.hpp"
#include "netio.hpp"

struct Client {
    int sock;
    sockaddr_in addr;
    char nickname[32];
    int authenticated;

    pthread_mutex_t send_mutex;
    pthread_mutex_t ids_mutex;

    uint32_t last_ids[32];
    int last_ids_pos;
};

std::vector<Client*> g_clients;
std::mutex g_clients_mutex;

pthread_mutex_t g_history_mutex = PTHREAD_MUTEX_INITIALIZER;

int g_delayMs = 0;
double g_dropRate = 0.0;
double g_corruptRate = 0.0;

std::mt19937 g_rng(std::random_device{}());

double random01() {
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(g_rng);
}

std::string addrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));

    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

void logRecvTransport(const Message& msg) {
    std::cout << "[Transport] recv "
              << messageTypeToString(msg.type)
              << " (id=" << getMsgId(msg) << ")\n";
}

void logSendTransport(const Message& msg) {
    std::cout << "[Transport] send "
              << messageTypeToString(msg.type)
              << " (id=" << getMsgId(msg) << ")\n";
}

void logApplication(const std::string& text) {
    std::cout << "[Application] " << text << "\n";
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

    g_clients.erase(
        std::remove(g_clients.begin(), g_clients.end(), client),
        g_clients.end()
    );
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

bool safeSend(Client* client, const Message& msg) {
    pthread_mutex_lock(&client->send_mutex);

    logSendTransport(msg);
    bool ok = sendMessage(client->sock, msg);

    pthread_mutex_unlock(&client->send_mutex);

    return ok;
}

bool sendToClient(Client* client, uint8_t type, uint32_t id, const std::string& text) {
    Message out{};
    buildMessage(out, type, id, text);

    return safeSend(client, out);
}

bool sendAck(Client* client, uint32_t id) {
    Message ack{};
    buildMessage(ack, MSG_ACK, id, "");

    std::cout << "[Transport][ACK] send MSG_ACK (id="
              << id << ")\n";

    return safeSend(client, ack);
}

void broadcastServerInfo(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);

    for (auto* c : g_clients) {
        if (!c->authenticated) {
            continue;
        }

        Message out{};
        buildMessage(out, MSG_SERVER_INFO, 0, text);

        pthread_mutex_lock(&c->send_mutex);

        logSendTransport(out);
        sendMessage(c->sock, out);

        pthread_mutex_unlock(&c->send_mutex);
    }
}

void broadcastText(Client* sender, const std::string& text) {
    std::string full = "[" + std::string(sender->nickname) + "]: " + text;

    std::lock_guard<std::mutex> lock(g_clients_mutex);

    for (auto* c : g_clients) {
        if (!c->authenticated) {
            continue;
        }

        Message out{};
        buildMessage(out, MSG_TEXT, getMsgId(out), full);
        buildMessage(out, MSG_TEXT, 0, full);

        pthread_mutex_lock(&c->send_mutex);

        logSendTransport(out);
        sendMessage(c->sock, out);

        pthread_mutex_unlock(&c->send_mutex);
    }
}

void saveHistory(const std::string& nick, uint32_t id, const std::string& text) {
    pthread_mutex_lock(&g_history_mutex);

    std::ofstream out("history.json", std::ios::app);

    out << "{"
        << "\"msg_id\":" << id << ","
        << "\"nick\":\"" << nick << "\","
        << "\"text\":\"" << text << "\","
        << "\"delivered\":true"
        << "}\n";

    pthread_mutex_unlock(&g_history_mutex);
}

bool handlePrivateMessage(Client* sender, const std::string& payload) {
    size_t pos = payload.find(':');

    if (pos == std::string::npos || pos == 0 || pos + 1 >= payload.size()) {
        return sendToClient(
            sender,
            MSG_ERROR,
            0,
            "Invalid private message format. Use target_nick:message"
        );
    }

    std::string targetNick = payload.substr(0, pos);
    std::string text = payload.substr(pos + 1);

    Client* target = findClientByNick(targetNick);

    if (!target) {
        return sendToClient(
            sender,
            MSG_ERROR,
            0,
            "User [" + targetNick + "] not found"
        );
    }

    std::string full = "[PRIVATE][" + std::string(sender->nickname) + "]: " + text;

    return sendToClient(target, MSG_PRIVATE, 0, full);
}

bool simulateNetwork(Message& msg) {
    uint32_t id = getMsgId(msg);

    if (g_delayMs > 0) {
        std::cout << "[Transport][SIM] DELAY applied: "
                  << g_delayMs << " ms\n";

        usleep(g_delayMs * 1000);
    }

    if (g_dropRate > 0.0 && random01() < g_dropRate) {
        std::cout << "[Transport][SIM] DROP (id="
                  << id << ", rate=" << g_dropRate << ")\n";
        return false;
    }

    uint32_t payloadLen = getPayloadLen(msg);

    if (g_corruptRate > 0.0 && payloadLen > 0 && random01() < g_corruptRate) {
        std::uniform_int_distribution<int> posDist(0, static_cast<int>(payloadLen - 1));
        int pos = posDist(g_rng);

        msg.payload[pos] ^= 0x01;

        std::cout << "[Transport][SIM] CORRUPT payload (id="
                  << id << ")\n";
    }

    return true;
}

bool isDuplicate(Client* client, uint32_t id) {
    if (id == 0) {
        return false;
    }

    bool duplicate = false;

    pthread_mutex_lock(&client->ids_mutex);

    for (uint32_t oldId : client->last_ids) {
        if (oldId == id) {
            duplicate = true;
            break;
        }
    }

    pthread_mutex_unlock(&client->ids_mutex);

    return duplicate;
}

void rememberId(Client* client, uint32_t id) {
    if (id == 0) {
        return;
    }

    pthread_mutex_lock(&client->ids_mutex);

    client->last_ids[client->last_ids_pos] = id;
    client->last_ids_pos = (client->last_ids_pos + 1) % 32;

    pthread_mutex_unlock(&client->ids_mutex);
}

bool doHandshakeHelloWelcome(int fd) {
    Message hello{};

    if (!recvMessage(fd, hello)) {
        return false;
    }

    logRecvTransport(hello);
    logApplication("handle initial handshake");

    if (hello.type != MSG_HELLO) {
        return false;
    }

    Message welcome{};
    buildMessage(welcome, MSG_WELCOME, 0, "WELCOME");

    logSendTransport(welcome);

    return sendMessage(fd, welcome);
}

bool authenticateClient(Client* client) {
    Message auth{};

    if (!recvMessage(client->sock, auth)) {
        return false;
    }

    logRecvTransport(auth);

    if (auth.type != MSG_AUTH) {
        sendToClient(client, MSG_ERROR, 0, "Authentication required");
        return false;
    }

    std::string nick = payloadToString(auth);

    logApplication("authentication request");

    if (nick.empty()) {
        sendToClient(client, MSG_ERROR, 0, "Nickname cannot be empty");
        return false;
    }

    if (nick.size() >= sizeof(client->nickname)) {
        sendToClient(client, MSG_ERROR, 0, "Nickname too long");
        return false;
    }

    if (!isNicknameUnique(nick)) {
        sendToClient(client, MSG_ERROR, 0, "Nickname already in use");
        return false;
    }

    std::strncpy(client->nickname, nick.c_str(), sizeof(client->nickname) - 1);
    client->nickname[sizeof(client->nickname) - 1] = '\0';
    client->authenticated = 1;

    logApplication("authentication success");

    sendToClient(client, MSG_SERVER_INFO, 0, "Authenticated as [" + nick + "]");

    return true;
}

void destroyClient(Client* client) {
    pthread_mutex_destroy(&client->send_mutex);
    pthread_mutex_destroy(&client->ids_mutex);
    delete client;
}

void handleClient(int fd, sockaddr_in caddr) {
    std::string peer = addrToString(caddr);

    std::cout << "Client connected: " << peer << "\n";

    Client* self = new Client{};

    self->sock = fd;
    self->addr = caddr;
    self->nickname[0] = '\0';
    self->authenticated = 0;
    self->last_ids_pos = 0;

    std::memset(self->last_ids, 0, sizeof(self->last_ids));

    pthread_mutex_init(&self->send_mutex, nullptr);
    pthread_mutex_init(&self->ids_mutex, nullptr);

    if (!doHandshakeHelloWelcome(fd)) {
        std::cout << "Handshake failed: " << peer << "\n";
        ::close(fd);
        destroyClient(self);
        return;
    }

    addClient(self);

    if (!authenticateClient(self)) {
        std::cout << "Authentication failed: " << peer << "\n";
        removeClient(self);
        ::close(fd);
        destroyClient(self);
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

        logRecvTransport(in);

        if (!simulateNetwork(in)) {
            continue;
        }

        uint32_t id = getMsgId(in);

        if (in.type == MSG_TEXT || in.type == MSG_PRIVATE || in.type == MSG_PING) {
            if (isDuplicate(self, id)) {
                std::cout << "[Application][DEDUP] duplicate ignored (id="
                          << id << ")\n";

                sendAck(self, id);
                continue;
            }

            rememberId(self, id);
        }

        switch (in.type) {
            case MSG_TEXT: {
                std::string text = payloadToString(in);

                std::cout << "[Application][ACK] process MSG_TEXT (id="
                          << id << ")\n";

                broadcastText(self, text);
                saveHistory(self->nickname, id, text);
                sendAck(self, id);

                break;
            }

            case MSG_PRIVATE: {
                std::cout << "[Application][ACK] process MSG_PRIVATE (id="
                          << id << ")\n";

                handlePrivateMessage(self, payloadToString(in));
                sendAck(self, id);

                break;
            }

            case MSG_PING: {
                std::cout << "[Transport][PING] recv MSG_PING (id="
                          << id << ")\n";

                Message pong{};
                buildMessage(pong, MSG_PONG, id, "");

                std::cout << "[Transport][PING] send MSG_PONG (id="
                          << id << ")\n";

                safeSend(self, pong);
                sendAck(self, id);

                break;
            }

            case MSG_BYE: {
                logApplication("handle MSG_BYE");
                goto finish;
            }

            default: {
                logApplication("unknown message type");
                sendToClient(self, MSG_ERROR, 0, "Unknown message type");
                break;
            }
        }
    }

finish:
    broadcastServerInfo("User [" + nick + "] disconnected");

    removeClient(self);

    ::close(fd);

    destroyClient(self);

    std::cout << "Connection closed: " << peer << "\n";
}

int main(int argc, char** argv) {
    int port = 5555;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.rfind("--delay=", 0) == 0) {
            g_delayMs = std::stoi(arg.substr(8));
        } else if (arg.rfind("--drop=", 0) == 0) {
            g_dropRate = std::stod(arg.substr(7));
        } else if (arg.rfind("--corrupt=", 0) == 0) {
            g_corruptRate = std::stod(arg.substr(10));
        } else {
            port = std::stoi(arg);
        }
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

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
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
    std::cout << "SIM delay=" << g_delayMs
              << "ms drop=" << g_dropRate
              << " corrupt=" << g_corruptRate << "\n";

    while (true) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);

        int c = ::accept(s, reinterpret_cast<sockaddr*>(&caddr), &clen);

        if (c < 0) {
            perror("accept");
            continue;
        }

        std::thread(handleClient, c, caddr).detach();
    }

    ::close(s);
    pthread_mutex_destroy(&g_history_mutex);

    return 0;
}