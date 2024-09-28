/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Hsin-Chun yin 2024. All rights reserved.
 *
 * Remove the static attribute of functions
 */

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
 * buffer for registering the memory region, need to be big enough for the
 * largest message. No need to be bigger.
 */
#define MR_RDMA_WRITE_SIZE 1048576

/*
 * No matter how much we set RX to be, there is always a possibility that
 * the client are sending so fast, the server will not have enough rec_wr to
 * receive them, Then those send request is queued.
 * But the higher we set RX_DEPTH, the less likely that will happen. But
 * when RX_DEPTH is too high, when we send large messages, speed will drop.
 * probably because it exceeds the buffer size, so the send request is queued?
 */
#define TX_DEPTH 5000
#define RX_DEPTH 5000

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

/*
 * This variable will store the max incline data for the qp
 */
uint32_t max_inline;


/**
 * pingpong receive work request id
 */
enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
    PINGPONG_WRITE_WRID = 3,
};

static int page_size;

/**
 * Notice the difference between this and original template:
 * 1. add buf_size: separate buffer size to message size
 *
 *
 * context
 * channel
 * pd               protection domain
 * mr               memory region
 * cq               completion queue
 * qp               queue pair
 * buf              the pointer to the buffer that holds the data being send
 * size             size of control message we are sending/receiving
 * rx_depth         depth of receive queue: how many receive work request
 *                  can be posted in advance.
 * routs            how many rounds of iteration in the pingpong test
 * portinfo         information of the port (port state, MTU, other config)
 *
 * Notice: the address where buf is pointing at is the same address for the
 * control messages.
 */
struct pingpong_context {
    struct ibv_context		*context;
    struct ibv_comp_channel	*channel;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr_control_send;
    struct ibv_mr       *mr_rdma_write;
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    void			*buf;
    void            *mr_rdma_write_start_ptr;
    int				size;
    int				rx_depth;
    int				routs;
    struct ibv_port_attr	portinfo;
};

/**
 * lid local id
 * qpn queue pair number
 * psn packet sequence number
 * gid global id
 * The gid and lid of the destination node.
 */
struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
    uint64_t buf_va;
    uint32_t buf_rkey;
};

/**
 * Convert mtu to enum of IBV verbs
 *
 * @param mtu
 * @return an "enum ibv_mtu" type object
 */
enum ibv_mtu pp_mtu_to_enum(int mtu){
    switch (mtu) {
        case 256:  return IBV_MTU_256;
        case 512:  return IBV_MTU_512;
        case 1024: return IBV_MTU_1024;
        case 2048: return IBV_MTU_2048;
        case 4096: return IBV_MTU_4096;
        default:   return -1;
    }
}

/**
 * Get lid of a port of a infiniband device (HCA) represented by context
 *
 * @param context the connection status with HCA
 * @param port the port we want to get the LID for.
 * @return
 */
uint16_t pp_get_local_lid(struct ibv_context *context, int port){
    /*
     * Create an attr struct, store the lid in the attr address, and return
     * attr.lid as lid
     */
    struct ibv_port_attr attr;

    if (ibv_query_port(context, port, &attr))
        return 0;

    return attr.lid;
}

/**
 * Get all attribute of a port
 * encapsulate ibv_query_port
 * @param context
 * @param port
 * @param attr
 * @return
 */
int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}

/**
* Convert the string representation of gid to ibv_gid typed gid
* @param wgid
* @param gid
*/
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid){
    char tmp[9];
    uint32_t v32;
    int i;

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
    }
}

/**
 * Convert a ibv_gid typed gid to string representation
 * @param gid
 * @param wgid
 */
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]){
    int i;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

/**
 * Connect to the remote infiniband device.
 * How? Use ibv_modify_qp to modify fields of QP of local infiniband device,
 * and add info of remote device to it. Then later we we do ibv_post_recv it
 * will take the local QP and know where is the remote device.
 *
 * Transitioning the state of local QP on local device from INIT to RTR to RTS.
 *
 *
 * @param ctx       Connection of our computer to the HCA on the computer
 * @param port      Port of local device we want to connect to
 * @param my_psn    My psn
 * @param mtu       The MTU set by us
 * @param sl
 * @param dest      type pingpong_dest, the lid, qpn, etc... of destination
 * @param sgid_idx
 * @return
 */
static int pp_connect_ctx(struct pingpong_context *ctx, int port, int
my_psn, enum ibv_mtu mtu, int sl, struct pingpong_dest *dest, int
                   sgid_idx){
    struct ibv_qp_attr attr = {
            .qp_state		= IBV_QPS_RTR, // set state to "ready to receive"
            .path_mtu		= mtu,
            .dest_qp_num		= dest->qpn,
            .rq_psn			= dest->psn,
            .max_dest_rd_atomic	= 1,
            .min_rnr_timer		= 12,
            .ah_attr		= {
                    .is_global	= 0,
                    .dlid		= dest->lid,
                    .sl		= sl,
                    .src_path_bits	= 0,
                    .port_num	= port
            }
    };

    if (dest->gid.global.interface_id) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.dgid = dest->gid;
        attr.ah_attr.grh.sgid_index = sgid_idx;
    }

    /*
     * We are changing qp.
     * Set some field of ctx.qp according to values in the given struct attr.
     * Only modify fields that are in attr_mask.
     * I assume these field names are the same for ctx.qp and attr.
     *
     * Here we first set the qp to RTR state, and change the corresponding
     * other attributes of qp that relates to the RTR state.
     */
    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE              |
                      IBV_QP_AV                 |
                      IBV_QP_PATH_MTU           |
                      IBV_QP_DEST_QPN           |
                      IBV_QP_RQ_PSN             |
                      IBV_QP_MAX_DEST_RD_ATOMIC |
                      IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }
    /*
     * Change the qp state and other states again for this qp.
     * Here we move qp from RTR to RTS state, and add some additional
     * configuration specific for the RTS state.
     * Why do we need to first set to RTR and then RTS? The transition
     * of qp through specific states in particular order is important. It's
     * written in NVIDIA doc and ChatGPT.
     */
    attr.qp_state	    = IBV_QPS_RTS;      // change state to "ready to send"
    attr.timeout	    = 14;
    attr.retry_cnt	    = 7;
    attr.rnr_retry	    = 7;
    attr.sq_psn	    = my_psn;
    attr.max_rd_atomic  = 1;

    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE              |
                      IBV_QP_TIMEOUT            |
                      IBV_QP_RETRY_CNT          |
                      IBV_QP_RNR_RETRY          |
                      IBV_QP_SQ_PSN             |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;
}

/**
 * This uses websocket (TCP) to first exchange information between the
 * local and remote node.
 * The information they exchange are pingpong_dest, so basically lid, qpn,
 * psn and gid.
 * This function is "client" first send its info to "server", and get
 * the info of server from response of the server.
 * So we assume the local computer that called this function is the client.
 *
 * Notice: Here I need to add remote key for mr to exchanged info because we
 * need RDMA write.
 *
 * @param servername
 * @param port
 * @param my_dest
 * @return
 */
static struct pingpong_dest *pp_client_exch_dest(const char *servername, int
port,
                                          const struct pingpong_dest *my_dest){
    /*
     * Trying to connect to the other node through 'servername' and 'service'.
     *
     * servername can be a website name or an ip address.
     * service can be something like 'http' or port number.
     *
     * 'getaddrinfo' will resolve the information and give linked list of
     * addrinfo structures: each addrinfo object is a potential
     * internet address (ip, port, socket type) of the given servername and
     * service
     *
     * then we can try to connect to them by order one by one, until we succeed
     * connect to one of them, or we failed all of them.
     */
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_family   = AF_INET,         // IPV4
            .ai_socktype = SOCK_STREAM      // TCP
    };
    char *service;
    int n;
    int sockfd = -1;
    struct pingpong_dest *rem_dest = NULL;

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(servername, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        return NULL;
    }

    /*
     * Send local connection to the other node: lid, qpn, gid, etc...
     * (the struct is pingpong_dest)
     * And then receive the connection information of the other node too.
     * Store the information of the other node.
     */
    char msg[sizeof "0000:000000:000000:0000000000000000:00000000:00000000000000000000000000000000"];
    char gid[33];

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%016lx:%08x:%s", my_dest->lid, my_dest->qpn,
            my_dest->psn, my_dest->buf_va, my_dest->buf_rkey, gid);

    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    if (read(sockfd, msg, sizeof msg) != sizeof msg) {
        perror("client read");
        fprintf(stderr, "Couldn't read remote address\n");
        goto out;
    }

    write(sockfd, "done", sizeof "done");

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%lx:%x:%s", &rem_dest->lid, &rem_dest->qpn,
           &rem_dest->psn, &rem_dest->buf_va, &rem_dest->buf_rkey, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    /*
     * If remote dest is not known then we jump to here.
     */
    out:
    close(sockfd);
    return rem_dest;
}

/**
 * This function serves basically the same purpose as above. It's the server
 * receiving the info from the client and then respond to the client the
 * server's info.
 *
 * So here the first part, establishing TCP connection, is "server listen
 * and accept". And if there is no connection yet, the accept will block the
 * process.
 *
 * Then the second part, exchange info, is "server first get info from
 * client, and then send server info to client".
 *
 * Notice: Here I need to add remote key for mr to exchanged info because we
 * need RDMA write.
 *
 * @param ctx
 * @param ib_port
 * @param mtu
 * @param port
 * @param sl
 * @param my_dest
 * @param sgid_idx
 * @return
 */
static struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
                                          int ib_port, enum ibv_mtu mtu,
                                          int port, int sl,
                                          const struct pingpong_dest *my_dest,
                                          int sgid_idx){
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_flags    = AI_PASSIVE,
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    int n;
    int sockfd = -1, connfd;
    struct pingpong_dest *rem_dest = NULL;

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(NULL, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            n = 1;

            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d\n", port);
        return NULL;
    }

    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, 0);
    close(sockfd);
    if (connfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return NULL;
    }
    char msg[sizeof "0000:000000:000000:0000000000000000:00000000:00000000000000000000000000000000"];
    char gid[33];


    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%lx:%x:%s", &rem_dest->lid, &rem_dest->qpn,
           &rem_dest->psn, &rem_dest->buf_va, &rem_dest->buf_rkey, gid);

    wire_gid_to_gid(gid, &rem_dest->gid);

    /*
     * Here we store the remote node info through pp_connect_ctx, whereas if
     * we are client, we will store the remote info in main.
     */
    if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest, sgid_idx)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }


    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%016lx:%08x:%s", my_dest->lid, my_dest->qpn,
            my_dest->psn, my_dest->buf_va, my_dest->buf_rkey, gid);
    if (write(connfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    read(connfd, msg, sizeof msg);

    out:
    close(connfd);
    return rem_dest;
}

#include <sys/param.h>
/**
 * Initialization:
 * 1. create buffer
 * 2. connect to local infiniband device, and port
 * 3. create completion channel (for poll completion queue)
 * 4. allocate protection domain
 * 5. register memory region
 * 6. create CQ,
 * 7. create QP, set the depth (len) of send and receive queue, set QP state
 * to be init
 *
 * Notice that here is not the same as in template, in template, they set
 * buffer size to be the same as message size, but here I separate this two
 * values, because I need to send multiple messages of different size.
 *
 * Notice: added query qp to init the max inline
 *
 * @param ib_dev
 * @param size          size of message
 * @param rdma_write_mr_size   buffer size (need to be bigger than message
 * size)
 * @param rx_depth
 *
 * @param tx_depth
 * @param port
 * @param use_event
 * @param is_server
 * @return
 */
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
                                     int size, int rdma_write_mr_size,
                                     int rx_depth, int tx_depth, int port,
                                     int use_event, int is_server){
    struct pingpong_context *ctx;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->size     = size;
    ctx->rx_depth = rx_depth;
    ctx->routs    = rx_depth;

    // allocate size for ctx buffer: both the region for control message and
    // the region for rdma write
    ctx->buf = malloc(roundup(size + rdma_write_mr_size, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return NULL;
    }

    // fill up the buffer region with value (if it's server then 124, else 123)
    memset(ctx->buf, 0x7b + is_server, size + rdma_write_mr_size);

    // connect to local device
    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
                ibv_get_device_name(ib_dev));
        return NULL;
    }


    // Create a channel for polling completion queue
    if (use_event) {
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            fprintf(stderr, "Couldn't create completion channel\n");
            return NULL;
        }
    } else
        ctx->channel = NULL;

    // allocate protection domain
    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return NULL;
    }

    // register mr: this mr is for control messages
    ctx->mr_control_send = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr_control_send) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

    // register another mr: this mr is for rdma write
    ctx->mr_rdma_write_start_ptr = (void *)((char*) ctx->buf + size);
    ctx->mr_rdma_write = ibv_reg_mr(ctx->pd, ctx->mr_rdma_write_start_ptr,
                                    rdma_write_mr_size,
                                    IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE);

    // create CQ: the size of CQ = rx_depth + tx_depth
    ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
                            ctx->channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return NULL;
    }

    // create QP
    {
        struct ibv_qp_init_attr attr = {
                .send_cq = ctx->cq,
                .recv_cq = ctx->cq,
                .cap     = {
                        .max_send_wr  = tx_depth,
                        .max_recv_wr  = rx_depth,
                        .max_send_sge = 1,
                        .max_recv_sge = 1
                },
                // here we created reliable qp
                .qp_type = IBV_QPT_RC
        };

        ctx->qp = ibv_create_qp(ctx->pd, &attr);
        if (!ctx->qp)  {
            fprintf(stderr, "Couldn't create QP\n");
            return NULL;
        }
    }

    // move QP state to init
    {
        struct ibv_qp_attr attr = {
                .qp_state        = IBV_QPS_INIT,
                .pkey_index      = 0,
                .port_num        = port,
                .qp_access_flags = IBV_ACCESS_REMOTE_READ |
                                   IBV_ACCESS_REMOTE_WRITE
        };

        if (ibv_modify_qp(ctx->qp, &attr,
                          IBV_QP_STATE              |
                          IBV_QP_PKEY_INDEX         |
                          IBV_QP_PORT               |
                          IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            return NULL;
        }
        // check the max inline data
        struct ibv_qp_attr qp_attr;
        struct ibv_qp_init_attr init_attr;
        if (ibv_query_qp(ctx->qp,  &qp_attr, IBV_QP_CAP, &init_attr)){
            fprintf(stderr, "Failed to query QP for max inline\n");
            return NULL;
        }
        // store the max inline number
        max_inline = qp_attr.cap.max_inline_data;
    }

    return ctx;
}

/**
 * Close a connection
 * Deallocate a lot of "objects" we created (CQ, MR, PD, etc...)
 *
 * Notice: add: need to deregister the mr_rdma_write
 * @param ctx
 * @return
 */
static int pp_close_ctx(struct pingpong_context *ctx){
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr_control_send)) {
        fprintf(stderr, "Couldn't deregister MR_control\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr_rdma_write)) {
        fprintf(stderr, "Couldn't deregister MR_rdma_write\n");
        return 1;
    }

    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ctx->channel) {
        if (ibv_destroy_comp_channel(ctx->channel)) {
            fprintf(stderr, "Couldn't destroy completion channel\n");
            return 1;
        }
    }

    if (ibv_close_device(ctx->context)) {
        fprintf(stderr, "Couldn't release context\n");
        return 1;
    }

    free(ctx->buf);
    free(ctx);

    return 0;
}

/**
 * post receive work request to the receive work queue.
 *  If we cannot post all receive at a time (probably the receive queue is
 *  full), then we break, and return how many receive work request we posted
 *  this time. The rest will be posted next time hopefully.
 *  But in reality, they usually provide the right amount of n to post, if
 *  this function return not exactly n, then the outer scope will raise
 *  error (See implementation of pp_wait_completions)
 *
 * Notice: since post receive will be used for post send, and post send will
 * only be used for control messages, we will just confine the size of the
 * expected receive length.
 *
 * @param ctx
 * @param n
 * @return
 */
static int pp_post_recv(struct pingpong_context *ctx, int n){
    // scatter gather element
    struct ibv_sge list = {
            .addr	= (uintptr_t) ctx->buf,
            // Here it is how much data you are expected to receive
            .length = ctx->size,
            .lkey	= ctx->mr_control_send->lkey
    };
    // work request
    struct ibv_recv_wr wr = {
            .wr_id	    = PINGPONG_RECV_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .next       = NULL
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i)
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
            break;

    return i;
}
/**
 * Post send work request to the send work queue.
 *
 * Why we don't need to worry if send queue is full (like we worried in post
 * receive, and thus we have a loop-break)?
 * probably because in post receive we are posting n receive work
 * request at a time, so we might fail at j << n. Here we are just posting
 * one send work request, so if we fail we immediately know.
 *
 * Notice: The post send will only be used for sending control messages.
 * @param ctx
 * @return
 */
static int pp_post_rdma_write(struct pingpong_context *ctx, struct
        pingpong_dest *rem_dest, int message_size){
    /*
     * The message to send is stored in buffer location
     */
    struct ibv_sge list = {
            .addr	= (uint64_t)ctx->mr_rdma_write_start_ptr,
            .length = message_size,
            // lkey: local key for local MR for rdma write
            .lkey	= ctx->mr_rdma_write->lkey
    };
    unsigned int flags = IBV_SEND_SIGNALED;
    if (message_size <= max_inline){
        flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    }
    struct ibv_send_wr *bad_wr, wr = {
            .wr_id	    = PINGPONG_SEND_WRID,   //todo: use send for now
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_RDMA_WRITE,
            .send_flags = flags,
            .next       = NULL,
            .wr.rdma.rkey = ntohl(rem_dest->buf_rkey),
            .wr.rdma.remote_addr = bswap_64(rem_dest->buf_va),
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}
/**
 * Post send work request to the send work queue.
 *
 * Why we don't need to worry if send queue is full (like we worried in post
 * receive, and thus we have a loop-break)?
 * probably because in post receive we are posting n receive work
 * request at a time, so we might fail at j << n. Here we are just posting
 * one send work request, so if we fail we immediately know.
 *
 * Notice: The post send will only be used for sending control messages.
 * @param ctx
 * @return
 */
static int pp_post_send(struct pingpong_context *ctx){
    /*
     * The message to send is stored in buffer location
     */
    struct ibv_sge list = {
            .addr	= (uint64_t)ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr_control_send->lkey     // lkey: local key for local MR
    };
    unsigned int flags = IBV_SEND_SIGNALED;
    if (ctx->size <= max_inline){
        flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    }
    struct ibv_send_wr *bad_wr, wr = {
            .wr_id	    = PINGPONG_SEND_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = flags,
            .next       = NULL
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

/**
 * Poll the completion queue and see see how many receive work request and
 * send work request have been completed. Increment number of completed send
 * wr (scnt) and number of completed receive wr (rcnt) accordingly.
 * If rcnt + scnt >= iters, then finish polling.
 *
 * During processing, we also monitor the receive queue, if the number of
 * receive work requests are lower than 10, we will fill up the receive
 * queue with receive work requests.
 *
 * We made a change to the template: here we will wait for exactly iters
 * number of completion.
 *
 * Notice: since we will use post_send and post_receive just for control
 * messages, technically we don't need to fill up the receive queue all the
 * time. But for sake of simplicity I will keep this design.
 *
 * @param ctx
 * @param iters
 * @return
 */
int pp_wait_completions(struct pingpong_context *ctx, int iters){
    int rcnt = 0, scnt = 0;
    while (rcnt + scnt < iters) {
        // wc: work completion buffer
        struct ibv_wc wc[WC_BATCH];
        int ne, i;

        // poll cq
        do {
            // poll at most WC_BATCH from the CQ. But, we make sure we will
            // poll exactly "iters" number of entries from the CQ.
            int num_poll = (WC_BATCH > iters - rcnt - scnt)? iters - rcnt -
                                                             scnt : WC_BATCH;
            ne = ibv_poll_cq(ctx->cq, num_poll, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);

        // ne: (number of entries): number of completion returned by
        // ibv_poll_cq
        for (i = 0; i < ne; ++i) {
            // check correctness
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                return 1;
            }

            // processed the item in CQ that we polled: increment scnt and
            // rcnt counter, so that the main loop continues.
            switch ((int) wc[i].wr_id) {
                case PINGPONG_SEND_WRID:
                    ++scnt;
                    break;


                case PINGPONG_RECV_WRID:
                    /*
                     * If number of receive request in in Receive queue is below
                     * certain threshold, it will post another n receive
                     * requests where n = ctx->rx_depth - ctx->routs; basically
                     * fill up the receive queue.
                     * Notice, each iteration we are just processing one receive
                     * . So we increment receive counter by one.
                     *
                     * routs: number of receive work requests that are currently
                     * in the receive queue (technically rwr that are currently
                     * not completed).
                     * rx_depth: max number of receive work request that can be
                     * posted to the receive queue.
                     *
                     * 1. decrement ctx->routs by 1: because one of the receive
                     * work request is completed.
                     * 2. increment ctx->routs by i that is returned by
                     * pp_post_recv, i indicates how many new receive wr we
                     * successfully posted into the receive queue.
                     *
                     * If we didn't successfully post all receive request (post
                     * n RWR into RQ so that RQ is 'full' (according to
                     * rx_depth), then we raise error.
                     *
                     * So basically we will 'fill up' RQ once the number of RWR
                     * inside is less than REFILL_RWR_THRES.
                     */
                    if (--ctx->routs <= REFILL_RWR_THRES) {
                        ctx->routs += pp_post_recv(ctx, ctx->rx_depth - ctx->routs);
                        if (ctx->routs < ctx->rx_depth) {
                            fprintf(stderr,
                                    "Couldn't post receive (%d)\n",
                                    ctx->routs);
                            return 1;
                        }
                    }
                    ++rcnt;
                    break;

                default:
                    fprintf(stderr, "Completion for unknown wr_id %d\n",
                            (int) wc[i].wr_id);
                    return 1;
            }
        }

    }
    return 0;
}

#include <unistd.h>

/**
 * Calculate throughput for given message size
 * Sending n messages from client to server, and server send 1 message back
 * to client. n = iters
 * the ctx will contain information about how big is the size of each message.
 *
 * In pp_wait_completion, we will wait for exactly iters
 * number of completion. This is important, else there will be bug! because
 * sometimes clients might send too fast, server are still polling it's
 * previous trail, the client already sent message for the new trial to server.
 *
 * This bug can also be solved if we don't change pp_wait_completion: server
 * can send a message to tell client to start trial, and then client will
 * only start trail after receive this message. (Then the rest is the same,
 * server receive message from client, send confirmation, client receive
 * confirmation, trail finished)
 *
 * But I feel like wait for exact n completion is a more natural solution.
 *
 * @param servername
 * @param ctx
 * @param tx_depth length/depth of send work queue
 * @param iters number of messages to send
 * @return
 */
int perform_single_experiment(char *servername, int warmup,
                              struct pingpong_context *ctx, int tx_depth,
                              int iters,
                              struct pingpong_dest *rem_dest, int message_size){
    /*
     * If we are client,
     * we will post send, we are posting n send work requests in total.
     * But every tx_depth sending, we will wait until all of them is
     * complete before we continue.
     * After we complete all n send wr (notice to check the final k wr), we
     * will wait for an additional 1 wr completion: this will be the
     * completion of a receive work request: getting the response form server.
     * (we don't need to post receive because pp_wait_completions will do it
     * for us).
     *
     * If we are server, we will wait for completion of n work requests.
     * Since we didn't send anything yet, this n wr will only
     * complete if we received n messages. (Because we always fill up the
     * receive queue during pp_wait_completions, so we don't need to worry
     * about post_receive)
     * Then we will post a send work request as response and wait for one wr
     * completion.
     *
     * Notice: since server will also send one message to client, when
     * client is waiting for completion, it will get one receive work
     * request completed in the CQ, that means in that iteration, the client
     * might post 10 send, but the pp_wait_completions will return as long
     * as there are 9 sends are completed + 1 receive completed. So this is a
     * bug.
     */
    if (servername) {
        // start the trial
        int i;
        int num_complete = 0;
        for (i = 0; i < iters ; i++) {
            // wait for the current tx_depth send_wr to complete before we
            // continue.
            if ((i != 0) && (i % tx_depth == 0)) {
                pp_wait_completions(ctx, tx_depth);
                num_complete += tx_depth;
            }

            if (pp_post_rdma_write(ctx, rem_dest, message_size)){
                fprintf(stderr, "Client couldn't post RDMA write\n");
                return 1;
            }
        }


        // tell the server that the writing is done.
        if (pp_post_send(ctx)) {
            fprintf(stderr, "Client couldn't post send\n");
            return 1;
        }
        // wait for the send to complete, and wait for the server to answer
        // with one control message: complete.
        pp_wait_completions(ctx, iters - num_complete + 2);

    } else {
        // wait for client to send the finish message.
        pp_wait_completions(ctx, 1);

        // send a response to client (So that client can stop counting time.
        if (pp_post_send(ctx)) {
            fprintf(stderr, "Server couldn't post send\n");
            return 1;
        }
        // waiting for sending wr to complete + it's send request to complete.
        pp_wait_completions(ctx, 1);

    }
    return 0;

}
/**
 * Since the post send here is using a reliable connection, do we need to
 * send a respond message back?
 * I guess it is negligible.
 *
 * @param servername
 * @param warmup
 * @param ctx
 * @param tx_depth
 * @param list_message_size
 * @param len_list_message_size
 * @param iters
 * @return
 */
int throughput_test(char *servername, int warmup,
                    struct pingpong_context *ctx, int tx_depth,
                    int iters, struct pingpong_dest *rem_dest){
    int list_message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
                                2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144,
                                524288, 1048576};
    int len_list_message_sizes = 21;
    for (int i = 0; i < len_list_message_sizes; i++){
        // set the message size for this trial
        int message_size = list_message_sizes[i];

        // strat timer
        struct timeval start_time, end_time;
        long time_elapse;
        gettimeofday(&start_time, NULL);

        // perform the trial
        perform_single_experiment(servername,warmup, ctx, tx_depth, iters,
                                  rem_dest, message_size);

        // calculate time elapsed
        gettimeofday(&end_time, NULL);
        // calculate time: in seconds
        double const total_second = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec)/1000000.0;

        // calculate throughput: afraid of overflow
        double const total_sent_Mb = iters * (message_size * 8 / 1048576.0);
        double const throughput = total_sent_Mb / total_second;

        // print the throughput: In MegaBytes per second
        if (!warmup && servername){
            printf("%d\t\t%.2f\t\tMbits/sec\n", message_size, throughput);
        }
    }

}

/**
 * We will start from client send one message to server, and then once
 * server receive one message, it will send one message back to client, and
 * then once client receive the message, it will send another one to server.
 * @param servername
 * @param warmup
 * @param ctx
 * @param tx_depth
 * @param list_message_sizes
 * @param len_list_message_sizes
 * @param iters
 * @return
 */
int latency_test(char *servername, int warmup,
                 struct pingpong_context *ctx, int iters){
    // strat timer
    struct timeval start_time, end_time;
    long time_elapse;
    gettimeofday(&start_time, NULL);

    for (int i = 0; i < iters; i++) {
        if (servername) {
            // send one message to server
            if (pp_post_send(ctx)) {
                fprintf(stderr, "Client couldn't post send\n");
                return 1;
            }

            // wait for response from server + send work complete
            pp_wait_completions(ctx, 2);

        }else{
            if (i == 0){
                // wait for first message from client
                pp_wait_completions(ctx, 1);
            }


            if (pp_post_send(ctx)) {
                fprintf(stderr, "Client couldn't post send\n");
                return 1;
            }

            if (i == iters - 1){
                // last time there will not be next message from client
                pp_wait_completions(ctx, 1);
            }else{
                // wait for send wr complete + next message from client
                pp_wait_completions(ctx, 2);
            }
        }
    }

    // calculate time elapsed
    gettimeofday(&end_time, NULL);
    // calculate time: in milliseconds
    double const round_trip_time = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_usec - start_time.tv_usec)/1000.0;

    // calculate latency
    double const latency = round_trip_time/ (iters * 2.0);

    // print the latency
    if (!warmup && servername){
        printf("Latency:\t\t%.4f\tmilliseconds\n", latency);
    }

    return 0;
}
/**
 * Funtion that provides usage to user
 * @param argv0
 */
static void usage(const char *argv0){
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("  %s <host>     connect to server at <host>\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
    printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
    printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
    printf("  -s, --size=<size>      size of message to exchange (default 4096)\n");
    printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
    printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
    printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
    printf("  -l, --sl=<sl>          service level value\n");
    printf("  -e, --events           sleep on CQ events (default poll)\n");
    printf("  -g, --gid-idx=<gid index> local port gid index\n");
}

/**
 *
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char *argv[])
{
    struct ibv_device      **dev_list;
    struct ibv_device       *ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest     my_dest;
    struct pingpong_dest    *rem_dest;
    char                    *ib_devname = NULL;
    char                    *servername;
    int                      port = 12345;
    int                      ib_port = 1;
    enum ibv_mtu             mtu = pp_mtu_to_enum(MTU) ;// mtu
    int                      rx_depth = RX_DEPTH;   // The length of receive queue
    int                      tx_depth = TX_DEPTH;    // The length of send queue
    int                      iters = NUMBER_MESSAGES;      // number of message to send
    int                      use_event = 0;         // poll CQ or not
    int                      size = 1;           // control message length
    // buffer size for RDMA Write
    int                      rdma_write_mr_size = MR_RDMA_WRITE_SIZE;
    int                      sl = 0;                // service level

    // the gid index: if set to -1 then we will set gid to 0, else, we will
    // actually query gid for the local device
    int                      gidx = -1;
    // empty buffer for inet_ntop to store the my_dest.gid, later for printing.
    char                     gid[33];

    srand48(getpid() * time(NULL));
    /*
     * The first part is to parse command line options
     * Try to minimize the while loop for readability
     */
    while (1) {
        int c;
        static struct option long_options[] = {
                { .name = "port",     .has_arg = 1, .val = 'p' },
                { .name = "ib-dev",   .has_arg = 1, .val = 'd' },
                { .name = "ib-port",  .has_arg = 1, .val = 'i' },
                { .name = "size",     .has_arg = 1, .val = 's' },
                { .name = "mtu",      .has_arg = 1, .val = 'm' },
                { .name = "rx-depth", .has_arg = 1, .val = 'r' },
                { .name = "iters",    .has_arg = 1, .val = 'n' },
                { .name = "sl",       .has_arg = 1, .val = 'l' },
                { .name = "events",   .has_arg = 0, .val = 'e' },
                { .name = "gid-idx",  .has_arg = 1, .val = 'g' },
                { 0 }
        };



        c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:eg:", long_options, NULL);
        if (c == -1)
            break;

        switch (c) {
            case 'p':
                port = strtol(optarg, NULL, 0);
                if (port < 0 || port > 65535) {
                    usage(argv[0]);
                    return 1;
                }
                break;

            case 'd':
                ib_devname = strdup(optarg);
                break;

            case 'i':
                ib_port = strtol(optarg, NULL, 0);
                if (ib_port < 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;

            case 's':
                size = strtol(optarg, NULL, 0);
                break;

            case 'm':
                mtu = pp_mtu_to_enum(strtol(optarg, NULL, 0));
                if (mtu < 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;

            case 'r':
                rx_depth = strtol(optarg, NULL, 0);
                break;

            case 'n':
                iters = strtol(optarg, NULL, 0);
                break;

            case 'l':
                sl = strtol(optarg, NULL, 0);
                break;

            case 'e':
                ++use_event;
                break;

            case 'g':
                gidx = strtol(optarg, NULL, 0);
                break;

            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind == argc - 1)
        servername = strdup(argv[optind]);
    else if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE);

    /*
     * This part is trying to see if there is local infiniband devices
     * If yes then get the first device that has a device name
     * and then Initialize the device.
     */
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    if (!ib_devname) {
        ib_dev = *dev_list;
        if (!ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        int i;
        for (i = 0; dev_list[i]; ++i)
            if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
                break;
        ib_dev = dev_list[i];
        if (!ib_dev) {
            fprintf(stderr, "IB device %s not found\n", ib_devname);
            return 1;
        }
    }

    ctx = pp_init_ctx(ib_dev, size, rdma_write_mr_size, rx_depth, tx_depth,
                      ib_port,
                      use_event,
                      !servername);
    if (!ctx)
        return 1;

    /*
     * "Fill up" the receive queue with receive work requests
     * If use_event means we will use channels to notify CQ when a task is done
     */
    ctx->routs = pp_post_recv(ctx, ctx->rx_depth);
    if (ctx->routs < ctx->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", ctx->routs);
        return 1;
    }

    if (use_event)
        if (ibv_req_notify_cq(ctx->cq, 0)) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            return 1;
        }

    /*
     * Get port info for the local infiniband device
     * Make sure the local device is a infiniband device and it has a lid
     * Get the gid of local device, if gidx > 0, else set my gid to 0.
     * store the lid, gid, pqn, psn to a pingpong_dest struct called my_dest
     * my_dest is the info for my node, remote_dest is for the node I am
     * connecting to.
     * Also, get the rkey and virtual mr address of myself (server), later
     * send it to the client so that the client can directly write to my MR
     */
    if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    my_dest.lid = ctx->portinfo.lid;
    if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }

    if (gidx >= 0) {
        if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
            fprintf(stderr, "Could not get local gid for gid index %d\n", gidx);
            return 1;
        }
    } else
        memset(&my_dest.gid, 0, sizeof my_dest.gid);

    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);

    // The remote memory region and remote key
    my_dest.buf_va = bswap_64((uintptr_t)ctx->mr_rdma_write_start_ptr);
    my_dest.buf_rkey = htonl(ctx->mr_rdma_write->rkey);

    /*
     * If servername is provided, then we are running main as a client, so
     * we will exchange info with remote host using pp_client_exch_dest,
     * else we are the server, so we will exchange with pp_server_exch_dest
     * Notice that pp_server_exch_dest will block the process untill the
     * client connects to it.
     *
     * Then we will print the remote's info
     */
    if (servername)
        rem_dest = pp_client_exch_dest(servername, port, &my_dest);
    else
        rem_dest = pp_server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest, gidx);

    if (!rem_dest)
        return 1;

    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);

    /*
     * If we are client, we will try to establish connection with the remote
     * If we are server, we already establish connection with remote during
     * the pp_server_exch_dest above. But this will happen only after client
     * make connection with the server, because the server will be blocked at
     * pp_server_exch_dest until the client connect to it.
     */
    if (servername)
        if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx))
            return 1;

    /*
     * warm up
     * each warmup cycle takes NUM_OF_MESSAGES=iters round trips
     */
    int warmup = 1;
    latency_test(servername, warmup, ctx, iters);

    /*
     * throughput test
     */
    warmup = 0;
    throughput_test(servername, warmup, ctx, tx_depth, iters, rem_dest);

//    /*
//     * Latency test
//     */
//    latency_test(servername, warmup, ctx, iters);

    /*
     * Free everything
     */
    pp_close_ctx(ctx);
    ibv_free_device_list(dev_list);
    free(rem_dest);
    return 0;
}


