#ifndef COMMUNICATION_NETWORKS_EX1_UTILS_H
#define COMMUNICATION_NETWORKS_EX1_UTILS_H
//#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
//#include <chrono>
//#include <ctime>
//#include <cstring>
//#include <fstream>
#include <netdb.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/time.h>
#include <stdlib.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#include <infiniband/verbs.h>

#endif //COMMUNICATION_NETWORKS_EX1_UTILS_H

static const int message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
                                    2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144,
                                    524288, 1048576};

static const int message_sizes_length = 21;
static const int warmup_messages = 100;
static const int real_messages = 5000;
static const int buffer_size = 1048576;
static const int PP_COMPLETION_ERROR = -1;


//static const int bits_in_byte = 8;
//static const int bytes_in_KB = 1024;
//static const int bytes_in_MB = 1048576;
static const char *filename = "output.txt";
static const char *header ="messageSize[Bytes]\tThroughput\tThroughputUnits";
