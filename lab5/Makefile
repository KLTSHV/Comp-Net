CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

all: server client

server: server.cpp message_ex.hpp netio.hpp
	$(CXX) $(CXXFLAGS) server.cpp -o server

client: client.cpp message_ex.hpp netio.hpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f server client history.json
