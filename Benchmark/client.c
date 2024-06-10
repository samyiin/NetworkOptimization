//
// Created by Sam Yiin on 03/06/2024.
//

#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

// must be the same as the server
#define PORT 8080

void measure_throughput(int sock, int message_size, int num_messages) {
    // use malloc because we don't know the size of message.
    char *message = (char *)malloc(message_size);
    // this function will set all the bytes in the allocated memory to be 'A', a char which is one byte.
    memset(message, 'A', message_size);


    // start timer
    struct timeval start, end;
    gettimeofday(&start, NULL);

    // send multiple messages
    for (int i = 0; i < num_messages; i++) {
        // send function takes a message as a string (message size because message is a ptr), a socket to send to
        // The last parameter is flag: 0 -- No Flag.
        // As documented in connect, here the os knows where to send the message to.
        send(sock, message, message_size, 0);
    }

    printf("sent message\n");

    // recieve result from server
    // todo: the logic here is flawed, you must keep sending until we read respond from the server.
    int const BUFFER_SIZE = 1048576 * 2;
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = read(sock, buffer, BUFFER_SIZE);
    printf("%s", buffer);
    printf("received message\n");

    // end timer
    gettimeofday(&end, NULL);
    // calculate time
    double total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    // calculate throughput: afraid of overflow
    double throughput = ((num_messages * message_size) /1000000) / total_time;
    // print the throughput
    printf("%d\t%.2f\tMbytes/sec\n", message_size, throughput);
    // calculate through put

    // free allocated memory
    free(message);
}

int main(int argc, char const *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <server-ip>\n", argv[0]);
        return -1;
    }

    // Create client socket: ipv4, TCP
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) {
        // todo: close sockets if exit.
        printf("\n Socket creation error \n");
        return -1;
    }

    // not setting ip, only set ipv4, port
    // server_ip will be a string of the ip address of the server, the port will be 8080 by default.
    const char *server_ip = argv[1];
    // build the information of the socket we are connecting to
    struct sockaddr_in serv_addr;

    // set ipv4, ip, port
    serv_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }
    serv_addr.sin_port = htons(PORT);


    // Connect to server:
    // After successfully calling connect, the operating system maintains the connection details for client_socket
    // So when I later call "send(clent_sever, message, ...), the os would automatically know that I am sending to the
    // server.
    if (connect(client_socket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }

    // measure throughput of all of these
    // int message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576};
    int message_sizes[] = {1};
    int num_sizes = sizeof(message_sizes) / sizeof(message_sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        measure_throughput(client_socket, message_sizes[i], 100); // Send 1000 messages for each size
    }
    printf("finish everthing, closing client...\n");


    // close socket: frees up descriptor, for TCP: send the FIN packet.
    close(client_socket);
    return 0;
}

