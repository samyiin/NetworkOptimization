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
#ifndef NETWORKOPTIMIZATION_IBV_API_H
#define NETWORKOPTIMIZATION_IBV_API_H

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

/*
 * The larger, the buffer size, then more message the server can receive at
 * once. (I guess?)
 */
#define BUFFER_SIZE 1048576 * 5

/*
 * No matter how much we set RX to be, there is always a possibility that
 * the client are sending so fast, the server will not have enough rec_wr to
 * receive them, Then those send request is queued.
 * But the higher we set RX_DEPTH, the less likely that will happen. But
 * when RX_DEPTH is too high, when we send large messages, speed will drop.
 * probably because it exceeds the buffer size, so the send request is queued?
 */
#define TX_DEPTH 1000
#define RX_DEPTH 1000

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
#define WC_BATCH (100)
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
 * pingpong receive work request id
 */
enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
    };

int page_size;

/**
 * context
 * channel
 * pd               protection domain
 * mr               memory region
 * cq               completion queue
 * qp               queue pair
 * buf              the pointer to the buffer that holds the data being send
 * buf_size         buffer size
 * size             size of message we are sending/receiving
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
    int             buf_size;
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

/**
 * Convert mtu to enum of IBV verbs
 *
 * @param mtu
 * @return an "enum ibv_mtu" type object
 */
enum ibv_mtu pp_mtu_to_enum(int mtu);


/**
 * Get lid of a port of a infiniband device (HCA) represented by context
 *
 * @param context the connection status with HCA
 * @param port the port we want to get the LID for.
 * @return
 */
uint16_t pp_get_local_lid(struct ibv_context *context, int port);

/**
 * Get all attribute of a port
 * encapsulate ibv_query_port
 * @param context
 * @param port
 * @param attr
 * @return
 */
int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr);

/**
* Convert the string representation of gid to ibv_gid typed gid
* @param wgid
* @param gid
*/
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);


/**
 * Convert a ibv_gid typed gid to string representation
 * @param gid
 * @param wgid
 */
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

/**
 * Connect to the remote infiniband device.
 * How? Use ibv_modify_qp to modify fields of QP of local infiniband device,
 * and add info of remote device to it. Then later we we do ibv_post_recv it
 * will take the local QP and know where is the remote device.
 *
 * Transitioning the state of local QP on local device from INIT to RTR to RTS.
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
                sgid_idx);

/**
 * This uses websocket (TCP) to first exchange information between the
 * local and remote node.
 * The information they exchange are pingpong_dest, so basically lid, qpn,
 * psn and gid.
 * This function is "client" first send its info to "server", and get
 * the info of server from response of the server.
 * So we assume the local computer that called this function is the client.
 * @param servername
 * @param port
 * @param my_dest
 * @return
 */
struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
        const struct pingpong_dest *my_dest);

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
                        int sgid_idx);

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
 * @param ib_dev
 * @param size          size of message
 * @param buffer_size   buffer size (need to be bigger than message size)
 * @param rx_depth
 *
 * @param tx_depth
 * @param port
 * @param use_event
 * @param is_server
 * @return
 */
struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
        int size, int buffer_size,
        int rx_depth, int tx_depth, int port,
        int use_event, int is_server);

/**
 * Close a connection
 * Deallocate a lot of "objects" we created (CQ, MR, PD, etc...)
 * @param ctx
 * @return
 */
int pp_close_ctx(struct pingpong_context *ctx);

/**
 * post receive work request to the receive work queue.
 *  If we cannot post all receive at a time (probably the receive queue is
 *  full), then we break, and return how many receive work request we posted
 *  this time. The rest will be posted next time hopefully.
 *  But in reality, they usually provide the right amount of n to post, if
 *  this function return not exactly n, then the outer scope will raise
 *  error (See implementation of pp_wait_completions)
 *
 *  Notice, since the pp_wait_completions will automatically fill up receive
 *  queue, (and I don't want to change that part), but the message we
 *  receive is not always the same size, so I will have to set the expected
 *  receive message size to buffer size (max size).
 *
 * @param ctx
 * @param n
 * @return
 */
int pp_post_recv(struct pingpong_context *ctx, int n);

/**
 * Post send work request to the send work queue.
 *
 * Why we don't need to worry if send queue is full (like we worried in post
 * receive, and thus we have a loop-break)?
 * probably because in post receive we are posting n receive work
 * request at a time, so we might fail at j << n. Here we are just posting
 * one send work request, so if we fail we immediately know.
 * @param ctx
 * @return
 */
int pp_post_send(struct pingpong_context *ctx);

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
 * @param ctx
 * @param iters
 * @return
 */
int pp_wait_completions(struct pingpong_context *ctx, int iters);



#endif // NETWORKOPTIMIZATION_IBV_API_H
