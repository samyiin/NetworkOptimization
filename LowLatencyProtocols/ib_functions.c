//
// Created by hsiny on 9/15/24.
//

#include "ib_functions.h"

/*
 * This variable will store the max incline data for the qp
 */
uint32_t max_inline;

/**
 * This variable is depend on each machine. We will assign it's value in
 * main funciton.
 */
int page_size;

/**
 * Convert mtu to enum of IBV verbs
 *
 * @param mtu
 * @return an "enum ibv_mtu" type object
 */
enum ibv_mtu pp_mtu_to_enum(int mtu) {
    switch (mtu) {
        case 256:
            return IBV_MTU_256;
        case 512:
            return IBV_MTU_512;
        case 1024:
            return IBV_MTU_1024;
        case 2048:
            return IBV_MTU_2048;
        case 4096:
            return IBV_MTU_4096;
        default:
            return -1;
    }
}

/**
 * Get lid of a port of a infiniband device (HCA) represented by context
 *
 * @param context the connection status with HCA
 * @param port the port we want to get the LID for.
 * @return
 */
uint16_t pp_get_local_lid(struct ibv_context *context, int port) {
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
                     struct ibv_port_attr *attr) {
    return ibv_query_port(context, port, attr);
}

/**
* Convert the string representation of gid to ibv_gid typed gid
* @param wgid
* @param gid
*/
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid) {
    char tmp[9];
    uint32_t v32;
    int i;

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        *(uint32_t *) (&gid->raw[i * 4]) = ntohl(v32);
    }
}

/**
 * Convert a ibv_gid typed gid to string representation
 * @param gid
 * @param wgid
 */
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]) {
    int i;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *) (gid->raw + i * 4)));
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
int pp_connect_ctx(struct pingpong_context *ctx, int port, int
my_psn, enum ibv_mtu mtu, int sl, struct pingpong_dest *dest, int
                   sgid_idx) {
    struct ibv_qp_attr attr = {
            .qp_state        = IBV_QPS_RTR, // set state to "ready to receive"
            .path_mtu        = mtu,
            .dest_qp_num        = dest->qpn,
            .rq_psn            = dest->psn,
            .max_dest_rd_atomic    = 1,
            .min_rnr_timer        = 12,
            .ah_attr        = {
                    .is_global    = 0,
                    .dlid        = dest->lid,
                    .sl        = sl,
                    .src_path_bits    = 0,
                    .port_num    = port
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
                      IBV_QP_STATE |
                      IBV_QP_AV |
                      IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN |
                      IBV_QP_RQ_PSN |
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
    attr.qp_state = IBV_QPS_RTS;      // change state to "ready to send"
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = my_psn;
    attr.max_rd_atomic = 1;

    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE |
                      IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY |
                      IBV_QP_SQ_PSN |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;
}

/**
 * This uses websocket (TCP) to first exchange information between the
 * local and remote node.
 * The information they exchange are pingpong_dest, so basically
 *         lid, qpn, psn and gid.
 *
 * This function is client first send its info to server, and get
 * the info of server from response of the server.
 * We assume the local computer that called this function is the client.
 *
 * @param servername
 * @param port
 * @param my_dest
 * @return
 */
struct pingpong_dest *pp_client_exch_dest(const char *servername, int
port,
                                          const struct pingpong_dest *my_dest) {
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
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    char gid[33];

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
            my_dest->psn, gid);

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

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
           &rem_dest->psn, gid);
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
 * So here the first part, establishing TCP connection, server listen
 * and accept. And if there is no connection yet, the accept will block the
 * process.
 *
 * Then the second part, exchange info, server first get info from
 * client, and then send server info to client.
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
struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
                                          int ib_port, enum ibv_mtu mtu,
                                          int port, int sl,
                                          const struct pingpong_dest *my_dest,
                                          int sgid_idx) {
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
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    char gid[33];


    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: Couldn't read remote address\n", n,
                (int) sizeof msg);
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
           &rem_dest->psn, gid);

    wire_gid_to_gid(gid, &rem_dest->gid);

    /*
     * Here we store the remote node info through pp_connect_ctx, whereas if
     * we are client, we will store the remote info in main.
     */
    if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
                       sgid_idx)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }


    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
            my_dest->psn, gid);
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
 * What's Different From Template?
 * 1. in template, the buffer is only used for one memory region: the memory
 * region for rdma_send. We make the buffer bigger, and separate to two
 * memory regions, the first is for controlled message (small messages like
 * rdma_send), the second if for bigger messages for rdma_write.
 *
 * mr_control_send uses the memory region where 'mr_send_start_ptr' points at,
 * and the mr_control_size is 'mr_control_size',
 * mr_rdma_write starts at where 'mr_rdma_write_start_ptr' points at,
 *
 * 2. added ibv_query_qp to init the max inline
 *
 * @param ib_dev
 * @param mr_control_size
 * @param rx_depth                  max number of receive wr in receive queue
 * @param tx_depth                  max number of send wr in send queue
 * @param port
 * @param use_event
 * @param is_server
 * @return
 */
struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
                                     int mr_control_size,
                                     int rx_depth, int tx_depth, int port,
                                     int use_event, int is_server) {
    struct pingpong_context *ctx;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx){
        return NULL;
    }


    /// The default message size is the max_size of control message: 4KB
    ctx->mr_control_size = mr_control_size;
    ctx->rx_depth = rx_depth;
    ctx->routs = rx_depth;

    /// connect to local device
    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
                ibv_get_device_name(ib_dev));
        return NULL;
    }


    /// Create a channel for polling completion queue
    if (use_event) {
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            fprintf(stderr, "Couldn't create completion channel\n");
            return NULL;
        }
    } else
        ctx->channel = NULL;

    /// allocate protection domain
    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return NULL;
    }

    /// register mr:
    // one mr for sending messages, rx_depth mr for receiving messages
    int buffer_size = roundup(ctx->mr_control_size * (rx_depth + 1),
                              page_size);
    void *buffer = malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return NULL;
    }
    // fill up the buffer region with value (if it's server then 124, else 123)
    memset(buffer, 0x7b + is_server, buffer_size);

    // the first mr is for sending messages
    ctx->mr_send_start_ptr = buffer;
    ctx->mr_control_send = ibv_reg_mr(ctx->pd, ctx->mr_send_start_ptr,
                                      ctx->mr_control_size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr_control_send) {
        fprintf(stderr, "Couldn't register send MR\n");
        return NULL;
    }

    // the rest rx_depth mr is for receiving messages
    ctx->array_mr_receive_info = malloc(sizeof(struct MRInfo) * rx_depth);
    for (int i = 0; i < rx_depth; i++){
        // for readability, we create a ptr that points to the mr_info
        struct MRInfo *ptr_mr_info = &ctx->array_mr_receive_info[i];
        ptr_mr_info->mr_start_ptr = buffer + (i + 1) *  mr_control_size;
        ptr_mr_info->mr = ibv_reg_mr(ctx->pd, ptr_mr_info->mr_start_ptr,
                                     ctx->mr_control_size, IBV_ACCESS_LOCAL_WRITE);
        if (!ptr_mr_info->mr) {
            fprintf(stderr, "Couldn't register receive MR\n");
            return NULL;
        }
        ptr_mr_info->mr_status = FREE;
    }



    /// create CQ: the mr_control_size of CQ = rx_depth + tx_depth
    ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
                            ctx->channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return NULL;
    }

    /// create QP: CQ for send and receive are the same
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
        if (!ctx->qp) {
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
                          IBV_QP_STATE |
                          IBV_QP_PKEY_INDEX |
                          IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            return NULL;
        }
        // get the max inline data
        struct ibv_qp_attr qp_attr;
        struct ibv_qp_init_attr init_attr;
        if (ibv_query_qp(ctx->qp, &qp_attr, IBV_QP_CAP, &init_attr)) {
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
 * What's Different From Template?
 * 1. Instead of deallocate mr, we deallocate mr_control and mr_rdma_write
 *
 * @param ctx
 * @return
 */
int pp_close_ctx(struct pingpong_context *ctx) {
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr_control_send)) {
        fprintf(stderr, "Couldn't deregister MR_control_send\n");
        return 1;
    }

    // deregister all the memory region for
    for (int i = 0; i < ctx->rx_depth; i++){
        struct MRInfo receive_mr_info = ctx->array_mr_receive_info[i];
        if (ibv_dereg_mr(receive_mr_info.mr)) {
            fprintf(stderr, "Couldn't deregister MR_control_send\n");
            return 1;
        }
    }
    // free the array_mr_receive_info
    free(ctx->array_mr_receive_info);

    // mr_send_start_ptr is also the pointer of total buffer, we malloc all
    // these together
    free(ctx->mr_send_start_ptr);


    /// If we registered the start pointer
    if (ctx->mr_rdma_write_start_ptr != NULL){
        if (ibv_dereg_mr(ctx->mr_rdma_write)) {
            fprintf(stderr, "Couldn't deregister MR_rdma_write\n");
            return 1;
        }
        free(ctx->mr_rdma_write_start_ptr);
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


    free(ctx);

    return 0;
}

/**
 * post receive work request to the receive work queue.
 * We made an adjustments to this function: it will fill up the receive queue.
 *
 * @param ctx
 * @param n
 * @return if successfully filled all receive: 0, else: 1
 */
int pp_post_recv(struct pingpong_context *ctx) {
    for (int i = 0; i < ctx->rx_depth; i++){
        struct MRInfo receive_mr_info = ctx->array_mr_receive_info[i];
        if (receive_mr_info.mr_status == FREE){
            /// put this mr to post receive
            // scatter gather element
            struct ibv_sge list = {
                    .addr    = (uintptr_t) receive_mr_info.mr_start_ptr,
                    // Here it is how much data you are expected to receive
                    .length = ctx->mr_control_size,
                    .lkey    = receive_mr_info.mr->lkey
            };
            // work request
            struct ibv_recv_wr wr = {
                    .wr_id        = i,      // the mr_id for this mr_receive
                    .sg_list    = &list,
                    .num_sge    = 1,
                    .next       = NULL
            };
            struct ibv_recv_wr *bad_wr;
            if (ibv_post_recv(ctx->qp, &wr, &bad_wr)){
                return 1;
            }
            /// change the status to in receive queue
            ctx->array_mr_receive_info[i].mr_status = IN_RECEIVE_QUEUE;
        }
    }
    return 0;
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
int pp_post_rdma_write(struct pingpong_context *ctx) {
    /*
     * The message to send is stored in buffer location
     */
    struct ibv_sge list = {
            .addr    = (uint64_t) ctx->mr_rdma_write_start_ptr,
            // This is how much message you are planning to send
            .length = ctx->mr_rdma_write_size,
            // lkey: local key for local MR for rdma write
            .lkey    = ctx->mr_rdma_write->lkey
    };
    unsigned int flags = IBV_SEND_SIGNALED;
    if (ctx->mr_rdma_write_size <= max_inline) {
        flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    }
    struct ibv_send_wr *bad_wr, wr = {
            .wr_id        = PINGPONG_WRITE_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_RDMA_WRITE,
            .send_flags = flags,
            .next       = NULL,
            .wr.rdma.rkey = ntohl(ctx->remote_buf_rkey),
            .wr.rdma.remote_addr = bswap_64(ctx->remote_buf_va),
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

/**
 * Post send work request to the send work queue.
 * Since in post receive, the expected receive message size is
 * mr_control_size, here every message we send will be size of mr_control_size
 *
 * Notice: The post send will only be used for sending control messages.
 * @param ctx
 * @return
 */
int pp_post_send(struct pingpong_context *ctx) {
    /*
     * The message to send is stored in buffer location
     */
    struct ibv_sge list = {
            .addr    = (uint64_t) ctx->mr_send_start_ptr,
            .length = ctx->mr_control_size,
            // lkey: local key for local mr_control
            .lkey    = ctx->mr_control_send->lkey
    };
    unsigned int flags = IBV_SEND_SIGNALED;
    if (ctx->mr_control_size <= max_inline) {
        flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    }
    struct ibv_send_wr *bad_wr, wr = {
            .wr_id        = PINGPONG_SEND_WRID,
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
 * If rcnt + scnt >= n_complete, then finish polling.
 *
 * During processing, we also monitor the receive queue, if the number of
 * receive work requests are lower than REFILL_RWR_THRES, we will fill up the
 * receive queue with receive work requests.
 *
 * This function is used for simply waiting for n work requests to complete.
 * We can not do anything to the completed work request.
 *
 * ===========================================================================
 * We made a change to the template: here we will wait for exactly n_complete
 * number of completion.
 *
 *
 * @param ctx
 * @param n_complete
 * @return
 */
int pp_wait_n_completions(struct pingpong_context *ctx, int n_complete) {
    int rcnt = 0, scnt = 0;
    while (rcnt + scnt < n_complete) {
        /// poll cq
        // wc: work completion buffer
        struct ibv_wc wc[WC_BATCH];
        int ne, i;
        do {
            // poll at most WC_BATCH from the CQ.
            int num_poll = (WC_BATCH > n_complete - rcnt - scnt) ? n_complete - rcnt -
                                                                   scnt : WC_BATCH;
            ne = ibv_poll_cq(ctx->cq, num_poll, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);

        /// process each completed wr
        // ne: number of completion returned by ibv_poll_cq
        for (i = 0; i < ne; ++i) {
            // check correctness
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                return 1;
            }

            // Process each type of request: we change the case to opcode
            int mr_id;
            switch (wc[i].opcode) {
                case IBV_WC_SEND:
                    ++scnt;
                    break;
                case IBV_WC_RDMA_WRITE:
                    ++scnt;
                    break;

                case IBV_WC_RECV:
                    /// Set the receive mr status to FREE
                    // mr id is the wr_id
                    mr_id = (int) wc[i].wr_id;
                    ctx->array_mr_receive_info[mr_id].mr_status = FREE;
                    // if routs are lower than threshold, we refill post_recev
                    if (--ctx->routs <= REFILL_RWR_THRES) {
                        // this will fill up receive queue
                        if (pp_post_recv(ctx) != 0){
                            fprintf(stderr, "wait_complete Couldn't post "
                                            "receive (%d)\n",
                                    ctx->routs);
                            return 1;
                        }
                        ctx->routs = ctx->rx_depth;
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

/**
 * This function will poll one work complete from cq and check if it's
 * successful, then return it to the user.
 * @param ctx
 * @return
 */
struct ibv_wc *pp_wait_next_complete(struct pingpong_context *ctx){
    /// poll one work complete from cq
    struct ibv_wc *wc = malloc(sizeof (struct ibv_wc));
    int ne;
    do {
        // poll 1 request from the CQ.
        int num_poll = 1;
        ne = ibv_poll_cq(ctx->cq, num_poll, wc);
        if (ne < 0) {
            fprintf(stderr, "poll CQ failed %d\n", ne);
            return NULL;
        }

    } while (ne == 0);
    /// Check if the work complete is successful
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc->status),
                wc->status, (int) wc->wr_id);
        return NULL;
    }
    return wc;
}

