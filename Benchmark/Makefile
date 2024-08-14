# Define the compiler
CC = gcc

# Targets and their dependencies
all: server client
	@rm -f *.o

# Compile server.c into the server executable
server: server.o
	$(CC) $(CFLAGS) -o server server.o

# Compile client.c into the client executable
client: client.o
	$(CC) $(CFLAGS) -o client client.o

# Compile server.c into an object file
server.o: server.c
	$(CC) $(CFLAGS) -c server.c

# Compile client.c into an object file
client.o: client.c
	$(CC) $(CFLAGS) -c client.c

# Clean up object files and executables
clean:
	rm -f *.o server client
