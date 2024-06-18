//
// Created by Sam Yiin on 03/06/2024.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>


// must be the same as the server
#define PORT 8080
#define END_MESSAGE "z" // this is a string, char*, because it's easier this way to send message.
#define BUFFER_SIZE 2*1048576

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
        // Another note: TCP will ensure that if client sents a message, the server will for sure receive the message.
        // Also, the TCP ensure that if the client send n messages, it will be delivered in order.
        // Finally, TCP ensure that the content of message will be the correct (same as what the client send).
        // Single socket multiple messages.
        send(sock, message, message_size, 0);
    }
    // send the final message so that the server knows:
    // Notice, message_size = 1 means I am only sending 'z', I am not sending the null terminator of the string.
    send(sock, END_MESSAGE, 1, 0);
    // if the print has no \n, then it will not be print duringing running time.

    // recieve result from server
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = recv(sock, buffer, BUFFER_SIZE, 0);
    if (bytes_read <= 0){printf("damaged message\n");}

    // end timer
    gettimeofday(&end, NULL);
    // calculate time: in seconds
    double const total_second = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec)/1000000.0;

    // calculate throughput: afraid of overflow
    double const total_sent_Mb = num_messages * (message_size / 1048576.0);
    double const throughput = total_sent_Mb / total_second;

    // print the throughput
    printf("%d\t\t%.2f\t\tMbytes/sec\n", message_size, throughput);

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
    int const message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576};
    int const num_experiments = sizeof(message_sizes) / sizeof(message_sizes[0]);
    int const num_messages = 10000;

    for (int i = 0; i < num_experiments; i++) {
        measure_throughput(client_socket, message_sizes[i], num_messages); // Send 1000 messages for each size
    }
    printf("finish everthing, closing client...\n");

    // close socket: frees up descriptor, for TCP: send the FIN packet.
    close(client_socket);
    return 0;
}


