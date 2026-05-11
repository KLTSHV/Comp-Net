CXX = g++
OPENSSL_PREFIX = /opt/homebrew/opt/openssl@3

CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -I$(OPENSSL_PREFIX)/include
LDFLAGS = -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto

all: server client

server: server.cpp message_ex.hpp netio.hpp
	$(CXX) $(CXXFLAGS) server.cpp -o server $(LDFLAGS)

client: client.cpp message_ex.hpp netio.hpp
	$(CXX) $(CXXFLAGS) client.cpp -o client $(LDFLAGS)

clean:
	rm -f server client history.json net_diag_*.json