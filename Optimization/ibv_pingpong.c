#include "ibv_api.h"
# include <unistd.h>

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
                              int iters){
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
        // strat timer
        struct timeval start_time, end_time;
        long time_elapse;
        gettimeofday(&start_time, NULL);

        // start the trial
        int i;
        int num_complete = 0;
        for (i = 0; i < iters; i++) {
            // wait for the current tx_depth send_wr to complete before we
            // continue.
            if ((i != 0) && (i % tx_depth == 0)) {
                pp_wait_completions(ctx, tx_depth);
                num_complete += tx_depth;
            }
            if (pp_post_send(ctx)) {
                fprintf(stderr, "Client couldn't post send\n");
                return 1;
            }
        }
        // wait for the last few send_wr to complete
        // + waiting for 1 response from server
        pp_wait_completions(ctx, iters - num_complete + 1);

        // calculate time elapsed
        gettimeofday(&end_time, NULL);
        // calculate time: in seconds
        double const total_second = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec)/1000000.0;

        // calculate throughput: afraid of overflow
        double const total_sent_Mb = iters * (ctx->size * 8 / 1048576.0);
        double const throughput = total_sent_Mb / total_second;

        // print the throughput: In MegaBytes per second
        if (!warmup){
            printf("%d\t\t%.2f\t\tMbits/sec\n", ctx->size, throughput);
        }

    } else {
        // wait for client to send
        pp_wait_completions(ctx, iters);

        // send a response to client
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
                   const int *list_message_sizes, int len_list_message_sizes,
                   int iters){
    for (int i = 0; i < len_list_message_sizes; i++){
        // set the message size for this trial
        ctx->size = list_message_sizes[i];

        // perform the trial
        perform_single_experiment(servername,warmup, ctx, tx_depth, iters);

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
                 struct pingpong_context *ctx, int message_size,
                         int iters){
    ctx->size = message_size;

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
void usage(const char *argv0){
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
    enum ibv_mtu             mtu = IBV_MTU_1024;
    int                      rx_depth = 100;    // The length of receive queue
    int                      tx_depth = 100;    // The length of send queue
    int                      iters = 5000;      // number of message to send
    int                      use_event = 0;
    // poll CQ or not
    int                      size = 4096;          // default message length
    int                      buffer_size = 1048576;   // buffer size
    int                      sl = 0;            // service level
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

    ctx = pp_init_ctx(ib_dev, size, buffer_size, rx_depth, tx_depth, ib_port, use_event,
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
//    printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
//           my_dest.lid, my_dest.qpn, my_dest.psn, gid);

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
//    printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
//           rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

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
     * perform the ping pong test
     */
    int warmup = 0;
    int list_message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
                                2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144,
                                524288, 1048576};
    int len_list_message_sizes = 21;
    throughput_test(servername, warmup,
                  ctx, tx_depth,
                  list_message_sizes, len_list_message_sizes, iters);

    /*
     * Latency test
     */
    int message_size = mtu;
    latency_test(servername, warmup, ctx, message_size, iters);

    /*
     * Free everything
     */
    ibv_free_device_list(dev_list);
    free(rem_dest);
    return 0;
}


