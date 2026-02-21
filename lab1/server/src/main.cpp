#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    int port = 9000;
    if (argc >= 2) port = std::stoi(argv[1]);

    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "socket() error: " << std::strerror(errno) << "\n";
        return 1;
    }
    sockaddr_in serverAddr{ };
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(port));
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); 

    if (::bind(sockfd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "bind() error: " << std::strerror(errno) << "\n";
        ::close(sockfd);
        return 1;
    }

    std::cout << "UDP server listening on port " << port << "...\n";

    char buf[1024];

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);

        ssize_t n = ::recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr*) &clientAddr, &clientLen);
        if (n < 0) {
            std::cerr << "recvfrom() error: " << std::strerror(errno) << "\n";
            continue;
        }

        buf[n] = '\0'; 

        char ipStr[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        int clientPort = ntohs(clientAddr.sin_port);

        std::cout << "From " << ipStr << ":" << clientPort << " -> " << buf << "\n";

        ssize_t sent = ::sendto(sockfd, buf, n, 0,
                                (struct sockaddr*) &clientAddr , clientLen);
        if (sent < 0) {
            std::cerr << "sendto() error: " << std::strerror(errno) << "\n";
        }
    }

    ::close(sockfd);
    return 0;
}