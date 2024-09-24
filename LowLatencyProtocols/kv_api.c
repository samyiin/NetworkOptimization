#include "kv_api.h"
# include "dynamic_array.h"


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
    // fill up the receive queue
    if (pp_post_recv(my_kv_handle->ctx) == 0){
        my_kv_handle->ctx->routs =my_kv_handle->ctx->rx_depth;
    }else {
        fprintf(stderr, "kv_open Couldn't post receive (%d)\n",
                my_kv_handle->ctx->routs);
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
    if (strlen(key) + strlen(value) + 2 <= CONTROL_MESSAGE_BUFFER_SIZE){
        /// send a message to the server containing key and value
        /// concatenate the mr_control_start_pointer as a kv_addr_pair pointer
        ControlMessage *key_value_pair = (ControlMessage *)
                ptr_kv_handle->ctx->mr_send_start_ptr;
        /// We will just mem_copy to a registered memory region: mr_control
        key_value_pair->operation = EAGER_KV_SET;
        strcpy(key_value_pair->buf, key);
        strcpy(key_value_pair->buf + strlen(key) + 1, value);
        /// send the message
        pp_post_send(ptr_kv_handle->ctx);
        /// Wait for this send to finish
        struct ibv_wc *next_complete_wc = pp_wait_next_complete(ptr_kv_handle->ctx);
        if (next_complete_wc->opcode != IBV_WC_SEND){
            // do something and skip the round
            fprintf(stderr, "kv_set: EAGER something went wrong\n");
            return 1;
        }
        /// (probably need one confirm from server)
        printf("Client: kv_set complete!\n");
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
 * This function get the value from the server side.
 *
 * 1. client send key
 * 2. server send either EAGER response or RENDEZVOUS response
 *
 * @param kv_handle
 * @param key
 * @param var
 * @return
 */
int kv_get(void *kv_handle, const char *key, char **var){
    /// First send a message containing the key
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    ControlMessage *key_value_pair = (ControlMessage *)
            ptr_kv_handle->ctx->mr_send_start_ptr;
    /// We will just mem_copy to a registered memory region: mr_control
    key_value_pair->operation = CLIENT_KV_GET;
    strcpy(key_value_pair->buf, key);
    /// send the message
    pp_post_send(ptr_kv_handle->ctx);
    /// Wait for send wc and server response
    // todo: this will need to be optimized, new wait for complete
    struct ibv_wc *new_2_complete[2];
    new_2_complete[0] = pp_wait_next_complete(ptr_kv_handle->ctx);
    new_2_complete[1] = pp_wait_next_complete(ptr_kv_handle->ctx);
    // Check the type of the two wc and get the response wc
    int contains_send_wc = 0, contains_receive_wc=0;
    struct ibv_wc *response_wc;
    for (int i = 0; i < 2; i++){
        if (new_2_complete[i]->opcode == IBV_WC_SEND){
            contains_send_wc = 1;
        }
        if (new_2_complete[i]->opcode == IBV_WC_RECV){
            contains_receive_wc = 1;
            response_wc = new_2_complete[i];
        }
    }
    if (contains_send_wc + contains_receive_wc != 2){
        // do something and skip the round
        fprintf(stderr, "kv_get: CLIENT_KV_GET something went wrong\n");
        return 1;
    }
    /// Now we get the information from response_wc
    int mr_id = (int) response_wc->wr_id;
    // make it a pointer because we need to change it later
    struct MRInfo *ptr_mr_receive = ptr_kv_handle->ctx->array_mr_receive_info +
                                    mr_id;
    // cast the memory region in form of struct Control message
    ControlMessage *ptr_control_message = (ControlMessage *)
            ptr_mr_receive->mr_start_ptr;

    /// Handle the response
    if (ptr_control_message->operation == SERVER_KV_GET_EAGER){
        // According to eager protocol, the value should be inside the buffer.
        char *buffer_value_address = ptr_control_message->buf;
        size_t value_length = strlen(buffer_value_address) + 1;
        char *local_value_address = malloc(value_length);
        // mem copy to where the value is
        strcpy(local_value_address, buffer_value_address);
        // assign this address to var
        *var = local_value_address;
        return 0;
    } else if (ptr_control_message->operation == SERVER_KV_GET_RENDEZVOUS){

    }else{
        // do something and skip the round
        fprintf(stderr, "kv_get: SERVER_KV_GET something went wrong\n");
        return 1;
    }


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

int _handle_eager_kv_set(KeyValueAddressArray *database, ControlMessage
*ptr_control_message){
    // According to eager protocol: the buffer will contain key and value
    char *control_message_buf = ptr_control_message->buf;
    // The key will be from the start address till first null terminator
    char *buf_key_address = control_message_buf;
    int key_length = strlen(buf_key_address);
    char *local_key_address = malloc(key_length + 1);
    strcpy(local_key_address, buf_key_address);
    // The value will be following right after key
    char *buf_value_address = control_message_buf + key_length + 1;
    int value_len = strlen(buf_value_address);
    char *local_value_address = malloc(value_len + 1);
    strcpy(local_value_address, buf_value_address);
    // insert the result to local database
    KeyValueAddressPair entry = {local_key_address, local_value_address,
                                 value_len + 1};
    insert_array(database, &entry);
    return 0;
}

int _handle_client_kv_get(KeyValueAddressArray *database, ControlMessage
*ptr_control_message, KVHandle *ptr_kv_handle){
    // According to eager protocol: the buffer will contain key and value
    char *control_message_buf = ptr_control_message->buf;
    // The key will be from the start address till first null terminator
    char *buf_key_address = control_message_buf;
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                           buf_key_address);
    // todo: if didn't exist in database: send another opcode
    if (get_kv_pair->value_size < CONTROL_MESSAGE_BUFFER_SIZE){
        /// We will send the value directly
        ControlMessage *key_value_pair = (ControlMessage *)
                ptr_kv_handle->ctx->mr_send_start_ptr;
        /// We will just mem_copy to a registered memory region: mr_control
        key_value_pair->operation = SERVER_KV_GET_EAGER;
        strcpy(key_value_pair->buf, get_kv_pair->value_address);
        /// send the message
        pp_post_send(ptr_kv_handle->ctx);
        // todo: wait for complete regulate how many are send and how many
        //  are receive, get all the receive?
//        struct ibv_wc *send_completed = pp_wait_next_complete
//                (ptr_kv_handle->ctx);
//        if (next_complete_wc->opcode != IBV_WC_RECV){
//            // do something and skip the round
//            continue;
//        }
        return 0;
    }
}



/**
 * Server will just run this function, and respond to the clients requests
 * todo: handle the free resource here!!!
 * @return
 */
int run_server(KVHandle *kv_handle){
    /// Initialize database: basically array of (key_addr, value_addr)
    KeyValueAddressArray *database = initialize_KeyValueAddressArray(20);

    while (1){
        // todo: this wait complete need to be modified!!! Three place in total
        struct ibv_wc *next_complete_wc = pp_wait_next_complete(kv_handle->ctx);
        if (next_complete_wc->opcode != IBV_WC_RECV){
            // do something and skip the round
            continue;
        }
        /// get the memory region
        int mr_id = (int) next_complete_wc->wr_id;
        // make it a pointer because we need to change it later
        struct MRInfo *ptr_mr_receive = kv_handle->ctx->array_mr_receive_info +
                                        mr_id;
        // cast the memory region in form of struct Control message
        ControlMessage *ptr_control_message = (ControlMessage *)
                ptr_mr_receive->mr_start_ptr;

        /// handle it
        // if we get a message from any client to tell us finish experiment
        if (ptr_control_message->operation == SHUT_DOWN_SERVER){
            break;
        }
        // eager set kv
        if (ptr_control_message->operation == EAGER_KV_SET){
            _handle_eager_kv_set(database, ptr_control_message);
        }

        /// Finish handling this receive, this region can be post receive again
        ptr_mr_receive->mr_status = FREE;

        /// free this work complete
        free(next_complete_wc);

        /// for testing: see how many value are set
        print_dynamic_array(database);
    }

    /// free the database and every allocated memory inside
    free_array(database);
}
