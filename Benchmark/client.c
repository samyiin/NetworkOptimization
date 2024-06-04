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

#define PORT 8080

void measure_throughput(int sock, int message_size, int num_messages) {
    char *message = (char *)malloc(message_size);
    memset(message, 'A', message_size);

    struct timeval start, end;
    double total_time;

    // Send messages
    gettimeofday(&start, NULL);
    for (int i = 0; i < num_messages; i++) {
        send(sock, message, message_size, 0);
    }
    gettimeofday(&end, NULL);

    total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    double throughput = ((num_messages * message_size) /1000000) / total_time;

    // afraid of overflow
    printf("%d\t%.2f\tMbytes/sec\n", message_size, throughput);

    free(message);
}

int main(int argc, char const *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <server-ip>\n", argv[0]);
        return -1;
    }

    const char *server_ip = argv[1];
    int sock;
    struct sockaddr_in serv_addr;

    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4/IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }

    // Connect to server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }

    int message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576};
    int num_sizes = sizeof(message_sizes) / sizeof(message_sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        measure_throughput(sock, message_sizes[i], 10000); // Send 1000 messages for each size
    }

    close(sock);
    return 0;
}

