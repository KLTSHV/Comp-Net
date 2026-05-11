#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <vector>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cstdlib>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#include "message_ex.hpp"
#include "netio.hpp"

struct PendingMsg {
    Message msg;
    int retries;
    bool acked;
};

struct PingResult {
    uint32_t id;
    bool received;
    double rtt_ms;
    double jitter_ms;
};

std::atomic<bool> g_running{true};
std::atomic<uint32_t> g_nextId{1};

std::string g_nick;

std::map<uint32_t, PendingMsg> g_pending;
pthread_mutex_t g_pending_mutex = PTHREAD_MUTEX_INITIALIZER;

std::map<uint32_t, std::chrono::steady_clock::time_point> g_pingSent;
std::vector<PingResult> g_pingResults;
double g_lastRtt = -1.0;
pthread_mutex_t g_ping_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t nextMsgId() {
    return g_nextId.fetch_add(1);
}

void markAck(uint32_t id) {
    pthread_mutex_lock(&g_pending_mutex);

    auto it = g_pending.find(id);

    if (it != g_pending.end()) {
        it->second.acked = true;
        std::cout << "[Transport][ACK] ACK received (id=" << id << ")\n";
    }

    pthread_mutex_unlock(&g_pending_mutex);
}

bool isAcked(uint32_t id) {
    bool result = false;

    pthread_mutex_lock(&g_pending_mutex);

    auto it = g_pending.find(id);

    if (it != g_pending.end()) {
        result = it->second.acked;
    }

    pthread_mutex_unlock(&g_pending_mutex);

    return result;
}

void removePending(uint32_t id) {
    pthread_mutex_lock(&g_pending_mutex);
    g_pending.erase(id);
    pthread_mutex_unlock(&g_pending_mutex);
}

void addPending(const Message& msg) {
    PendingMsg p{};
    p.msg = msg;
    p.retries = 0;
    p.acked = false;

    pthread_mutex_lock(&g_pending_mutex);
    g_pending[getMsgId(msg)] = p;
    pthread_mutex_unlock(&g_pending_mutex);
}

bool sendReliable(SSL* ssl, const Message& msg) {
    uint32_t id = getMsgId(msg);

    addPending(msg);

    const int maxRetries = 3;
    const int timeoutMs = 2000;
    const int pollMs = 50;

    for (int attempt = 0; attempt <= maxRetries; ++attempt) {
        if (attempt == 0) {
            std::cout << "[Transport][RETRY] send "
                      << messageTypeToString(msg.type)
                      << " (id=" << id << ")\n";
        } else {
            std::cout << "[Transport][RETRY] resend "
                      << attempt << "/" << maxRetries
                      << " (id=" << id << ")\n";
        }

        std::cout << "[Security][ENC] SSL_write "
                  << messageTypeToString(msg.type)
                  << " (id=" << id << ")\n";

        if (!sendMessage(ssl, msg)) {
            removePending(id);
            return false;
        }

        int waited = 0;

        while (waited < timeoutMs) {
            if (isAcked(id)) {
                removePending(id);
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
            waited += pollMs;
        }

        if (!isAcked(id)) {
            std::cout << "[Transport][RETRY] wait ACK timeout (id="
                      << id << ")\n";
        }
    }

    std::cout << "[Transport][RETRY] delivery failed (id="
              << id << ")\n";

    removePending(id);
    return false;
}

void handlePong(const Message& in) {
    uint32_t id = getMsgId(in);
    auto now = std::chrono::steady_clock::now();

    pthread_mutex_lock(&g_ping_mutex);

    auto it = g_pingSent.find(id);

    if (it != g_pingSent.end()) {
        double rtt = std::chrono::duration<double, std::milli>(now - it->second).count();

        double jitter = 0.0;

        if (g_lastRtt >= 0.0) {
            jitter = std::fabs(rtt - g_lastRtt);
        }

        g_lastRtt = rtt;

        g_pingResults.push_back({id, true, rtt, jitter});
        g_pingSent.erase(it);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "PING " << id << " -> RTT=" << rtt << "ms";

        if (g_pingResults.size() > 1) {
            std::cout << " | Jitter=" << jitter << "ms";
        }

        std::cout << "\n";
    }

    pthread_mutex_unlock(&g_ping_mutex);
}

void rxThreadFunc(SSL* ssl) {
    while (g_running.load()) {
        Message in{};

        if (!recvMessage(ssl, in)) {
            g_running.store(false);
            break;
        }

        std::cout << "[Security][ENC] SSL_read "
                  << messageTypeToString(in.type)
                  << " (id=" << getMsgId(in) << ")\n";

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

            case MSG_SECURE_ERROR:
                std::cout << "[SECURE ERROR]: " << payloadToString(in) << "\n";
                break;

            case MSG_TLS_INFO:
                std::cout << "[TLS INFO]: " << payloadToString(in) << "\n";
                break;

            case MSG_PONG:
                handlePong(in);
                break;

            case MSG_ACK:
                markAck(getMsgId(in));
                break;

            case MSG_BYE:
                g_running.store(false);
                break;

            default:
                std::cout << "[UNKNOWN]: " << payloadToString(in) << "\n";
                break;
        }
    }
}

void runPingSeries(SSL* ssl, int count) {
    pthread_mutex_lock(&g_ping_mutex);
    g_pingResults.clear();
    g_pingSent.clear();
    g_lastRtt = -1.0;
    pthread_mutex_unlock(&g_ping_mutex);

    for (int i = 0; i < count; ++i) {
        uint32_t id = nextMsgId();

        Message ping{};
        buildMessage(ping, MSG_PING, id, "");

        pthread_mutex_lock(&g_ping_mutex);
        g_pingSent[id] = std::chrono::steady_clock::now();
        pthread_mutex_unlock(&g_ping_mutex);

        sendReliable(ssl, ping);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    pthread_mutex_lock(&g_ping_mutex);

    for (const auto& p : g_pingSent) {
        std::cout << "PING " << p.first << " -> timeout\n";
        g_pingResults.push_back({p.first, false, 0.0, 0.0});
    }

    g_pingSent.clear();

    pthread_mutex_unlock(&g_ping_mutex);
}

void printNetDiag() {
    pthread_mutex_lock(&g_ping_mutex);

    int sent = static_cast<int>(g_pingResults.size());
    int received = 0;

    double rttSum = 0.0;
    double jitterSum = 0.0;
    int jitterCount = 0;

    bool havePreviousSuccessful = false;

    for (const auto& r : g_pingResults) {
        if (r.received) {
            received++;
            rttSum += r.rtt_ms;

            if (havePreviousSuccessful) {
                jitterSum += r.jitter_ms;
                jitterCount++;
            }

            havePreviousSuccessful = true;
        }
    }

    double rttAvg = received > 0 ? rttSum / received : 0.0;
    double jitterAvg = jitterCount > 0 ? jitterSum / jitterCount : 0.0;
    double loss = sent > 0 ? ((sent - received) * 100.0 / sent) : 0.0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "RTT avg : " << rttAvg << " ms\n";
    std::cout << "Jitter  : " << jitterAvg << " ms\n";
    std::cout << "Loss    : " << loss << " %\n";

    std::string filename = "net_diag_" + g_nick + ".json";
    std::ofstream out(filename);

    out << std::fixed << std::setprecision(2);
    out << "{\n";
    out << "  \"nickname\": \"" << g_nick << "\",\n";
    out << "  \"sent\": " << sent << ",\n";
    out << "  \"received\": " << received << ",\n";
    out << "  \"loss_percent\": " << loss << ",\n";
    out << "  \"rtt_avg_ms\": " << rttAvg << ",\n";
    out << "  \"jitter_avg_ms\": " << jitterAvg << "\n";
    out << "}\n";

    std::cout << "Saved to " << filename << "\n";

    pthread_mutex_unlock(&g_ping_mutex);
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

    g_nick = nick;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());

    if (!ctx) {
        std::cerr << "Failed to create SSL_CTX\n";
        ERR_print_errors_fp(stderr);
        return 1;
    }

    std::cout << "[Security][TLS] SSL context created\n";

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    int s = ::socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0) {
        perror("socket");
        SSL_CTX_free(ctx);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Bad ip\n";
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    std::cout << "[Transport] connected to "
              << ip << ":" << port << "\n";

    SSL* ssl = SSL_new(ctx);

    if (!ssl) {
        std::cerr << "Failed to create SSL object\n";
        ERR_print_errors_fp(stderr);
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    SSL_set_fd(ssl, s);

    std::cout << "[Security][TLS] handshake started\n";

    if (SSL_connect(ssl) <= 0) {
        std::cerr << "[Security][TLS] handshake failed\n";
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    std::cout << "[Security][TLS] handshake success\n";
    std::cout << "[Security][ENC] encrypted channel established\n";

    X509* cert = SSL_get_peer_certificate(ssl);

    if (cert) {
        std::cout << "[Security][CERT] server certificate received\n";
        X509_free(cert);
    } else {
        std::cout << "[Security][CERT] no server certificate\n";
    }

    Message hello{};
    buildMessage(hello, MSG_HELLO, 0, "hello");

    if (!sendMessage(ssl, hello)) {
        std::cerr << "Failed to send HELLO\n";
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    Message welcome{};

    if (!recvMessage(ssl, welcome) || welcome.type != MSG_WELCOME) {
        std::cerr << "Expected WELCOME\n";
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    std::cout << "[SERVER]: " << payloadToString(welcome) << "\n";

    Message auth{};
    buildMessage(auth, MSG_AUTH, 0, nick);

    if (!sendMessage(ssl, auth)) {
        std::cerr << "Failed to send AUTH\n";
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    Message authReply{};

    if (!recvMessage(ssl, authReply)) {
        std::cerr << "Server closed connection\n";
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    if (authReply.type == MSG_ERROR || authReply.type == MSG_SECURE_ERROR) {
        std::cerr << "[SERVER ERROR]: " << payloadToString(authReply) << "\n";
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(s);
        SSL_CTX_free(ctx);
        return 1;
    }

    if (authReply.type == MSG_SERVER_INFO || authReply.type == MSG_TLS_INFO) {
        std::cout << "[SERVER]: " << payloadToString(authReply) << "\n";
    }

    std::thread rx(rxThreadFunc, ssl);

    std::string line;

    while (g_running.load()) {
        std::cout << "> " << std::flush;

        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line == "/quit") {
            Message bye{};
            buildMessage(bye, MSG_BYE, nextMsgId(), "");
            sendMessage(ssl, bye);
            g_running.store(false);
            break;
        } else if (line.rfind("/ping", 0) == 0) {
            int count = 10;

            if (line.size() > 6) {
                try {
                    count = std::stoi(line.substr(6));
                } catch (...) {
                    std::cout << "Usage: /ping or /ping N\n";
                    continue;
                }
            }

            if (count <= 0) {
                std::cout << "N must be positive\n";
                continue;
            }

            runPingSeries(ssl, count);
        } else if (line == "/netdiag") {
            printNetDiag();
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
            buildMessage(pm, MSG_PRIVATE, nextMsgId(), targetNick + ":" + text);

            if (!sendReliable(ssl, pm)) {
                std::cout << "Private message was not delivered\n";
            }
        } else {
            Message txt{};
            buildMessage(txt, MSG_TEXT, nextMsgId(), line);

            if (!sendReliable(ssl, txt)) {
                std::cout << "Message was not delivered\n";
            }
        }
    }

    g_running.store(false);

    SSL_shutdown(ssl);

    ::shutdown(s, SHUT_RDWR);
    ::close(s);

    if (rx.joinable()) {
        rx.join();
    }

    SSL_free(ssl);
    SSL_CTX_free(ctx);

    pthread_mutex_destroy(&g_pending_mutex);
    pthread_mutex_destroy(&g_ping_mutex);

    std::cout << "Disconnected\n";

    return 0;
}