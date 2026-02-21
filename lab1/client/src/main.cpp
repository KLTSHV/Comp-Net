#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string serverIp = "127.0.0.1";
    int serverPort = 9000;

    if (argc >= 2) serverIp = argv[1];
    if (argc >= 3) serverPort = std::stoi(argv[2]);

    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "socket() error: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(serverPort));
    if (::inet_pton(AF_INET, serverIp.c_str(), &serverAddr.sin_addr) != 1) {
        std::cerr << "inet_pton() error: invalid server IP\n";
        ::close(sockfd);
        return 1;
    }

    std::cout << "Enter text (empty line to exit):\n";

    std::string line;
    while (true) {
        std::getline(std::cin, line);
        if (!std::cin || line.empty()) break;

        ssize_t sent = ::sendto(sockfd, line.data(), line.size(), 0,
                                (struct sockaddr*) &serverAddr, sizeof(serverAddr));
        if (sent < 0) {
            std::cerr << "sendto() error: " << std::strerror(errno) << "\n";
            continue;
        }

        char buf[1024];
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);

        ssize_t n = ::recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr*) &fromAddr, &fromLen);
        if (n < 0) {
            std::cerr << "recvfrom() error: " << std::strerror(errno) << "\n";
            continue;
        }

        buf[n] = '\0';
        std::cout << "Server replied: " << buf << "\n";
    }
    ::close(sockfd);
    return 0;
}