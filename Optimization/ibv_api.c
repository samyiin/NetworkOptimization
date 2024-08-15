#include "ibv_api.h"
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


int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}
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

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]){
    int i;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

int pp_connect_ctx(struct pingpong_context *ctx, int port, int
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


struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
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
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

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
    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
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

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    /*
     * If remote dest is not known then we jump to here.
     */
    out:
    close(sockfd);
    return rem_dest;
}

struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
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
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

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

    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
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
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
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

struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
        int size, int buffer_size,
        int rx_depth, int tx_depth, int port,
        int use_event, int is_server){
    struct pingpong_context *ctx;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->size     = size;
    ctx->rx_depth = rx_depth;
    ctx->routs    = rx_depth;

    // define page size
    ctx->buf = malloc(roundup(buffer_size, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return NULL;
    }
    ctx->buf_size = buffer_size;

    /*
     * Create buffer
     * if it's server, then the buffer starting point is different, I don't
     * know what this is for, but I know that it is not for testing locally,
     * we still cannot open client and server locally.
     */
    memset(ctx->buf, 0x7b + is_server, size);

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

    // register mr
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, buffer_size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

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
    }

    return ctx;
}

int pp_close_ctx(struct pingpong_context *ctx){
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
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


int pp_post_recv(struct pingpong_context *ctx, int n){
    // scatter gather element
    struct ibv_sge list = {
            .addr	= (uintptr_t) ctx->buf,
            // Here it is how much data you are expected to receive
            .length = ctx->buf_size,
            .lkey	= ctx->mr->lkey
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

int pp_post_send(struct pingpong_context *ctx){
    /*
     * The message to send is stored in buffer location
     */
    struct ibv_sge list = {
            .addr	= (uint64_t)ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
            .wr_id	    = PINGPONG_SEND_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
            .next       = NULL
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

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
