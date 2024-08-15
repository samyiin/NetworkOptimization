//
// Created by Sam Yiin on 03/06/2024.
//
#define _GNU_SOURCE             // this is for function "asprintf"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <netdb.h>


// must be the same as the server
# define PORT 8080
# define END_MESSAGE "z" // this is a string, char*, because it's easier this way to send message.
# define BUFFER_SIZE 2 * 1048576
# define NUM_OF_MESSAGES 10000

void measure_throughput(int const sock, int const num_experiments, int const warmup) {
	int message_size = 1;
	for (int n = 0; n < num_experiments; n++) {
	    // use malloc because we don't know the size of message.
		char *message = (char *)malloc(message_size);
	    // this function will set all the bytes in the allocated memory to be 'A', a char which is one byte.
	    memset(message, 'A', message_size);

	    // start timer
	    struct timeval start, end;
	    gettimeofday(&start, NULL);

	    // send multiple messages
	    for (int i = 0; i < NUM_OF_MESSAGES; i++) {
            // send message
	        send(sock, message, message_size, 0);
	    }
	    // send the final message so that the server knows:
	    // Notice, message_size = 1 means I am only sending 'z', I am not sending the null terminator of the string.
	    send(sock, END_MESSAGE, 1, 0);
	    // if the print has no \n, then it will not be print duringing running time.

	    // recieve result from server
	    char buffer[BUFFER_SIZE] = {0};
		int const bytes_read = recv(sock, buffer, BUFFER_SIZE, 0);
	    if (bytes_read <= 0){printf("damaged message\n");}

	    // end timer
	    gettimeofday(&end, NULL);
	    // calculate time: in seconds
	    double const total_second = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec)/1000000.0;

	    // calculate throughput: afraid of overflow
	    double const total_sent_Mb = NUM_OF_MESSAGES * (message_size * 8 /
	            1048576.0);
	    double const throughput = total_sent_Mb / total_second;

	    // print the throughput: In MegaBytes per second
	    if (!warmup){
	        printf("%d\t\t%.2f\t\tMbits/sec\n", message_size, throughput);
	    }

	    // free allocated memory
	    free(message);

		// double the message size
		message_size *= 2;
	}
}

void measure_latency(int const sock, int const warmup) {
	// We will send an END_MESSAGE, receive a result, repeat NUM_OF_MESSAGES times. measure the RTT
	// Latency will be measured by RTT/(NUM_OF_MESSAGES*2)
	// regarding choice of the size of message? I choose to send the smallest message because it should be the fastest.

	// start timer
	struct timeval start, end;
	gettimeofday(&start, NULL);

	for (int i = 0; i < NUM_OF_MESSAGES; i++) {
		// send single message
		// Notice, message_size = 1 means I am only sending 'z', I am not sending the null terminator of the string.
		send(sock, END_MESSAGE, 1, 0);
		// if the print has no \n, then it will not be print duringing running time.

		// recieve result from server
		char buffer[BUFFER_SIZE] = {0};
		int const bytes_read = recv(sock, buffer, BUFFER_SIZE, 0);
		if (bytes_read <= 0){printf("damaged message\n");}
	}

	// end timer
	gettimeofday(&end, NULL);

	// calculate time: in milliseconds
	double const round_trip_time = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_usec - start.tv_usec)/1000.0;

	// calculate latency
	double const latency = round_trip_time/ (NUM_OF_MESSAGES * 2.0);

	// print the latency
	if (!warmup){
		printf("Latency:\t\t%.4f\tmilliseconds\n", latency);
	}
}

int main(int const argc, char const *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <server-ip>\n", argv[0]);
        return -1;
    }
    // server_ip will be a string of the ip address of the server, the port will be 8080 by default.
    const char *servername = argv[1];

    /*
     * Trying to find the server ip given the server name
     */
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_family   = AF_INET,         // IPV4
            .ai_socktype = SOCK_STREAM      // TCP
    };
    char *service;
    int n;
    int client_socket = -1;
    if (asprintf(&service, "%d", PORT) < 0){
        return -1;
    }


    n = getaddrinfo(servername, service, &hints, &res);
    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, PORT);
        free(service);
        return -1;
    }

    // t is pointer to current potential server address,
    // res is the first potential server address
    for (t = res; t; t = t->ai_next) {
        // create a client socket that match the style of the potential server
        client_socket = socket(t->ai_family, t->ai_socktype,
                                   t->ai_protocol);
        if (client_socket >= 0) {
            if (!connect(client_socket, t->ai_addr, t->ai_addrlen))
                break;
            close(client_socket);
            client_socket = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    // If we didn't manage to connect to any server
    if (client_socket < 0) {
        // todo: close sockets if exit.
        printf("\n Socket creation error \n");
        return -1;
    }


    // warm up cycle (each warmup cycle takes NUM_OF_MESSAGES round trips)
	measure_latency(client_socket, 1);

	// actual trails
	measure_throughput(client_socket, 21, 0);

	// actual trails: if we don't want to print, we can put warmup=0
	measure_latency(client_socket, 0);

    // close socket: frees up descriptor, for TCP: send the FIN packet.
    close(client_socket);
    return 0;
}


