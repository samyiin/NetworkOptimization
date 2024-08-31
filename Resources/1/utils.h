#ifndef COMMUNICATION_NETWORKS_EX1_UTILS_H
#define COMMUNICATION_NETWORKS_EX1_UTILS_H
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <ctime>
#include <cstring>
#include <fstream>
#include <netdb.h>

#endif //COMMUNICATION_NETWORKS_EX1_UTILS_H

using namespace std;
static const int message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
                                    2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144,
                                    524288, 1048576};
static const int port_num = 8080;
static const char *reply_message = "Message received.";
static const int warmup_messages = 400;
static const int real_messages = 2000;

static const int warmup_messages_ind = 0;
static const int real_messages_ind = 1;
static const int num_messages[] = {warmup_messages,real_messages};
static const int bits_in_byte = 8;
static const int bytes_in_KB = 1024;


static const int server_reply_len = strlen(reply_message);
static const string header ="messageSize[Bytes]\tThroughput\tThroughputUnits";
