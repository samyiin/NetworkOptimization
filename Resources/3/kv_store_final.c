#include "utils.h"
# define WC_BATCH 1

int g_argc;
char **g_argv;
bool is_server = false;
struct kv_pairs* kv_list = NULL;



enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

static int page_size;

struct pingpong_context {
    struct ibv_context	*context;
    struct ibv_comp_channel *channel;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr;
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    void			*buf;
    int			 size;
    int			 rx_depth;
    int                      routs;
    int			 pending;
    struct ibv_port_attr     portinfo;
};

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

uint16_t pp_get_local_lid(struct ibv_context *context, int port)
{
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

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    int i;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                          enum ibv_mtu mtu, int sl,
                          struct pingpong_dest *dest, int sgid_idx)
{
    struct ibv_qp_attr attr = {
            .qp_state		= IBV_QPS_RTR,
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

    attr.qp_state	    = IBV_QPS_RTS;
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

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                            int rx_depth, int port,
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

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

    ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
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
                        .max_send_wr  = 1,
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

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
    struct ibv_sge list = {
            .addr	= (uintptr_t) ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr->lkey
    };
    struct ibv_recv_wr wr = {
            .wr_id	    = PINGPONG_RECV_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i)
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
            break;

    return i;
}

static int pp_post_send(struct pingpong_context *ctx, enum ibv_wr_opcode opcode, unsigned int size, const char *local_ptr, void *remote_ptr, uint32_t remote_key)
{
    struct ibv_sge list = {
            .addr	= (uintptr_t) (local_ptr ? local_ptr : ctx->buf),
            .length = size,
            .lkey	= ctx->mr->lkey
    };
    struct ibv_send_wr wr = {
            .wr_id	    = PINGPONG_SEND_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = opcode,
            .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_wr;

    if (remote_ptr) {
        wr.wr.rdma.remote_addr = (uintptr_t) remote_ptr;
        wr.wr.rdma.rkey = remote_key;
    }

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
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

struct kv_pairs* get_node_of_key(char* key){
    struct kv_pairs* curr = kv_list;
    while(curr!=NULL){
        if(strcmp(curr->key, key)==0)
            return curr;
        curr = curr->next;
    }
    return NULL;
}



void process_server_packet(struct pingpong_context *ctx){
    struct packet *packet = (struct packet *) ctx->buf;
    unsigned int response_size = 0;
    struct ibv_mr* memory_reg; /// a struct to store a memory region
    struct packet* mr_answer; /// a struct for the packet with MR data (in case of rendezvous)
    struct packet* eager_answer; /// a struct for the packet with value length (in case of eager)

    struct kv_pairs* curr;
    int length_of_val;
    int key_len;
    char* val_to_add;
    switch (packet->type) {
        case GET_INITIAL_MESSAGE:
            curr = get_node_of_key(packet->packet_union.get_initial_message.key);
            if (curr != NULL){
                //// key exists in list
                curr->num_client_gets++;
                length_of_val = strlen(curr->value) + 1; // +1 for null terminator
                if(length_of_val <= EAGER_MAX_MESSAGE_SIZE)
                {
                    /// EAGER PROTOCOL
                    eager_answer = (struct packet *) ctx->buf;
                    eager_answer->type = EAGER_GET_ANSWER;
                    strcpy(eager_answer->packet_union.eager_get_answer.value, curr->value);
                    eager_answer->packet_union.eager_get_answer.value_len = length_of_val;
                    response_size = (unsigned int) ((void *) packet->packet_union.eager_get_answer.value - (void *) packet + length_of_val);
                    curr->num_client_gets--;
                }
                else{
                    /// RENDEZVOUS PROTOCOL
                    memory_reg = ibv_reg_mr(ctx->pd, curr->value, length_of_val, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
                    mr_answer = (struct packet*) ctx->buf;
                    mr_answer->type = RENDEZVOUS_GET_ANSWER;
                    mr_answer->packet_union.rndv_get_answer.remote_addr = memory_reg->addr;
                    mr_answer->packet_union.rndv_get_answer.remote_key = memory_reg->rkey;
                    mr_answer->packet_union.rndv_get_answer.value_len = length_of_val;
                    response_size = packet_sizeof;
                }
            }
            else{
                //// if there is no key - no need for rendezvous (value len = 0 < 4096)
                //// key does not exist in list
                eager_answer = (struct packet*) ctx->buf;
                eager_answer->type = EAGER_GET_ANSWER;
                eager_answer->packet_union.eager_get_answer.value[0] = '\0';
                eager_answer->packet_union.eager_get_answer.value_len = 1;
                response_size = (unsigned int) ((void *) packet->packet_union.eager_get_answer.value - (void *) packet + 1);
            }
            break;

        case EAGER_SET_INITIAL_MESSAGE:
            key_len = strlen(packet->packet_union.eager_set_initial.key_val) + 1;
            length_of_val = strlen(packet->packet_union.eager_set_initial.key_val + key_len) + 1;
            val_to_add = strdup(packet->packet_union.eager_set_initial.key_val + key_len);
            curr = get_node_of_key(packet->packet_union.eager_set_initial.key_val);
            if(curr != NULL) {
                /// key exists in list

                if (curr->num_client_gets == 0) {
                    /// no one else reads this value so we can update it
                    free(curr->value);
                    curr->value = val_to_add;
                    if (curr->futureValue != NULL) {
                        free(curr->futureValue);
                        curr->futureValue = NULL;
                    }
                } else {
                    /// someone else reads this value, so we don't update it yet
                    if (curr->futureValue != NULL) {
                        free(curr->futureValue);
                    }
                    curr->futureValue = val_to_add;
                }
            }
            else{
                /// key does not exists in list
                struct kv_pairs * new_node = (struct kv_pairs*)malloc(kv_pair_size);
                new_node->key = calloc(key_len,1);
                new_node->value = calloc(length_of_val,1);
                strcpy(new_node->key, packet->packet_union.eager_set_initial.key_val);
                strcpy(new_node->value, packet->packet_union.eager_set_initial.key_val + key_len);
                new_node->num_client_gets = 0;
                new_node->futureValue = NULL;
                new_node->next = kv_list;
                kv_list = new_node;
            }
            break;

        case RENDEZVOUS_SET_INITIAL_MESSAGE:
            length_of_val = packet->packet_union.rndv_set_initial.value_len;
            char* buf_for_val = calloc(length_of_val, 1);
            memory_reg = ibv_reg_mr(ctx->pd, buf_for_val, length_of_val, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);

            /// create a response buffer
            mr_answer = (struct packet*) ctx->buf;
            mr_answer->type = RENDEZVOUS_SET_ANSWER;
            mr_answer->packet_union.rndv_set_answer.remote_addr=memory_reg->addr;
            mr_answer->packet_union.rndv_set_answer.remote_key=memory_reg->rkey;
            response_size = packet_sizeof;
            break;
        case FIN_SET:
            key_len = strlen(packet->packet_union.rndv_fin.key) + 1; /// +1 for null terminator
            curr = get_node_of_key(packet->packet_union.rndv_fin.key);
            if (curr != NULL) {
                /// key exists in list
                if(curr->num_client_gets == 0){
                    /// no one reads this node so we can update it
                    free(curr->value);
                    curr->value = packet->packet_union.rndv_fin.val_address;
                    if(curr->futureValue != NULL){
                        free(curr->futureValue);
                        curr->futureValue = NULL;
                    }
                }
                else{
                    /// the value is being read at the moment, so we don't update it yet
                    if(curr->futureValue != NULL){
                        free(curr->futureValue);
                    }
                    curr->futureValue = packet->packet_union.rndv_fin.val_address;
                }
            }
            else {
                /// key does not exist in list
                curr = (struct kv_pairs *) malloc(kv_pair_size);
                curr->key = calloc(key_len, 1);
                curr->value = packet->packet_union.rndv_fin.val_address;
                strcpy(curr->key, packet->packet_union.rndv_fin.key);
                curr->next = kv_list;
                curr->num_client_gets = 0;
                curr->futureValue = NULL;
                kv_list = curr;
            }
            break;

        case FIN_GET:
            curr = get_node_of_key(packet->packet_union.rndv_fin.key);
            //// Because we used rendezvous, the key must exist in the dictionary (otherwise,
            //// we handled it using eager and didn't send a fin message).
            assert(curr != NULL);
            assert(curr->num_client_gets > 0);
            curr->num_client_gets--;
            if(curr->num_client_gets == 0){
                //// no one reads this node so we can update it
                if(curr->futureValue != NULL){
                    printf("FUTURE IS NOT NULL\n");
                    free(curr->value);
                    curr->value = curr->futureValue;
                    curr->futureValue = NULL;
                }
            }
            break;
        default:
            break;
    }

    if (response_size) {
        pp_post_send(ctx, IBV_WR_SEND, response_size, NULL, NULL, 0);
    }
}

int pp_wait_completions(struct pingpong_context *ctx, int iters)
{
    int rcnt, scnt;
    rcnt = scnt = 0;
    while (rcnt + scnt < iters) {
        struct ibv_wc wc[WC_BATCH];
        int ne, i;

        do {
            ne = ibv_poll_cq(ctx->cq, WC_BATCH, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);

        for (i = 0; i < ne; ++i) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                return 1;
            }

            switch ((int) wc[i].wr_id) {
                case PINGPONG_SEND_WRID:
                    scnt++;
                    break;

                case PINGPONG_RECV_WRID:
                    if(is_server){
                        process_server_packet(ctx);
                    }
                    pp_post_recv(ctx, 1);
                    rcnt++;
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

int kv_open(char* servername, void **kv_handle)
{
    struct pingpong_context ** result_ctx = (struct pingpong_context **)kv_handle;
    unsigned int size = EAGER_MAX_MESSAGE_SIZE;
    struct ibv_device      **dev_list;
    struct ibv_device	*ib_dev;
    struct pingpong_context *ctx;
    struct pingpong_dest     my_dest;
    struct pingpong_dest    *rem_dest;
    struct timeval           start, end;
    char                    *ib_devname = NULL;
    int                      ib_port = 1;
    int                      port = port_num;
    enum ibv_mtu		 mtu = IBV_MTU_1024;
    int                      rx_depth = 1;
    int                      iters = 1000;
    int                      use_event = 0;
    int                      routs;
    int                      rcnt, scnt;
    int                      num_cq_events = 0;
    int                      sl = 0;
    int			 gidx = -1;
    char			 gid[33];

    srand48(getpid() * time(NULL));

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

        c = getopt_long(g_argc, g_argv, "p:d:i:s:m:r:n:l:eg:", long_options, NULL);
        printf("c is: %d\n",c);
        if (c == -1)
            break;

        switch (c) {
            case 'p':
                port = strtol(optarg, NULL, 0);
                if (port < 0 || port > 65535) {
                    usage(g_argv[0]);
                    return 1;
                }
                break;

            case 'd':
                ib_devname = strdup(optarg);
                break;

            case 'i':
                ib_port = strtol(optarg, NULL, 0);
                if (ib_port < 0) {
                    usage(g_argv[0]);
                    return 1;
                }
                break;

            case 's':
                size = strtol(optarg, NULL, 0);
                break;

            case 'm':
                mtu = pp_mtu_to_enum(strtol(optarg, NULL, 0));
                if (mtu < 0) {
                    usage(g_argv[0]);
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
                usage(g_argv[0]);
                return 1;
        }
    }



    page_size = sysconf(_SC_PAGESIZE);

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

    ctx = pp_init_ctx(ib_dev, size, rx_depth, ib_port, use_event, !servername);
    if (!ctx)
        return 1;

    routs = pp_post_recv(ctx, ctx->rx_depth);
    if (routs < ctx->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", routs);
        return 1;
    }

    if (use_event)
        if (ibv_req_notify_cq(ctx->cq, 0)) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            return 1;
        }


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


    if (servername)
        rem_dest = pp_client_exch_dest(servername, port, &my_dest);
    else
        rem_dest = pp_server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest, gidx);

    if (!rem_dest)
        return 1;

    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
    printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

    if (servername)
        if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx))
            return 1;

    ibv_free_device_list(dev_list);
    free(rem_dest);
    *result_ctx = ctx;

    return 0;

}

int kv_set(void *kv_handle, const char *key, const char *value)
{
    struct pingpong_context *ctx = kv_handle;
    struct packet *set_packet = (struct packet*)ctx->buf; /// for writing directly on the buffer

    unsigned int packet_size = strlen(key) + strlen(value) + packet_sizeof + 2; /// +2 for null terminators
    if (packet_size <= EAGER_MAX_MESSAGE_SIZE) {
        /// EAGER PROTOCOL
        set_packet->type = EAGER_SET_INITIAL_MESSAGE;
        /// write the key and value on the buffer in the format <key>\0<value>\0
        strcpy(set_packet->packet_union.eager_set_initial.key_val, key);
        strncpy(set_packet->packet_union.eager_set_initial.key_val+strlen(key),"\0",1);
        strcpy(set_packet->packet_union.eager_set_initial.key_val+strlen(key)+1, value);
        strncpy(set_packet->packet_union.eager_set_initial.key_val+strlen(key)+1+strlen(value),"\0",1);
        pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0);
        return pp_wait_completions(ctx, 1);
    }
    /// RENDEZVOUS PROTOCOL
    set_packet->type = RENDEZVOUS_SET_INITIAL_MESSAGE;

    int val_len = strlen(value) + 1;

    set_packet->packet_union.rndv_set_initial.value_len = val_len;
    strcpy(set_packet->packet_union.rndv_set_initial.key, key);

    pp_post_recv(ctx, 1);
    pp_post_send(ctx, IBV_WR_SEND, packet_sizeof + strlen(key) + 1, NULL, NULL, 0);

    pp_wait_completions(ctx, 2);


    void* remote_addr = set_packet->packet_union.rndv_set_answer.remote_addr;
    uint32_t remote_k = set_packet->packet_union.rndv_set_answer.remote_key;


    /// save the old MR of the client before creating a new one (with the properties sent by the server).
    struct ibv_mr* ctxMR = (struct ibv_mr*)ctx->mr;
    struct ibv_mr* clientMR = ibv_reg_mr(ctx->pd, (void*)value, val_len, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    ctx->mr = (struct ibv_mr*) clientMR;
    pp_post_send(ctx, IBV_WR_RDMA_WRITE, val_len, value, remote_addr, remote_k);
    pp_wait_completions(ctx, 1);
    ctx->mr = (struct ibv_mr*) ctxMR;
    ibv_dereg_mr(clientMR);

    //// send fin_set
    struct packet *fin_packet = (struct packet*)ctx->buf;
    packet_size = strlen(key) + packet_sizeof + 1; // +1 for key null terminator
    fin_packet->type = FIN_SET;
    strcpy(fin_packet->packet_union.rndv_fin.key, key);
    strncpy(fin_packet->packet_union.rndv_fin.key + strlen(key),"\0",1);
    fin_packet->packet_union.rndv_fin.val_address = remote_addr;
    pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0);
    return pp_wait_completions(ctx, 1);
}


int kv_get(void *kv_handle, const char *key, char **value)
{
    struct pingpong_context *ctx = kv_handle;
    struct packet *get_packet = (struct packet*)ctx->buf; /// for writing directly on the buffer

    unsigned int packet_size = strlen(key) + packet_sizeof + 1; /// +1 for null terminator

    /// initializing the get_packet struct to send the key to the server
    get_packet->type = GET_INITIAL_MESSAGE;
    strcpy(get_packet->packet_union.get_initial_message.key, key);

    pp_post_recv(ctx, 1);
    /// send the get request to the server
    pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0);

    pp_wait_completions(ctx, 2);

    /// process the get response from the server
    struct packet* server_answer = (struct packet*)ctx->buf;
    if(server_answer->type == EAGER_GET_ANSWER){
        /// EAGER PROTOCOL - copy the value to a newly allocated buffer and return.
        *value = (char*)malloc(server_answer->packet_union.eager_get_answer.value_len);
        strcpy(*value, server_answer->packet_union.eager_get_answer.value);
        return 0;
    }
    /// ELSE RENDEZVOUS PROTOCOL

    int val_len = server_answer->packet_union.rndv_get_answer.value_len;
    void* remote_addr = server_answer->packet_union.rndv_get_answer.remote_addr;
    uint32_t remote_k = server_answer->packet_union.rndv_get_answer.remote_key;


    *value = calloc(val_len,1);
    /// temp_val is used to make sure we copy to the actual buffer the client sent only after the RDMA read is done
    char* temp_val = calloc(val_len,1);

    /// save the old MR of the client before creating a new one (with the properties sent by the server).
    struct ibv_mr* ctxMR = ctx->mr;
    struct ibv_mr* clientMR = ibv_reg_mr(ctx->pd, (void*)temp_val, val_len, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    ctx->mr = (struct ibv_mr*) clientMR;
    pp_post_send(ctx, IBV_WR_RDMA_READ, val_len, temp_val, remote_addr, remote_k);
    pp_wait_completions(ctx, 1);

    /// restore the old MR of the client and deallocate the new one created
    ctx->mr = (struct ibv_mr*) ctxMR;
    ibv_dereg_mr(clientMR);

    /// copy the value to the actual pointer sent by the client and free the temporary one
    strcpy(*value, temp_val);
    free(temp_val);

    //// send fin_get
    struct packet *fin_packet = (struct packet*)ctx->buf;
    packet_size = strlen(key) + packet_sizeof + 1; /// +1 for key null terminator
    fin_packet->type = FIN_GET;
    strcpy(fin_packet->packet_union.rndv_fin.key, key);
    strncpy(fin_packet->packet_union.rndv_fin.key + strlen(key),"\0",1);
    fin_packet->packet_union.rndv_fin.val_address = remote_addr;
    pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0);
    return pp_wait_completions(ctx, 1);

}

void kv_release(char *value)
{
    free(value);
}

int kv_close(void *kv_handle)
{
    return pp_close_ctx((struct pingpong_context*)kv_handle);
}

void free_kv_pairs() {
    struct kv_pairs* current = kv_list;
    while (current != NULL) {
        struct kv_pairs* next = current->next;
        /// Free the dynamically allocated strings
        free(current->key);
        free(current->value);
        if(current->futureValue != NULL)
            free(current->futureValue);
        /// Free the current node
        free(current);
        current = next;
    }
}

int main(int argc, char **argv)
{
    char *servername = NULL;
    if (argc == 2){
        servername = strdup(argv[argc - 1]);
    }
    void *kv_ctx;

    char* send_buffer =(char*) malloc(RENDEZVOUS_TEST_SIZE); /*malloc to buff*/
    char *recv_buffer;

    /// Setting the global variables to point to the arguments so we can use them in kv_open
    g_argc = argc;
    g_argv = argv;

    if (!servername) {
        /// Server Part
        is_server = true;
        kv_open(0, &kv_ctx);
        while (0 <= pp_wait_completions(kv_ctx, 1));
        pp_close_ctx(kv_ctx);
        free_kv_pairs();
    }

    else {
      /// Client Part
//        run_tests_multiple_clients(servername);
//        run_tests_one_client(servername);

//        return 0;

        kv_open(servername, &kv_ctx);
        char *send_buffer = (char *) malloc(100);
        char *send_buffer2 = (char *) malloc(100);
        char *send_buffer3 = (char *) malloc(100);
        strcpy(send_buffer, "1");
        strcpy(send_buffer2, "2");
        strcpy(send_buffer3, "3");
        char *recv_buffer;
        char *recv_buffer2;
        char *recv_buffer3;
//        kv_set(kv_ctx, "1", "1");

        const char *key = "1";
        char *value = NULL;
        kv_get(kv_ctx, key, &value);
        printf("value returned for key of 1 is: %s\n", value);
//        return 0;
        kv_set(kv_ctx, "1", "1");
        kv_set(kv_ctx, "2", "100");
        kv_set(kv_ctx, "2", "2");
        kv_set(kv_ctx, "3", "3");
        kv_set(kv_ctx, "3", "5");
        kv_set(kv_ctx, "2", "5");
        kv_set(kv_ctx, "1", "111");
        kv_set(kv_ctx, "2", "19");

        kv_set(kv_ctx, "4", "54");
        kv_set(kv_ctx, "3", "80");
        kv_set(kv_ctx, "1", "14");
        kv_set(kv_ctx, "4", "54");
        kv_set(kv_ctx, "3", "54");
        kv_set(kv_ctx, "3", "24");
        kv_set(kv_ctx, "3", "54");



////        printf("AFTER SENT BUFFER IS: %s\n", ((struct packet*) ctx->buf)->eager_set_client.key);
        printf("im in client\n");
        kv_get(kv_ctx, "1", &recv_buffer);
        printf("value returned for key of 1 is: %s\n", recv_buffer);
        kv_get(kv_ctx, "2", &recv_buffer2);
        printf("value returned for key of 2 is: %s\n", recv_buffer2);
        kv_get(kv_ctx, "3", &recv_buffer3);
        printf("value returned for key of 3 is: %s\n", recv_buffer3);
        kv_get(kv_ctx, "4", &recv_buffer3);
        printf("value returned for key of 4 is: %s\n", recv_buffer3);


        memset(send_buffer, 'a', 100);
        kv_set(kv_ctx, "1", send_buffer);
        printf("done with set 1\n");
        kv_get(kv_ctx, "1", &recv_buffer);
        if (0 != strcmp(send_buffer, recv_buffer)) {
            printf("get failed\n");
            return 0;
        }
        printf("done with 1\n");


        kv_release(recv_buffer);


        /* Test logic */
        kv_set(kv_ctx, "test1", send_buffer);
        kv_get(kv_ctx, "test1", &recv_buffer);
        if (0 != strcmp(send_buffer, recv_buffer)) {
            printf("test1 failed\n");
            return 0;
        }
        printf("done with test1\n");

        kv_release(recv_buffer);
        memset(send_buffer, 'b', 100);
        kv_set(kv_ctx, "1", send_buffer);
        memset(send_buffer, 'c', 100);
        kv_set(kv_ctx, "22", send_buffer);
        memset(send_buffer, 'b', 100);
        kv_get(kv_ctx, "1", &recv_buffer);
        if (0 != strcmp(send_buffer, recv_buffer)) {
            printf("test2 failed\n");
            return 0;
        }
        printf("done with test2\n");

        kv_release(recv_buffer);


        /* Test large size */
        char *send_buffer11 = (char *) malloc(RENDEZVOUS_TEST_SIZE);
        char *recv_buffer11;

        memset(send_buffer11, 'a', RENDEZVOUS_TEST_SIZE - 1);
        kv_set(kv_ctx, "1", send_buffer11);
        kv_set(kv_ctx, "333", send_buffer11);
        kv_get(kv_ctx, "1", &recv_buffer11);
        if(0 != strcmp(send_buffer11, recv_buffer11)){
            printf("test3 failed\n");
            return 0;
        }
        printf("done with test3\n");

        kv_release(recv_buffer11);
    }
    return 0;
}