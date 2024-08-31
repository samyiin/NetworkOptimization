#include <assert.h>
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
#include <stdbool.h>
#include <infiniband/verbs.h>

#define RED     "\033[31m"      /// Paints text in red

static const int EAGER_MAX_MESSAGE_SIZE = 4096;
static const int RENDEZVOUS_TEST_SIZE = 10 * EAGER_MAX_MESSAGE_SIZE;
static const int CLIENT_NUM_ARGC = 2;
static const int PP_COMPLETION_ERROR = -1;
static const int MALLOC_ERROR = -1;
static const int SUCCESS = 0;
static const int port_num = 12344;

enum packet_type {
    GET_INITIAL_MESSAGE, /// The client sends a get request with the key wanted

    EAGER_GET_ANSWER, /// The server responds to the get request with the value
    RENDEZVOUS_GET_ANSWER, /// The server responds to the get request with the value, using Rendezvous

    EAGER_SET_INITIAL_MESSAGE,
    RENDEZVOUS_SET_INITIAL_MESSAGE,
    RENDEZVOUS_SET_ANSWER,

    FIN_SET,
    FIN_GET

};

struct packet {
    enum packet_type type;
    union {
        struct {
            char key_val[0];
        } eager_set_initial;

        struct {
            unsigned int value_len;
            char value[0];
        } eager_get_answer;

        struct {
            char key[0];
        } get_initial_message;

        struct {
            void* remote_addr;
            uint32_t remote_key;
            unsigned int value_len;
        } rndv_get_answer;

        struct {
            void* remote_addr;
            uint32_t remote_key;
            unsigned int value_len;
            char key[0];
        } rndv_set_initial;

        struct {
            void* remote_addr;
            uint32_t remote_key;
        } rndv_set_answer;

        struct {
            void* val_address;
            char key[0];
        } rndv_fin;
    }packet_union;
};

static const unsigned int packet_sizeof = sizeof(struct packet);

struct kv_pairs{
    char* key;
    char* value;
    int num_client_gets;
    char* futureValue;
    struct kv_pairs* next;
};

static const unsigned int kv_pair_size = sizeof(struct kv_pairs);



