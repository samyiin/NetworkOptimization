#include "kv_api.h"


/**
 * Basically the main in the template, get rid off all the options
 * @param servername
 * @param kv_handle
 * @return
 */
int kv_open(char *servername, void **kv_handle){
    /// Initialize KV handle
    KVHandle *my_kv_handle = malloc(sizeof(KVHandle));
    my_kv_handle->ib_devname = NULL;
    my_kv_handle->port = 12345;
    my_kv_handle->ib_port = 1;
    my_kv_handle->mtu = pp_mtu_to_enum(MTU) ;
    my_kv_handle->rx_depth = RX_DEPTH;
    my_kv_handle->tx_depth = TX_DEPTH;
    my_kv_handle->use_event = 0;
    my_kv_handle->mr_control_size = sizeof(ControlMessage);
    my_kv_handle->sl = 0;
    my_kv_handle->gidx = -1;
    /// record the address where my_kv_handle points to
    *kv_handle = my_kv_handle;


    /// Set up the random generator
    srand48(getpid() * time(NULL));

    ////Initialize page size
    page_size = sysconf(_SC_PAGESIZE);

    /**
     * This part is trying to see if there is local infiniband devices
     * If yes then get the first device that has a device name
     * and then Initialize the device.
     */
    my_kv_handle->dev_list = ibv_get_device_list(NULL);
    if (!my_kv_handle->dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    if (!my_kv_handle->ib_devname) {
        my_kv_handle->ib_dev = *(my_kv_handle->dev_list);
        if (!my_kv_handle->ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        int i;
        for (i = 0; my_kv_handle->dev_list[i]; ++i)
            if (!strcmp(ibv_get_device_name(my_kv_handle->dev_list[i]), my_kv_handle->ib_devname))
                break;
        my_kv_handle->ib_dev = my_kv_handle->dev_list[i];
        if (!my_kv_handle->ib_dev) {
            fprintf(stderr, "IB device %s not found\n", my_kv_handle->ib_devname);
            return 1;
        }
    }

    /// initialization: create the ctx
    my_kv_handle->ctx = pp_init_ctx(my_kv_handle->ib_dev,
                                    my_kv_handle->mr_control_size,
                                    my_kv_handle->rx_depth,
                                    my_kv_handle->tx_depth,
                                    my_kv_handle->ib_port,
                                    my_kv_handle->use_event,
                                    !servername);
    if (!my_kv_handle->ctx)
        return 1;

    /**
     * "Fill up" the receive queue with receive work requests
     * If use_event means we will use channels to notify CQ when a task is done
     */
    my_kv_handle->ctx->routs = pp_post_recv(my_kv_handle->ctx,
                                            my_kv_handle->ctx->rx_depth);
    if (my_kv_handle->ctx->routs < my_kv_handle->ctx->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", my_kv_handle->ctx->routs);
        return 1;
    }

    if (my_kv_handle->use_event)
        if (ibv_req_notify_cq(my_kv_handle->ctx->cq, 0)) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            return 1;
        }

    /**
     * Get port info for the local infiniband device
     * Make sure the local device is a infiniband device and it has a lid
     * Get the gid of local device, if gidx > 0, else set my gid to 0.
     * store the lid, gid, pqn, psn to a pingpong_dest struct called my_dest
     * my_dest is the info for my node, remote_dest is for the node I am
     * connecting to.
     */
    if (pp_get_port_info(my_kv_handle->ctx->context,
                         my_kv_handle->ib_port,
                         &(my_kv_handle->ctx->portinfo))) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    my_kv_handle->my_dest.lid = my_kv_handle->ctx->portinfo.lid;
    if (my_kv_handle->ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_kv_handle->my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }

    if (my_kv_handle->gidx >= 0) {
        if (ibv_query_gid(my_kv_handle->ctx->context, my_kv_handle->ib_port,
                          my_kv_handle->gidx, &(my_kv_handle->my_dest.gid))) {
            fprintf(stderr, "Could not get local gid for gid index %d\n", my_kv_handle->gidx);
            return 1;
        }
    } else{
        memset(&(my_kv_handle->my_dest.gid), 0, sizeof my_kv_handle->my_dest.gid);
    }

    my_kv_handle->my_dest.qpn = my_kv_handle->ctx->qp->qp_num;
    my_kv_handle->my_dest.psn = lrand48() & 0xffffff;
    inet_ntop(AF_INET6, &(my_kv_handle->my_dest.gid), my_kv_handle->gid, sizeof my_kv_handle->gid);

    /*
     * If servername is provided, then we are running main as a client, so
     * we will exchange info with remote host using pp_client_exch_dest,
     * else we are the server, so we will exchange with pp_server_exch_dest
     * todo: pp_server_exch_dest will block the process until the client
     * connects to it.
     */
    if (servername){
        my_kv_handle->rem_dest = pp_client_exch_dest(servername,
                                                     my_kv_handle->port,
                                                     &(my_kv_handle->my_dest));
    }

    else{
        my_kv_handle->rem_dest = pp_server_exch_dest(my_kv_handle->ctx,
                                                     my_kv_handle->ib_port,
                                                     my_kv_handle->mtu,
                                                     my_kv_handle->port,
                                                     my_kv_handle->sl,
                                                     &(my_kv_handle->my_dest),
                                                     my_kv_handle->gidx);
    }


    if (!my_kv_handle->rem_dest){
        return 1;
    }

    inet_ntop(AF_INET6, &my_kv_handle->rem_dest->gid, my_kv_handle->gid, sizeof my_kv_handle->gid);

    /// todo this will be deleted later
    printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           my_kv_handle->rem_dest->lid, my_kv_handle->rem_dest->qpn, my_kv_handle->rem_dest->psn, my_kv_handle->gid);

    /**
     * If we are client, we will try to establish connection with the remote
     * If we are server, we already establish connection with remote during
     * the pp_server_exch_dest above. But this will happen only after client
     * make connection with the server, because the server will be blocked at
     * pp_server_exch_dest until the client connect to it.
     */
    if (servername){
        if (pp_connect_ctx(my_kv_handle->ctx,
                           my_kv_handle->ib_port,
                           my_kv_handle->my_dest.psn,
                           my_kv_handle->mtu,
                           my_kv_handle->sl,
                           my_kv_handle->rem_dest,
                           my_kv_handle->gidx)){
            return 1;
        }
    }
}


/**
 * This function sets the value of a key on server's side
 * This function is called by client, not server.
 *
 * We define that it's the server's responsibility to decode the message.
 * @param kv_handle
 * @param key
 * @param value
 * @return
 */
int kv_set(void *kv_handle, const char *key, const char *value){
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    if (ptr_kv_handle->protocol == EAGER){
        /// send a message to the server containing key and value
        /// concatenate the mr_control_start_pointer as a kv_addr_pair pointer
        ControlMessage *key_value_pair = (ControlMessage *)
                ptr_kv_handle->ctx->mr_control_start_ptr;
        /// We will just mem_copy to a registered memory region: mr_control
        key_value_pair->protocol = EAGER;
        strcpy(key_value_pair->buf, key);
        strcpy(key_value_pair->buf + strlen(key) + 1, value);
        /// send the message
        pp_post_send(ptr_kv_handle->ctx);
        /// Wait for this send to finish and complete response from server
        pp_wait_completions(ptr_kv_handle->ctx, 2);
        // todo: write the server part of this!!! decode + an array of
        //  pointers of key value pair. 
    }else{
//        /// It's more expensive to perform mem_copy than register memory.
//        /// So we will register the address of the value as a memory region.
//        ctx->mr_rdma_write_start_ptr = (void *) ((char *) ctx->buf +
//                                                 ctx->mr_control_size);
//        ctx->mr_rdma_write = ibv_reg_mr(ctx->pd, ctx->mr_rdma_write_start_ptr,
//                                        ctx->mr_rdma_write_size,
//                                        IBV_ACCESS_LOCAL_WRITE |
//                                        IBV_ACCESS_REMOTE_WRITE);
//    // The remote memory region and remote key
//    my_dest.buf_va = bswap_64((uintptr_t)ctx->mr_rdma_write_start_ptr);
//    my_dest.buf_rkey = htonl(ctx->mr_rdma_write->rkey);

        return 1;
    }
    return 0;
}


/**
 * Free everything
 * @param kv_handle
 * @return
 */
int kv_close(void *kv_handle){
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    pp_close_ctx(ptr_kv_handle->ctx);
    ibv_free_device_list(ptr_kv_handle->dev_list);
    free(ptr_kv_handle->rem_dest);
}