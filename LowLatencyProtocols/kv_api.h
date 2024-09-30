//
// The communication protocol should be determined ahead of time. So when
// using the kv_api, the communication protocol should be given. The
// protocol can be determined at the time of client server exchange, or
// simply predetermined.
//

#ifndef NETWORKOPTIMIZATION_KV_API_H
#define NETWORKOPTIMIZATION_KV_API_H

# include "ib_functions.h"
#include "dynamic_array.h"

#define CONTROL_MESSAGE_BUFFER_SIZE 4096
/**
 * When receive queue are different memory regions, then RX_DEPTH cannot be
 * too deep, else the RAM explodes. RX_depth should be about the same size
 * as client - one receive buffer for one client. Suppose we have 100 clients.
 */
#define TX_DEPTH 100
#define RX_DEPTH 100


/// The number of clients
#define NUM_CLIENTS 2


/**
 * The performed operation
 */
enum Operation{
    SHUT_DOWN_SERVER = -1,

    CLIENT_KV_SET_EAGER = 0,
    CLIENT_KV_SET_RENDEZVOUS = 1,
    SERVER_KV_SET_RENDEZVOUS = 2,

    CLIENT_KV_GET = 3,
    SERVER_KV_GET_EAGER = 4,
    SERVER_KV_GET_RENDEZVOUS = 5,
    SERVER_KV_GET_KEY_NOT_FOUND = 6,
    SERVER_IN_PROGRESS = 7,

    CLIENT_KV_GET_RENDEZVOUS_FIN = 8,
    CLIENT_KV_SET_RENDEZVOUS_FIN = 9,
    SERVER_KV_SET_SUCCESSFUL = 10,
    SERVER_KV_GET_SUCCESSFUL = 11,
};

/**
 * Basically all the fields we later used
 */
typedef struct KVHandle{
    struct ibv_device      **dev_list;
    struct ibv_device       *ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest    my_dest;
    struct pingpong_dest    *rem_dest;
    char                    *ib_devname;
    int                     port;
    int                     ib_port;
    enum ibv_mtu            mtu;// mtu
    int                     rx_depth;           // The length of receive queue
    int                     tx_depth;           // The length of send queue
    int                     use_event;          // poll CQ or not
    // buffer size for control message
    int                     mr_control_size;
    // buffer size for RDMA Write (This will only be initialized when needed
    int                      mr_rdma_write_size;
    int                     sl;                   // service level
    // the gid index: if set to -1 then we will set gid to 0, else, we will
    // actually query gid for the local device
    int                     gidx;
    // empty buffer for inet_ntop to store the my_dest.gid, later for printing.
    char                    gid[33];
    int                     num_remote_host;
    struct pingpong_context **array_ctx_ptr;
    struct pingpong_dest    **array_remote_dest;
}KVHandle;

typedef struct ControlMessage{
    enum Operation operation;
    char buf[CONTROL_MESSAGE_BUFFER_SIZE];
}ControlMessage;

enum TaskType{
    SET_VALUE = 0,
    GET_VALUE = 1,
};

typedef struct task{
    enum TaskType task_type;
    char *key;
    char *value;
}Task;

int kv_open(char *servername, void **kv_handle);

int kv_set(void *kv_handle, const char *key, const char *value);

int kv_get(void *kv_handle, const char *key, char **var);

int kv_close(void *kv_handle);

void kv_release(char *value);

int run_server(KVHandle *kv_handle,  int print_database);

///============================================================================
///                              Protocol
///============================================================================

/**
 * Set my_control_message to send
 * This will mem copy from the input pointers to the send control message mr
 * @return
 */
static int set_control_message(KVHandle *ptr_kv_handle, enum Operation
operation, const void **array_messages_address){
    ControlMessage *my_control_message = (ControlMessage *)
            ptr_kv_handle->ctx->mr_control_send_start_ptr;
    my_control_message->operation = operation;
    void *current_buf_ptr = my_control_message->buf;

    int array_size;
    size_t messages_sizes[3]; // all message contains less than 3

    if (operation == CLIENT_KV_SET_EAGER){
        /// CLIENT_KV_SET_EAGER: key + value_size + value
        array_size = 3;
        size_t key_size = strlen(array_messages_address[0]) + 1;
        size_t value_size = strlen(array_messages_address[2]) + 1;
        messages_sizes[0] = key_size;
        messages_sizes[1] = sizeof(size_t);
        messages_sizes[2] =value_size;
    }else if (operation == CLIENT_KV_SET_RENDEZVOUS){
        /// CLIENT_KV_SET_RENDEZVOUS: key + value_size
        array_size = 2;
        size_t key_size = strlen(array_messages_address[0]) + 1;
        messages_sizes[0] = key_size;
        messages_sizes[1] = sizeof(size_t);
    }else if (operation == SERVER_KV_SET_RENDEZVOUS){
        /// SERVER_KV_SET_RENDEZVOUS: va + rkey
        array_size = 2;
        messages_sizes[0] = sizeof(uint64_t);
        messages_sizes[1] = sizeof(uint32_t);
    }else if (operation == CLIENT_KV_GET){
        /// CLIENT_KV_GET: key
        array_size = 1;
        size_t key_size = strlen(array_messages_address[0]) + 1;
        messages_sizes[0] = key_size;

    }else if (operation == SERVER_KV_GET_EAGER){
        /// SERVER_KV_GET_EAGER: value
        array_size = 1;
        size_t value_size = strlen(array_messages_address[0]) + 1;
        messages_sizes[0] = value_size;

    }else if (operation == SERVER_KV_GET_RENDEZVOUS){
        /// SERVER_KV_GET_RENDEZVOUS: value size + va + rkey
        array_size = 3;
        messages_sizes[0] = sizeof(size_t);
        messages_sizes[1] = sizeof(uint64_t);
        messages_sizes[2] =sizeof(uint32_t);

    }else if (operation == CLIENT_KV_GET_RENDEZVOUS_FIN){
        /// CLIENT_KV_GET_RENDEZVOUS_FIN: key
        array_size = 1;
        size_t key_size = strlen(array_messages_address[0]) + 1;
        messages_sizes[0] = key_size;

    }else if (operation == CLIENT_KV_SET_RENDEZVOUS_FIN){
        /// CLIENT_KV_GET_RENDEZVOUS_FIN: key
        array_size = 1;
        size_t key_size = strlen(array_messages_address[0]) + 1;
        messages_sizes[0] = key_size;

    }
    else{
        /// The else will just be all the operation without information
        array_size = 0;
    }

    for (int i = 0; i < array_size; i++){
        memcpy(current_buf_ptr, array_messages_address[i],
               messages_sizes[i]);
        current_buf_ptr += messages_sizes[i];
    }
    return 0;
}

/**
 * Decode the message in control message buffer
 * If it's a string, provide message size == 0 (unknown)
 */
static int decode_control_message_buffer(enum Operation operation,
                                         void *control_message_buf,
                                         void **array_ptr_to_fill){
    int array_size;
    size_t messages_sizes[3]; // all message contains less than 3

    if (operation == CLIENT_KV_SET_EAGER){
        /// CLIENT_KV_SET_EAGER: key + value_size + value
        array_size = 3;
        messages_sizes[0] = 0;
        messages_sizes[1] = sizeof(size_t);
        messages_sizes[2] =0;
    }else if (operation == CLIENT_KV_SET_RENDEZVOUS){
        /// CLIENT_KV_SET_RENDEZVOUS: key + value_size
        array_size = 2;
        messages_sizes[0] = 0;
        messages_sizes[1] = sizeof(size_t);
    }else if (operation == SERVER_KV_SET_RENDEZVOUS){
        /// SERVER_KV_SET_RENDEZVOUS: va + rkey
        array_size = 2;
        messages_sizes[0] = sizeof(uint64_t);
        messages_sizes[1] = sizeof(uint32_t);
    }else if (operation == CLIENT_KV_GET){
        /// CLIENT_KV_GET: key
        array_size = 1;
        messages_sizes[0] = 0;

    }else if (operation == SERVER_KV_GET_EAGER){
        /// SERVER_KV_GET_EAGER: value
        array_size = 1;
        messages_sizes[0] = 0;

    }else if (operation == SERVER_KV_GET_RENDEZVOUS){
        /// SERVER_KV_GET_RENDEZVOUS: value size + va + rkey
        array_size = 3;
        messages_sizes[0] = sizeof(size_t);
        messages_sizes[1] = sizeof(uint64_t);
        messages_sizes[2] =sizeof(uint32_t);

    }else if (operation == CLIENT_KV_GET_RENDEZVOUS_FIN){
        /// CLIENT_KV_GET_RENDEZVOUS_FIN: key
        array_size = 1;
        messages_sizes[0] = 0;

    }else if (operation == CLIENT_KV_SET_RENDEZVOUS_FIN){
        /// CLIENT_KV_GET_RENDEZVOUS_FIN: key
        array_size = 1;
        messages_sizes[0] = 0;

    }else{
        /// The else will just be all the operation without information
        array_size = 0;
    }

    if (array_size == 1){
        array_ptr_to_fill[0] = control_message_buf;
        return 0;
    }
    void *current_buf_ptr = control_message_buf;
    for (int i = 0; i < array_size; i++){
        array_ptr_to_fill[i] = current_buf_ptr;
        if (messages_sizes[i] == 0){
            // It's a string, so we don't know it's size
            size_t string_length = strlen(current_buf_ptr) + 1;
            current_buf_ptr += string_length;
        }else{
            current_buf_ptr += messages_sizes[i];
        }
    }
    return 0;
}

#endif //NETWORKOPTIMIZATION_KV_API_H
