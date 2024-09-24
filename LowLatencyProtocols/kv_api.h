//
// The communication protocol should be determined ahead of time. So when
// using the kv_api, the communication protocol should be given. The
// protocol can be determined at the time of client server exchange, or
// simply predetermined.
//

#ifndef NETWORKOPTIMIZATION_KV_API_H
#define NETWORKOPTIMIZATION_KV_API_H

# include "ib_functions.h"

#define CONTROL_MESSAGE_BUFFER_SIZE 4096
/**
 * When receive queue are different memory regions, then RX_DEPTH cannot be
 * too deep, else the RAM explodes. RX_depth should be about the same size
 * as client - one receive buffer for one client. Suppose we have 100 clients.
 */
#define TX_DEPTH 100
#define RX_DEPTH 100

/**
 * The performed operation
 */
enum Operation{
    SHUT_DOWN_SERVER = -1,
    EAGER_KV_SET = 0,
    CLIENT_KV_GET = 1,
    SERVER_KV_GET_EAGER = 2,
    SERVER_KV_GET_RENDEZVOUS = 3,
};

/**
 * Basically all the fields we later used
 */
typedef struct KVHandle{
    struct ibv_device      **dev_list;
    struct ibv_device       *ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest     my_dest;
    struct pingpong_dest    *rem_dest;
    char                    *ib_devname;
    int                      port;
    int                      ib_port;
    enum ibv_mtu             mtu;// mtu
    int                      rx_depth;           // The length of receive queue
    int                      tx_depth;           // The length of send queue
    int                      use_event;          // poll CQ or not
    // buffer size for control message
    int                      mr_control_size;
    // buffer size for RDMA Write (This will only be initialized when needed
    int                      mr_rdma_write_size;
    int                      sl;                   // service level
    // the gid index: if set to -1 then we will set gid to 0, else, we will
    // actually query gid for the local device
    int                      gidx;
    // empty buffer for inet_ntop to store the my_dest.gid, later for printing.
    char                     gid[33];
}KVHandle;

typedef struct ControlMessage{
    enum Operation operation;
    size_t value_size;
    char buf[CONTROL_MESSAGE_BUFFER_SIZE];
}ControlMessage;

int kv_open(char *servername, void **kv_handle);

int kv_set(void *kv_handle, const char *key, const char *value);

int kv_get(void *kv_handle, const char *key, char **var);

int kv_close(void *kv_handle);

int run_server(KVHandle *kv_handle);


#endif //NETWORKOPTIMIZATION_KV_API_H
