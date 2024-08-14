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
 */
#define _GNU_SOURCE

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

#define WC_BATCH (10)

/**
 * pingpong receive work request id
 */
enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};


static int page_size;

/**
 * context
 * channel
 * pd               protection domain
 * mr               memory region
 * cq               completion queue
 * qp               queue pair
 * buf              the pointer to the buffer that holds the data being send
 * size             size of buffer (since it's a pointer)
 * rx_depth         depth of receive queue: how many receive work request
 *                  can be posted in advance.
 * routs            how many rounds of iteration in the pingpong test
 * portinfo         information of the port (port state, MTU, other config)
 */
struct pingpong_context {
    struct ibv_context		*context;
    struct ibv_comp_channel	*channel;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr;
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    void			*buf;
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
};

enum ibv_mtu pp_mtu_to_enum(int mtu)
{
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
uint16_t pp_get_local_lid(struct ibv_context *context, int port)
{
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
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
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
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    int i;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

/**
 * Connect to the remote HCA. Transitioning the state of qp on local device
 * from INIT to RTR to RTS.
 * Also the info of dest is also provided, so we connect to the destination
 * through modifying the local device's attribute. More precisely, we are
 * modifying the qp of the local device: adding the remote info to this
 * local qp.
 * Later the ibv_... will handle post send giving the qp have the info of
 * the remote HCA
 *
 * @param ctx       Connection of our computer to the HCA on the computer
 * @param port
 * @param my_psn
 * @param mtu
 * @param sl
 * @param dest      type pingpong_dest, the info of destination
 * @param sgid_idx
 * @return
 */
static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                          enum ibv_mtu mtu, int sl,
                          struct pingpong_dest *dest, int sgid_idx)
{
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

    /**
     * Set some field of ctx.qp by the given struct attr. We are changing qp.
     * Only modify fields that are in attr_mask.
     * I assume these field names are the same for ctx.qp and attr.
     * Here we first set the qp to RTR state, and change the corresponding
     * other attributes of qp that relates to the RTR state.
     *
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
    /**
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
 * sender and receiver node.
 * The information they exchange are pingpong_dest, so basically lid, qpn,
 * psn and gid.
 * This function is "client" trying to send its info to "server", and get
 * the info of server from response of the server.
 * @param servername
 * @param port
 * @param my_dest
 * @return
 */
static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
                                                 const struct pingpong_dest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    /**
     * Trying to connect to the other node through servername and service.
     * server name can be a website name or a ip address. service can be
     * something like http or port number. this function will resolve the
     * information and give linked list of addrinfo structures: then we can
     * try to connect to them by order one by one, until we succeed connect to
     * one of them or we failed.
     * them
     */
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

    /**
     * Send local connection to the other node: lid, qpn, gid, etc...
     * And then receive the connection information of the other node too.
     * Store the information of the other node
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

    out:
    close(sockfd);
    return rem_dest;
}

/**
 * This function serves basically the same purpose as above. It's the server
 * receiving the info from the client and then respond to the client the
 * server's info.
 * So here the first part, establishing TCP connection, is "server listen
 * and accept". And if there is no connection yet, the accept will block the
 * process.
 *
 * Then the second part, exchange info, is "server first get info from
 * client, and then send server info to client".
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
                                                 int sgid_idx)
{
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

#include <sys/param.h>

/**
 * Initialization:
 * 1. create buffer
 * 2. connect to local infiniband device
 * 3. create completion channel
 * 4. allocate protection domain
 * 5. register memory region
 * 6. create CQ, QP, set QP state to be init
 * @param ib_dev
 * @param size
 * @param rx_depth
 * @param tx_depth
 * @param port
 * @param use_event
 * @param is_server
 * @return
 */
static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                            int rx_depth, int tx_depth, int port,
                                            int use_event, int is_server)
{
    struct pingpong_context *ctx;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->size     = size;
    ctx->rx_depth = rx_depth;
    ctx->routs    = rx_depth;

    ctx->buf = malloc(roundup(size, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return NULL;
    }

    // if it's server, then the buffer starting point is different, in case
    // we are testing locally.
    memset(ctx->buf, 0x7b + is_server, size);

    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
                ibv_get_device_name(ib_dev));
        return NULL;
    }

    if (use_event) {
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            fprintf(stderr, "Couldn't create completion channel\n");
            return NULL;
        }
    } else
        ctx->channel = NULL;

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return NULL;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

    ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
            ctx->channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return NULL;
    }

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
/**
 * Deallocate a lot of "objects" we created.
 * @param ctx
 * @return
 */
int pp_close_ctx(struct pingpong_context *ctx)
{
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

/**
 * post receive
 *  if we cannot post more receive at a time (probably the receive queue is
 *  full), then we break, and return how many receive work request we posted
 *  this time. The rest will be posted next time hopefully.
 * @param ctx
 * @param n
 * @return
 */
static int pp_post_recv(struct pingpong_context *ctx, int n)
{
    struct ibv_sge list = {
            .addr	= (uintptr_t) ctx->buf,
            .length = ctx->size,
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

/**
 * Post send
 *
 * Why we don't need to worry if send queue is full (like we worried in post
 * receive, and thus we have a loop-break)?
 * probably because in post receive we are posting n receive work
 * request at a time, so we might fail at j << n. Here we are just posting
 * one send work request, so if we fail we immediately know.
 * @param ctx
 * @return
 */
static int pp_post_send(struct pingpong_context *ctx)
{
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


/**
 * Poll the completion queue and see see how many receive work request and
 * send work request have been completed. If number of completed send wr +
 * receive wr is bigger than iters, then finish polling.
 * @param ctx
 * @param iters
 * @return
 */
int pp_wait_completions(struct pingpong_context *ctx, int iters)
{
    int rcnt = 0, scnt = 0;
    while (rcnt + scnt < iters) {
        // wc: work completion
        struct ibv_wc wc[WC_BATCH];
        int ne, i;

        // poll cq
        do {
            ne = ibv_poll_cq(ctx->cq, WC_BATCH, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);

        // ne: number of completion returned by ibv_poll_cq
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
                /**
                 * If number of receive request in in Receive queue is below
                 * certain threshold (10), it will post another n receive
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
                 * inside is less than 10.
                 */
                if (--ctx->routs <= 10) {
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

static void usage(const char *argv0)
{
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
    enum ibv_mtu             mtu = IBV_MTU_2048;
    int                      rx_depth = 100;    // The length of receive queue
    int                      tx_depth = 100;    // The length of send queue
    int                      iters = 1000;
    int                      use_event = 0;
    int                      size = 1;
    int                      sl = 0;
    int                      gidx = -1;
    char                     gid[33];

    srand48(getpid() * time(NULL));

    while (1) {
        /**
         * The first part is to parse command line options
         */
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

    /**
     * This part is trying to see if there is local infiniband devices
     * Run some checks
     * If then get the first device that has a device name
     *
     * and then Initialize the device
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

    ctx = pp_init_ctx(ib_dev, size, rx_depth, tx_depth, ib_port, use_event, !servername);
    if (!ctx)
        return 1;

    /**
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

    /**
     * Get port info for the local infiniband device
     * Make sure the local device is a infiniband device and it has a lid
     * Make sure the local device have a gid
     * store the lid, gid, pqn, psn to a pingpong_dest struct called my_dest
     * my_dest is the info for my node, remote_dest is for the node I am
     * connecting to.
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
    printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           my_dest.lid, my_dest.qpn, my_dest.psn, gid);

    /**
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
    printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

    /**
     * If we are client, we will try to establish connection with the remote
     * If we are server, we already establish connection with remote during
     * the pp_server_exch_dest above.
     */
    if (servername)
        if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx))
            return 1;

    /**
     * If we are client, we will post send, and then after the
     * first post send, we will keep posting send for tx_depth times:
     * basically if the length of send queue is x, we will post x send.
     * After we posted x send, we will wait for the x send is complete. The
     * way we achieve this is through setting iter=tx_depth, so after we
     * posted tx_depth send requests, pp_wait_completions will only return
     * if all tx_depth send requests are completed.
     * We will loop until we send n=iter number of messages. Notice, we are
     * not iterating the above process n=iter times, we are sending n
     * messages, but once every tx_depth sending, we will wait until all of
     * them is complete before we continue.
     *
     * If we are server, we will just post send to the client. And then we
     * will wait for completion. We set pp_wait_completions_iter=iter, but
     * we only send one message, so the rest iter-1 is the completion of
     * receive (from client).
     *
     * Notice: since server will also send one message to client, when
     * client is waiting for completion, it will get one receive work
     * request completed in the CQ, that means in that iteration, the client
     * might post 10 send, but the pp_wait_completions will return as long
     * as there are 9 sends are completed + 1 receive completed. So this is a
     * bug.
     */
    if (servername) {
        int i;
        for (i = 0; i < iters; i++) {
            if ((i != 0) && (i % tx_depth == 0)) {
                pp_wait_completions(ctx, tx_depth);
            }
            if (pp_post_send(ctx)) {
                fprintf(stderr, "Client couldn't post send\n");
                return 1;
            }
        }
        printf("Client Done.\n");
    } else {
        // In our test the server doesn't need to post send. It just needs to
        // receive
        if (pp_post_send(ctx)) {
            fprintf(stderr, "Server couldn't post send\n");
            return 1;
        }
        pp_wait_completions(ctx, iters);
        printf("Server Done.\n");
    }

    /**
     * Free everything
     */
    ibv_free_device_list(dev_list);
    free(rem_dest);
    return 0;
}
