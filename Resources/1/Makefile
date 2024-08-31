CC=g++
SRC=server.cpp client.cpp utils.h
BJS=$(SRC:.cpp=.o)
TARGETS=server client

all:$(TARGETS)

server: server.cpp utils.h
	$(CC) $^ -o server

client: client.cpp utils.h
	$(CC) $^ -o client
clean:
	$(RM) $(TARGETS) *~ *core

