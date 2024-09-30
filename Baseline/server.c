//
// Created by hsiny on 6/3/24.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

// "127.0.0.1": loopback address: only listen to clients on this machine
// INADDR_ANY: 0.0.0.0: to bind to all available interface
// "132.65.164.103": mlx-stud-03 server's public ip.
#define SERVER_IP INADDR_ANY
#define PORT 8080
#define END_MESSAGE 'z' // this is a char, not a string as in Client.c, this way it's easier for comparason
#define RECEIVE_MESSAGE "R"
#define BUFFER_SIZE 2*1048576

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;

    // AF_INET means it's ipv4
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // specify is it ip4 or ip6 (sin - socket internet, s - struct)
    address.sin_family = AF_INET;
    // setting server ip
    address.sin_addr.s_addr = htonl(SERVER_IP);
    // set port number
    address.sin_port = htons(PORT);

    // bind the server to the above address, above port (since it's pointer, we need size)
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // mark a socket as a passive socket,
    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // accept a new connection from client: see notes above.
    struct sockaddr_in client_addr;
    int client_addrlen = sizeof(client_addr);
    if ((new_socket = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t*)&client_addrlen)) < 0) {
        perror("accept");
        close(server_fd);
        exit(EXIT_FAILURE);
    }


    // each iter of the while is an experiment. The experiment starts with client sendig messages 'A', and ends when the
    // client send 'z'.
    while(1) {
        int bytes_read;
        char buffer[BUFFER_SIZE] = {0};

        while (1) {
            bytes_read = recv(new_socket, buffer, BUFFER_SIZE, 0);
            if (bytes_read <= 0) {
                // Probably Client socket is closed
                // close both sockets
                close(new_socket);
                close(server_fd);
                return 0;
            }
            // check if the last byte of the current buffer is the end: to determine the end of messages
            if (buffer[bytes_read -1] == END_MESSAGE){break;}
        }

        // send a message to the client to indicate that process is completed
        send(new_socket, RECEIVE_MESSAGE, 2,0);
    }

}



