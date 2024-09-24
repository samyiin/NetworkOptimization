//
// Created by hsiny on 9/15/24.
//

#ifndef NETWORKOPTIMIZATION_IB_FUNCTIONS_H
#define NETWORKOPTIMIZATION_IB_FUNCTIONS_H

#define _GNU_SOURCE             // this is for function "asprintf"
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
#include <infiniband/verbs.h>

#include <byteswap.h>




/*
 * The WC_BATCH should also be related to tx_depth and rx_depth.
 * 1. it doesn't need to exceed the length of CQ = rx_depth + tx_depth
 * 2. As long as we poll more than the amount of sent messages, it doesn't
 * make a difference.
 * 3. If we poll slower than the client send speed, then for the same amount
 * of message, we will poll more times.
 * But somehow when this number is high, the speed will drop for large
 * messages.
 *
 */
#define WC_BATCH (10)
/*
 * The REFILL_RWR_THRES is related to how fast the server can process
 * received messages. Depend on computer.
 * If it processed slow, then it should refill more often.
 * Else the server will run out of rec_wr while it is still processing.
 * But if we refill too often, it also creates overhead.
 * As long as we refill before the client send more messages, it won't make
 * a difference if we refill faster.
 */
# define REFILL_RWR_THRES 10

/*
 * Number of iterations is the bigger the better. The problem is just wast
 * time to perform the test, so we should find a sweet spot
 */
#define NUMBER_MESSAGES 5000

/*
 * mtu will affect how many packets we will send for each message. depends
 * on how often do we send large message, the mtu will be set differently.
 * Choices: 256 - 4096 (power of 2)
 */
#define MTU 4096

/**
 * This variable will store the max incline data for the qp
 * It is initialized (define and assign) in ib_function.c
 */
extern uint32_t max_inline;

/**
 * This variable is depend on each machine.
 * It is defined in ib_function.c but it will be first assigned value in main.c
 *
 */
extern int page_size;

/**
 * pingpong receive work request id
 *
 * What's Different From Template?
 * Added PINGPONG_WRITE_WRID
 */
enum {
    PINGPONG_SEND_WRID = 0,
    PINGPONG_WRITE_WRID = 1,
};

enum MRStatus{
    FREE = 0,
    IN_RECEIVE_QUEUE = 1,
};

/**
 * This struct is used for receive memory regions.
 */
struct MRInfo{
    struct ibv_mr       *mr;
    void                *mr_start_ptr;
    enum MRStatus       mr_status;
};

/**
 * context
 * channel
 * pd               protection domain
 * (Deleted) mr     memory region
 * cq               completion queue
 * qp               queue pair
 * buf              the pointer to the buffer that holds the data being send
 * (Deleted) size   size of control message we are sending/receiving
 * rx_depth         max number of receive wr in receive queue
 * routs            how many rounds of iteration in the pingpong test
 * portinfo         information of the port (port state, MTU, other config)
 * ==========================================================================
 * mr_control                   memory region for control message
 * mr_control_start_ptr         ptr to the buffer of mr_control
 * mr_rdma_write                memory region for rdma writemr_receive
 * mr_rdma_write_start_ptr      ptr to the buffer of mr_rdma_write
 * mr_control_size              size of control message
 * mr_rdma_write_size           size of rdma_write message
 * ==========================================================================
 * remote_buf_va                the virtual address of remote buffer
 * remote_buf_rkey              the virtual address of remote key
 */
struct pingpong_context {
    struct ibv_context		    *context;
    struct ibv_comp_channel	    *channel;
    struct ibv_pd		        *pd;
    struct ibv_mr               *mr_rdma_write;
    struct ibv_cq		        *cq;
    struct ibv_qp		        *qp;
    void			            *buf;
    struct ibv_mr		        *mr_control_send;
    void                        *mr_send_start_ptr;
    int                         mr_control_size;
    struct MRInfo               *array_mr_receive_info;

    void                        *mr_rdma_write_start_ptr;
    int                         mr_rdma_write_size;
    int				            rx_depth;
    int				            routs;
    struct ibv_port_attr	    portinfo;
    uint64_t                    remote_buf_va;
    uint32_t                    remote_buf_rkey;
};

/**
 * lid          local id
 * qpn          queue pair number
 * psn          packet sequence number
 * gid          global id
 *
 */
struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};



enum ibv_mtu pp_mtu_to_enum(int mtu);

uint16_t pp_get_local_lid(struct ibv_context *context, int port);

int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

int pp_connect_ctx(struct pingpong_context *ctx, int port, int
        my_psn, enum ibv_mtu mtu, int sl, struct pingpong_dest *dest,
                int sgid_idx);

struct pingpong_dest *pp_client_exch_dest(const char *servername, int
        port, const struct pingpong_dest *my_dest);


struct pingpong_dest *pp_server_exch_dest(struct pingpong_context
        *ctx, int ib_port, enum ibv_mtu mtu, int port, int sl, const struct
                pingpong_dest *my_dest, int sgid_idx);

struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int
        mr_control_size, int rx_depth, int tx_depth, int port,
                                     int use_event, int is_server);

int pp_close_ctx(struct pingpong_context *ctx);

int pp_post_recv(struct pingpong_context *ctx);

int pp_post_rdma_write(struct pingpong_context *ctx);

int pp_post_send(struct pingpong_context *ctx);

int pp_wait_completions(struct pingpong_context *ctx, int n_complete);


#endif //NETWORKOPTIMIZATION_IB_FUNCTIONS_H
