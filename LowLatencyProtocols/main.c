//
// Created by hsiny on 9/16/24.
//
#include <time.h>
# include "kv_api.h"
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
}

/**
 *
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char *argv[])
{
    /// If servername is provided then it's client, else server
    char *servername;
    if (optind == argc - 1)
        servername = strdup(argv[optind]);
    else if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    /// Create an empty pointer, kv_open will add stuff to it
    KVHandle *kv_handle;
    kv_open(servername, (void*) &kv_handle);

    /// decide communication protocol
    kv_handle->protocol = EAGER;

    /// for test
    int iters = NUMBER_MESSAGES;
    latency_test(servername, 0, kv_handle->ctx, iters);

    /// free everything
    kv_close(kv_handle);

    return 0;
}


