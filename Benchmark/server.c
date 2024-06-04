//
// Created by hsiny on 6/3/24.
//

#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 1048576 // 1MB

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    char buffer[BUFFER_SIZE] = {0};

    // AF_INET means it's ipv4, SOCK_STREAM means it's TCP, protocal=0 means automatically select protocal.
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // set socket options: server_fd: the file descriptor of the server socket
    // protocal level: SOL_SOCKET: all the options is applied to this socket, not a specific protocol in this socket
    // option name: SO_REUSEADDR: allows multiple sockets to bind to the same address and port
    //     There is a slight difference between SO_REUSEADDR and SO_REUSEPORT, see documentation. Using together?
    // opt value: means value of "SO_REUSEADDR | SO_REUSEPORT" is 1. This could be pointer to a complex struct as value
    // since we need to pass optval as pointer, we need the size of it so that C can know where does it's content end.
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // specify is it ip4 or ip6 (sin - socket internet, s - struct)
    address.sin_family = AF_INET;
    // specify the ip address INADDR_ANY: to bind to all available interface
    // can specify the ip address here: change INADDR_ANY to inet_addr("127.0.0.1");
    // (to bind to the loopback address： only listen to clients on this machine)
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    // set port number
    address.sin_port = htons(PORT);

    // bind the server to the above address, above port (since it's pointer, we need size)
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // mark a socket as a passive socket,
    // that is, as a socket that will be used to accept incoming connection requests using the accept function
    // backlog = 3: maximum number of pending connections that can be queued up before the kernel starts to refuse new
    // connection attempts
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // accept(server_fd,...) is server.accept in oop
    // besides from server_fd, it takes an empty socket address struct (
    struct sockaddr_in client_address;
    int client_addrlen = sizeof(client_address);
    if ((new_socket = accept(server_fd, (struct sockaddr *)&client_address, (socklen_t*)&client_addrlen)) < 0) {
        perror("accept");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    int bytes_read;
    while ((bytes_read = read(new_socket, buffer, BUFFER_SIZE)) > 0) {
        // Simulate processing
    }

    close(new_socket);
    close(server_fd);
    return 0;
}

