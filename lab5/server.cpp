#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "message_ex.hpp"
#include "netio.hpp"

static const char* HISTORY_FILE = "history.json";

struct Client {
    int sock;
    sockaddr_in addr;
    char nickname[MAX_NAME];
    int authenticated;
    std::mutex send_mutex;
};

struct Task {
    int fd;
    sockaddr_in addr;
};

struct OfflineMsg {
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char text[MAX_PAYLOAD];
    time_t timestamp;
    uint32_t msg_id;
};

struct HistoryRecord {
    uint32_t msg_id;
    time_t timestamp;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string text;
    bool delivered;
    bool is_offline;
};

std::vector<Client*> g_clients;
std::mutex g_clients_mutex;

std::vector<OfflineMsg> g_offline;
std::mutex g_offline_mutex;

std::vector<HistoryRecord> g_history;
std::mutex g_history_mutex;

std::queue<Task> g_tasks;
std::mutex g_tasks_mutex;
std::condition_variable g_tasks_cv;

std::atomic<uint32_t> g_next_msg_id{1};

std::string addrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string jsonUnescape(std::string s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'n') {
                out += '\n';
                ++i;
            } else {
                out += s[i + 1];
                ++i;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

void saveHistoryFileLocked() {
    std::ofstream file(HISTORY_FILE, std::ios::trunc);

    file << "[\n";
    for (size_t i = 0; i < g_history.size(); ++i) {
        const auto& r = g_history[i];

        file << "  {"
             << "\"msg_id\":" << r.msg_id << ","
             << "\"timestamp\":" << r.timestamp << ","
             << "\"sender\":\"" << jsonEscape(r.sender) << "\","
             << "\"receiver\":\"" << jsonEscape(r.receiver) << "\","
             << "\"type\":\"" << jsonEscape(r.type) << "\","
             << "\"text\":\"" << jsonEscape(r.text) << "\","
             << "\"delivered\":" << (r.delivered ? "true" : "false") << ","
             << "\"is_offline\":" << (r.is_offline ? "true" : "false")
             << "}";

        if (i + 1 < g_history.size()) file << ",";
        file << "\n";
    }
    file << "]\n";
}

std::string extractString(const std::string& obj, const std::string& key) {
    std::regex rg("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    std::smatch m;
    if (std::regex_search(obj, m, rg)) return jsonUnescape(m[1].str());
    return "";
}

uint32_t extractUInt(const std::string& obj, const std::string& key) {
    std::regex rg("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch m;
    if (std::regex_search(obj, m, rg)) return static_cast<uint32_t>(std::stoul(m[1].str()));
    return 0;
}

bool extractBool(const std::string& obj, const std::string& key) {
    std::regex rg("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(obj, m, rg)) return m[1].str() == "true";
    return false;
}

void loadHistoryFile() {
    std::ifstream file(HISTORY_FILE);
    if (!file) return;

    std::stringstream ss;
    ss << file.rdbuf();
    std::string data = ss.str();

    std::regex objRg("\\{[^\\}]*\\}");
    auto begin = std::sregex_iterator(data.begin(), data.end(), objRg);
    auto end = std::sregex_iterator();

    uint32_t maxId = 0;

    for (auto it = begin; it != end; ++it) {
        std::string obj = it->str();

        HistoryRecord r{};
        r.msg_id = extractUInt(obj, "msg_id");
        r.timestamp = static_cast<time_t>(extractUInt(obj, "timestamp"));
        r.sender = extractString(obj, "sender");
        r.receiver = extractString(obj, "receiver");
        r.type = extractString(obj, "type");
        r.text = extractString(obj, "text");
        r.delivered = extractBool(obj, "delivered");
        r.is_offline = extractBool(obj, "is_offline");

        g_history.push_back(r);
        maxId = std::max(maxId, r.msg_id);

        if (r.type == "MSG_PRIVATE" && !r.delivered && r.is_offline) {
            OfflineMsg off{};
            copyField(off.sender, MAX_NAME, r.sender);
            copyField(off.receiver, MAX_NAME, r.receiver);
            copyField(off.text, MAX_PAYLOAD, r.text);
            off.timestamp = r.timestamp;
            off.msg_id = r.msg_id;
            g_offline.push_back(off);
        }
    }

    g_next_msg_id.store(maxId + 1);
}

void appendHistory(const MessageEx& msg, bool delivered, bool isOffline) {
    std::lock_guard<std::mutex> lock(g_history_mutex);

    HistoryRecord r{};
    r.msg_id = ntohl(msg.msg_id);
    r.timestamp = msg.timestamp;
    r.sender = msg.sender;
    r.receiver = msg.receiver;
    r.type = typeToString(msg.type);
    r.text = msg.payload;
    r.delivered = delivered;
    r.is_offline = isOffline;

    g_history.push_back(r);
    saveHistoryFileLocked();
}

void markHistoryDelivered(uint32_t msgId) {
    std::lock_guard<std::mutex> lock(g_history_mutex);

    for (auto& r : g_history) {
        if (r.msg_id == msgId) {
            r.delivered = true;
            break;
        }
    }

    saveHistoryFileLocked();
}

std::string getLastHistoryText(size_t count) {
    std::lock_guard<std::mutex> lock(g_history_mutex);

    if (g_history.empty()) return "";

    if (count == 0 || count > g_history.size()) {
        count = g_history.size();
    }

    size_t start = g_history.size() - count;
    std::ostringstream out;

    for (size_t i = start; i < g_history.size(); ++i) {
        const auto& r = g_history[i];

        out << "[" << formatTime(r.timestamp) << "]"
            << "[id=" << r.msg_id << "]";

        if (r.type == "MSG_PRIVATE") {
            if (r.is_offline) out << "[OFFLINE]";
            out << "[PRIVATE][" << r.sender << " -> " << r.receiver << "]: ";
        } else {
            out << "[" << r.sender << "]: ";
        }

        out << r.text << "\n";
    }

    return out.str();
}

void logIncomingTcpIp(const MessageEx& msg, const sockaddr_in& src, const sockaddr_in& dst) {
    std::cout << "[Network Access] frame received via network interface\n";
    std::cout << "[Internet] src=" << addrToString(src)
              << " dst=" << addrToString(dst)
              << " proto=TCP\n";
    std::cout << "[Transport] recv() " << sizeof(MessageEx) << " bytes via TCP\n";
    std::cout << "[Application] deserialize MessageEx -> "
              << typeToString(msg.type) << "\n";
}

void logOutgoingTcpIp(const MessageEx& msg, const sockaddr_in& dst) {
    std::cout << "[Application] prepare " << typeToString(msg.type) << "\n";
    std::cout << "[Transport] send() " << sizeof(MessageEx) << " bytes via TCP\n";
    std::cout << "[Internet] destination=" << addrToString(dst) << " proto=TCP\n";
    std::cout << "[Network Access] frame sent to network interface\n";
}

bool recvMessageEx(int fd, MessageEx& msg, const sockaddr_in& src, const sockaddr_in& dst) {
    if (!recvAll(fd, &msg, sizeof(MessageEx))) return false;

    uint32_t len = ntohl(msg.length);
    if (len != sizeof(MessageEx) - sizeof(uint32_t)) return false;

    logIncomingTcpIp(msg, src, dst);
    return true;
}

bool sendMessageEx(Client* client, const MessageEx& msg) {
    logOutgoingTcpIp(msg, client->addr);

    std::lock_guard<std::mutex> lock(client->send_mutex);
    return sendAll(client->sock, &msg, sizeof(MessageEx));
}

Client* findClientByNick(const std::string& nick) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);

    for (Client* c : g_clients) {
        if (c->authenticated && nick == c->nickname) {
            return c;
        }
    }

    return nullptr;
}

bool isNickUnique(const std::string& nick) {
    return findClientByNick(nick) == nullptr;
}

void addClient(Client* c) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.push_back(c);
}

void removeClient(Client* c) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), c), g_clients.end());
}

uint32_t nextMsgId() {
    return g_next_msg_id.fetch_add(1);
}

MessageEx makeServerMessage(uint8_t type, const std::string& receiver, const std::string& text) {
    MessageEx msg{};
    buildMessageEx(msg, type, nextMsgId(), "SERVER", receiver, text);
    return msg;
}

void sendServerInfo(Client* c, const std::string& text) {
    MessageEx msg = makeServerMessage(MSG_SERVER_INFO, c->nickname, text);
    sendMessageEx(c, msg);
}

void sendError(Client* c, const std::string& text) {
    MessageEx msg = makeServerMessage(MSG_ERROR, c->nickname, text);
    sendMessageEx(c, msg);
}

void broadcastServerInfo(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);

    for (Client* c : g_clients) {
        if (!c->authenticated) continue;
        MessageEx msg = makeServerMessage(MSG_SERVER_INFO, c->nickname, text);
        sendMessageEx(c, msg);
    }
}

void broadcastText(Client* sender, const std::string& text) {
    MessageEx msg{};
    buildMessageEx(
        msg,
        MSG_TEXT,
        nextMsgId(),
        sender->nickname,
        "",
        text
    );

    appendHistory(msg, true, false);

    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (Client* c : g_clients) {
        if (!c->authenticated) continue;
        sendMessageEx(c, msg);
    }
}

void handlePrivate(Client* sender, const MessageEx& in) {
    std::string receiver = in.receiver;
    std::string text = in.payload;

    if (receiver.empty() || text.empty()) {
        sendError(sender, "Invalid private message");
        return;
    }

    MessageEx out{};
    buildMessageEx(
        out,
        MSG_PRIVATE,
        nextMsgId(),
        sender->nickname,
        receiver,
        text
    );

    Client* target = findClientByNick(receiver);

    if (target) {
        sendMessageEx(target, out);
        appendHistory(out, true, false);
        sendServerInfo(sender, "Private message delivered to [" + receiver + "]");
    } else {
        OfflineMsg off{};
        copyField(off.sender, MAX_NAME, sender->nickname);
        copyField(off.receiver, MAX_NAME, receiver);
        copyField(off.text, MAX_PAYLOAD, text);
        off.timestamp = out.timestamp;
        off.msg_id = ntohl(out.msg_id);

        {
            std::lock_guard<std::mutex> lock(g_offline_mutex);
            g_offline.push_back(off);
        }

        appendHistory(out, false, true);

        std::cout << "[Application] receiver " << receiver << " is offline\n";
        std::cout << "[Application] store message in offline queue\n";
        std::cout << "[Application] append record to history file delivered=false\n";

        sendServerInfo(sender, "User [" + receiver + "] is offline. Message saved.");
    }
}

void deliverOfflineMessages(Client* client) {
    std::lock_guard<std::mutex> lock(g_offline_mutex);

    bool any = false;
    auto it = g_offline.begin();

    while (it != g_offline.end()) {
        if (std::string(it->receiver) == client->nickname) {
            any = true;

            MessageEx msg{};
            buildMessageEx(
                msg,
                MSG_PRIVATE,
                it->msg_id,
                it->sender,
                it->receiver,
                std::string("[OFFLINE] ") + it->text
            );
            msg.timestamp = it->timestamp;

            if (sendMessageEx(client, msg)) {
                markHistoryDelivered(it->msg_id);
                it = g_offline.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }

    if (!any) {
        std::cout << "[Application] no offline messages for "
                  << client->nickname << "\n";
    }
}

void handleList(Client* client) {
    std::ostringstream out;
    out << "Online users:\n";

    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (Client* c : g_clients) {
            if (c->authenticated) {
                out << c->nickname << "\n";
            }
        }
    }

    MessageEx msg = makeServerMessage(MSG_SERVER_INFO, client->nickname, out.str());
    sendMessageEx(client, msg);
}

void handleHistory(Client* client, const std::string& param) {
    size_t count = 10;

    if (!param.empty()) {
        try {
            int n = std::stoi(param);
            if (n <= 0) {
                sendError(client, "History parameter must be positive");
                return;
            }
            count = static_cast<size_t>(n);
        } catch (...) {
            sendError(client, "Invalid history parameter");
            return;
        }
    }

    std::string text = getLastHistoryText(count);
    if (text.empty()) text = "History is empty\n";

    MessageEx msg = makeServerMessage(MSG_HISTORY_DATA, client->nickname, text);
    sendMessageEx(client, msg);
}

bool doHandshake(Client* client, const sockaddr_in& serverAddr) {
    MessageEx hello{};

    if (!recvMessageEx(client->sock, hello, client->addr, serverAddr)) return false;
    if (hello.type != MSG_HELLO) return false;

    MessageEx welcome = makeServerMessage(MSG_WELCOME, "", "WELCOME");
    return sendMessageEx(client, welcome);
}

bool authenticate(Client* client, const sockaddr_in& serverAddr) {
    MessageEx auth{};

    if (!recvMessageEx(client->sock, auth, client->addr, serverAddr)) return false;

    if (auth.type != MSG_AUTH) {
        sendError(client, "Authentication required");
        return false;
    }

    std::string nick = auth.sender;
    if (nick.empty()) nick = auth.payload;

    if (nick.empty()) {
        sendError(client, "Nickname cannot be empty");
        return false;
    }

    if (nick.size() >= MAX_NAME) {
        sendError(client, "Nickname too long");
        return false;
    }

    if (!isNickUnique(nick)) {
        sendError(client, "Nickname already in use");
        return false;
    }

    copyField(client->nickname, MAX_NAME, nick);
    client->authenticated = 1;

    std::cout << "[Application] authentication success: " << nick << "\n";
    std::cout << "[Application] SYN -> ACK -> READY\n";
    std::cout << "[Application] packets never sleep\n";

    sendServerInfo(client, "Authenticated as [" + nick + "]");
    return true;
}

void handleClient(int fd, sockaddr_in caddr, sockaddr_in serverAddr) {
    Client* client = new Client{};
    client->sock = fd;
    client->addr = caddr;
    client->authenticated = 0;
    client->nickname[0] = '\0';

    std::cout << "Client connected: " << addrToString(caddr) << "\n";

    if (!doHandshake(client, serverAddr)) {
        ::close(fd);
        delete client;
        return;
    }

    addClient(client);

    if (!authenticate(client, serverAddr)) {
        removeClient(client);
        ::close(fd);
        delete client;
        return;
    }

    std::string nick = client->nickname;

    deliverOfflineMessages(client);
    broadcastServerInfo("User [" + nick + "] connected");

    while (true) {
        MessageEx in{};

        if (!recvMessageEx(fd, in, caddr, serverAddr)) {
            break;
        }

        switch (in.type) {
            case MSG_TEXT:
                std::cout << "[Application] handle MSG_TEXT\n";
                broadcastText(client, in.payload);
                break;

            case MSG_PRIVATE:
                std::cout << "[Application] handle MSG_PRIVATE\n";
                handlePrivate(client, in);
                break;

            case MSG_LIST:
                std::cout << "[Application] handle MSG_LIST\n";
                handleList(client);
                break;

            case MSG_HISTORY:
                std::cout << "[Application] handle MSG_HISTORY\n";
                handleHistory(client, in.payload);
                break;

            case MSG_PING: {
                std::cout << "[Application] handle MSG_PING\n";
                MessageEx pong = makeServerMessage(MSG_PONG, client->nickname, "PONG");
                sendMessageEx(client, pong);
                break;
            }

            case MSG_BYE:
                std::cout << "[Application] handle MSG_BYE\n";
                goto finish;

            default:
                sendError(client, "Unknown message type");
                break;
        }
    }

finish:
    broadcastServerInfo("User [" + nick + "] disconnected");
    removeClient(client);
    ::close(fd);
    delete client;

    std::cout << "Client disconnected: " << nick << "\n";
}

void queuePush(const Task& task) {
    {
        std::lock_guard<std::mutex> lock(g_tasks_mutex);
        g_tasks.push(task);
    }
    g_tasks_cv.notify_one();
}

Task queuePop() {
    std::unique_lock<std::mutex> lock(g_tasks_mutex);

    while (g_tasks.empty()) {
        g_tasks_cv.wait(lock);
    }

    Task t = g_tasks.front();
    g_tasks.pop();
    return t;
}

void worker(sockaddr_in serverAddr) {
    while (true) {
        Task task = queuePop();
        handleClient(task.fd, task.addr, serverAddr);
    }
}

int main(int argc, char** argv) {
    int port = 5555;
    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    loadHistoryFile();

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(s, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        ::close(s);
        return 1;
    }

    if (::listen(s, 16) < 0) {
        perror("listen");
        ::close(s);
        return 1;
    }

    sockaddr_in logServerAddr{};
    logServerAddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &logServerAddr.sin_addr);
    logServerAddr.sin_port = htons(static_cast<uint16_t>(port));

    for (int i = 0; i < 10; ++i) {
        std::thread(worker, logServerAddr).detach();
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

        queuePush(Task{c, caddr});
    }

    ::close(s);
    return 0;
}
