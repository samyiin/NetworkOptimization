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




/**
 * The WC_BATCH: how many work complete do we poll from cq in one ibv_poll_cq
 *
 */
#define WC_BATCH (10)

/**
 * The REFILL_RWR_THRES: when receive queue have less receive work request,
 * we will refill the receive queue.
 */
# define REFILL_RWR_THRES 10

/**
 * This is for throughput test
 */
#define NUMBER_MESSAGES 5000

/**
 * mtu depends on how often do we send large message, the mtu will be set
 * differently.
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
 * Work request id, in our exercise not really important.....
 */
enum {
    PINGPONG_SEND_WRID = 1,
    PINGPONG_WRITE_WRID = 2,
    PINGPONG_RECEIVE_WRID = 3,
};

/**
 * This enum is the status of receive MRs. If we used them to post receive,
 * then it will IN_RECEIVE_QUEUE, after the receive wr is completed, this mr
 * will be FREE again
 */
enum MRStatus{
    // relevant to re-post receive queue
    FREE = 0,
    IN_RECEIVE_QUEUE = 1,
    // not relevant to receive queue
    RDMA = 2,
};

/**
 * This struct is used for receive memory regions.
 */
struct MRInfo{
    struct ibv_mr       *mr;
    void                *mr_start_ptr;
    size_t              mr_size;
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
 * mr_control_send                   memory region for control message
 * mr_control_send_start_ptr         ptr to the buffer of mr_control_send
 * mr_rdma_write                memory region for rdma writemr_receive
 * mr_rdma_write_start_ptr      ptr to the buffer of mr_rdma_write
 * mr_control_send_size              size of control message
 * mr_rdma_write_size           size of rdma_write message
 * ==========================================================================
 * remote_buf_va                the virtual address of remote buffer
 * remote_buf_rkey              the virtual address of remote key
 */
struct pingpong_context {
    struct ibv_context		    *context;
    struct ibv_comp_channel	    *channel;
    struct ibv_pd		        *pd;
    struct ibv_cq		        *send_cq;
    struct ibv_cq		        *receive_cq;

    struct ibv_port_attr	    portinfo;

    struct ibv_qp		        *qp;

    // control messages (send and receive)
    struct ibv_mr		        *mr_control_send;
    void                        *mr_control_send_start_ptr;
    int                         mr_control_send_size;

    // control messages receive
    struct MRInfo               *array_mr_receive_info;
    struct ibv_mr		        *mr_control_receive;
    void                        *mr_control_receive_start_ptr;
    int                         mr_control_receive_size;
    int				            rx_depth;
    int				            routs;

    // temporary RDMA context (read and write) (Used to post send)
    struct ibv_mr		        *mr_rdma;
    void                        *mr_rdma_start_ptr;
    size_t                      mr_rdma_size;
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

int pp_post_send(struct pingpong_context *ctx);

int pp_post_rdma(struct pingpong_context *ctx, enum ibv_wr_opcode opcode);

int poll_n_send_wc(struct pingpong_context *ctx, int n_complete);

int poll_next_receive_wc(struct pingpong_context *ctx, int blocking);

#endif //NETWORKOPTIMIZATION_IB_FUNCTIONS_H
