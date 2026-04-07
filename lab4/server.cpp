#include <iostream>
#include <string>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "message.hpp"
#include "netio.hpp"



struct Task {
    int fd;
    sockaddr_in addr;
};
struct Client{
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
    uint16_t port = ntohs(a.sin_port);
    return std::string(ip) + ":" + std::to_string(port);
}

void logRecvTransport(){
    std::cout << "[Layer 4 - Transport] recv()\n";
}
void logRecvPresentation(const Message& msg){
    std::cout << "[Layer 6 - Presentation] deserialize Message -> " << messageTypeToString(msg.type) << "\n";
}
void logSession(const std::string& text){
    std::cout << "[Layer 5 - Session] " << text << "\n";
}
void logApplication(const std::string& text){
    std::cout << "[Layer 7 - Application] " << text << "\n";
}
void logSendPresentation(const Message& msg){
    std::cout << "[Layer 6 - Presentation] serialize Message -> " << messageTypeToString(msg.type) << "\n";
}
void logSendTransport(){
    std::cout << "[Layer 4 - Transport] send()\n";
}

bool recvMessage(int fd, Message& msg) {
    logRecvTransport();
    
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
bool isNicknameUnique(const std::string& nick){
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for(auto* c : g_clients){
        if(c->authenticated && nick == c->nickname){
            return false;
        }
    }
    return true;

}
void addClient(Client* client){
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.push_back(client);
}
void removeClient(Client* client){
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), client), g_clients.end());
}
Client* findClientByNick(const std::string& nick){
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for(auto* c : g_clients){
        if(c->authenticated && nick == c->nickname){
            return c;
        }
    }
    return nullptr;
}
void queuePush(const Task& task){
    pthread_mutex_lock(&g_queue_mutex);
    g_queue.push(task);
    pthread_cond_signal(&g_queue_not_empty);
    pthread_mutex_unlock(&g_queue_mutex);
}

Task queuePop(){
    pthread_mutex_lock(&g_queue_mutex);
    while(g_queue.empty()){
        pthread_cond_wait(&g_queue_not_empty, &g_queue_mutex);
    }
    Task task = g_queue.front();
    g_queue.pop();
    pthread_mutex_unlock(&g_queue_mutex);
    return task;
}
Client* addClient(int fd, const sockaddr_in& addr, const std::string& nick){
    Client* client = new Client;
    client->fd = fd;
    client->addr = addr;
    client->nick = nick;
    client->next = nullptr;
    pthread_mutex_init(&client->send_mutex, nullptr);
    pthread_mutex_lock(&g_clients_mutex);
    client->next = g_clients;
    g_clients = client;
    pthread_mutex_unlock(&g_clients_mutex);
    return client;
}
void removeClient(Client* client){
    pthread_mutex_lock(&g_clients_mutex);
    Client** cur = &g_clients;
    while (*cur != nullptr){
        if (*cur == client){
            *cur = client->next;
            break;
        }
        cur = &((*cur)->next);
    }
    pthread_mutex_unlock(&g_clients_mutex);
    pthread_mutex_destroy(&client->send_mutex);
    delete client;

}
void broadcastText(Client* sender, const std::string& text){
    std::string peer = addrToString(sender->addr);
    std::string fullText = sender->nick + " [" + peer +"]: " + text;
    Message out{};
    buildMessage(out, MSG_TEXT, fullText);
    pthread_mutex_lock(&g_clients_mutex);

    for(Client* cur = g_clients; cur != nullptr; cur = cur->next){
        pthread_mutex_lock(&cur->send_mutex);
        sendMessage(cur->fd, out);
        pthread_mutex_unlock(&cur->send_mutex);
    }
    pthread_mutex_unlock(&g_clients_mutex);
}
void handleClient(int fd, const sockaddr_in& caddr) {
    std::string peer = addrToString(caddr);
    std::cout << "Client connected: " << peer << "\n";

    Message msg{};
    if (!recvMessage(fd, msg) || msg.type != MSG_HELLO) {
        std::cerr << "Expected HELLO from " << peer << "\n";
        ::close(fd);
        return;
    }

    std::string nick = payloadToString(msg);
    std::cout << "[" << peer << "]: " << nick << "\n";

    Message welcome{};
    buildMessage(welcome, MSG_WELCOME, "Welcome " + nick);

    if (!sendMessage(fd, welcome)) {
        std::cerr << "Failed to send WELCOME to " << peer << "\n";
        ::close(fd);
        return;
    }

    Client* self = addClient(fd, caddr, nick);

    while (true) {
        Message in{};
        if (!recvMessage(fd, in)) {
            std::cout << "Client disconnected: " << nick << " [" << peer << "]\n";
            break;
        }

        if (in.type == MSG_TEXT) {
            std::string text = payloadToString(in);
            std::cout << nick << " [" << peer << "]: " << text << "\n";
            broadcastText(self, text);
        } else if (in.type == MSG_PING) {
            Message pong{};
            buildMessage(pong, MSG_PONG, "");

            pthread_mutex_lock(&self->send_mutex);
            bool ok = sendMessage(fd, pong);
            pthread_mutex_unlock(&self->send_mutex);

            if (!ok) {
                std::cout << "Client disconnected: " << nick << " [" << peer << "]\n";
                break;
            }
        } else if (in.type == MSG_BYE) {
            std::cout << "Client disconnected: " << nick << " [" << peer << "]\n";
            break;
        } else {
            std::cout << "Unknown message type from " << nick
                      << " [" << peer << "]: " << int(in.type) << "\n";
        }
    }

    removeClient(self);
    ::close(fd);
}

void* workerThread(void* arg) {
    (void)arg;

    while (true) {
        Task task = queuePop();
        handleClient(task.fd, task.addr);
    }

    return nullptr;
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

    pthread_t workers[10];
    for (int i = 0; i < 10; ++i) {
        if (pthread_create(&workers[i], nullptr, workerThread, nullptr) != 0) {
            std::cerr << "pthread_create failed\n";
            ::close(s);
            return 1;
        }
        pthread_detach(workers[i]);
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

        Task task{};
        task.fd = c;
        task.addr = caddr;

        queuePush(task);
    }

    ::close(s);
    return 0;
}