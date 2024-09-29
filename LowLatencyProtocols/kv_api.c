#include "kv_api.h"


/**
 * Basically the main in the template, get rid off all the options
 * @param servername
 * @param kv_handle
 * @return
 */
int kv_open(char *servername, void **kv_handle) {
    /// Initialize KV handle
    KVHandle *ptr_kv_handle = malloc(sizeof(KVHandle));  // freed in kv_close
    ptr_kv_handle->ib_devname = NULL;
    ptr_kv_handle->port = 12345;
    ptr_kv_handle->ib_port = 1;
    ptr_kv_handle->mtu = pp_mtu_to_enum(MTU);
    ptr_kv_handle->rx_depth = RX_DEPTH;
    ptr_kv_handle->tx_depth = TX_DEPTH;
    ptr_kv_handle->use_event = 0;
    ptr_kv_handle->mr_control_size = sizeof(ControlMessage);
    ptr_kv_handle->sl = 0;
    ptr_kv_handle->gidx = -1;
    /// record the address where ptr_kv_handle points to
    *kv_handle = ptr_kv_handle;


    /// Set up the random generator
    srand48(getpid() * time(NULL));

    ////Initialize page size
    page_size = sysconf(_SC_PAGESIZE);

    /**
     * This part is trying to see if there is local infiniband devices
     * If yes then get the first device that has a device name
     * and then Initialize the device.
     */
    ptr_kv_handle->dev_list = ibv_get_device_list(NULL);
    if (!ptr_kv_handle->dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    if (!ptr_kv_handle->ib_devname) {
        ptr_kv_handle->ib_dev = *(ptr_kv_handle->dev_list);
        if (!ptr_kv_handle->ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        int i;
        for (i = 0; ptr_kv_handle->dev_list[i]; ++i)
            if (!strcmp(ibv_get_device_name(ptr_kv_handle->dev_list[i]),
                        ptr_kv_handle->ib_devname))
                break;
        ptr_kv_handle->ib_dev = ptr_kv_handle->dev_list[i];
        if (!ptr_kv_handle->ib_dev) {
            fprintf(stderr, "IB device %s not found\n",
                    ptr_kv_handle->ib_devname);
            return 1;
        }
    }

    int num_remote_host;
    if (servername){
        num_remote_host = 1;
    } else{
        num_remote_host = NUM_CLIENTS;
    }
    ptr_kv_handle->num_remote_host = num_remote_host;

    // todo: free this two + free remote dest in array_remote_dest
    ptr_kv_handle->array_ctx_ptr = malloc(sizeof(struct pingpong_context*) *
            num_remote_host);
    ptr_kv_handle->array_remote_dest = malloc(sizeof(struct pingpong_dest*)
            * num_remote_host);
    int array_client_socket[num_remote_host];


    /**
     * If we are server, first wait for all the clients to setup, and send
     * us their dest info, then we will start setting up and send back our
     * info.
     */
    if (!servername){
        // first connect to all the clients with web sockets
        printf("Waiting for %d clients to connect... \n", num_remote_host);
        if (listen_to_websocket(ptr_kv_handle->port, array_client_socket,
                                num_remote_host) != 0){
            fprintf(stderr, "Could not establish socket connection\n");
            return 1;
        }
        printf("All %d clients to connected! \n", num_remote_host);
    }




    /// initialization: create the ctx for each client
    for (int remote_host_id = 0; remote_host_id < num_remote_host; remote_host_id ++){
        ptr_kv_handle->array_ctx_ptr[remote_host_id] = pp_init_ctx(ptr_kv_handle->ib_dev,
                                                    ptr_kv_handle->mr_control_size,
                                                    ptr_kv_handle->rx_depth,
                                                    ptr_kv_handle->tx_depth,
                                                    ptr_kv_handle->ib_port,
                                                    ptr_kv_handle->use_event,
                                                    !servername);
        if (!ptr_kv_handle->array_ctx_ptr[remote_host_id]){
            fprintf(stderr, "kv_open: pp_init_ctx failed\n");
            return 1;
        }

        ptr_kv_handle->ctx = ptr_kv_handle->array_ctx_ptr[remote_host_id];

        /*
         * "Fill up" the receive queue with receive work requests
         * If use_event means we will use channels to notify CQ when a task is done
         * This feature is not used in our exercise
         */
        if (pp_post_recv(ptr_kv_handle->ctx) == 0) {
            ptr_kv_handle->ctx->routs = ptr_kv_handle->ctx->rx_depth;
        } else {
            fprintf(stderr, "kv_open Couldn't post receive (%d)\n",
                    ptr_kv_handle->ctx->routs);
            return 1;
        }

        if (ptr_kv_handle->use_event) {
            if (ibv_req_notify_cq(ptr_kv_handle->ctx->send_cq, 0)) {
                fprintf(stderr, "Couldn't request send_cq notification\n");
                return 1;
            }
            if (ibv_req_notify_cq(ptr_kv_handle->ctx->receive_cq, 0)) {
                fprintf(stderr, "Couldn't request receive_cq notification\n");
                return 1;
            }
        }
        /**
         * Get port info for the local infiniband device
         * Make sure the local device is a infiniband device and it has a lid
         * Get the gid of local device, if gidx > 0, else set my gid to 0.
         * store the lid, gid, pqn, psn to a pingpong_dest struct called my_dest
         * my_dest is the info for my node, remote_dest is for the node I am
         * connecting to.
         */
        if (pp_get_port_info(ptr_kv_handle->ctx->context,
                             ptr_kv_handle->ib_port,
                             &(ptr_kv_handle->ctx->portinfo))) {
            fprintf(stderr, "Couldn't get port info\n");
            return 1;
        }

        ptr_kv_handle->my_dest.lid = ptr_kv_handle->ctx->portinfo.lid;
        if (ptr_kv_handle->ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND &&
            !ptr_kv_handle->my_dest.lid) {
            fprintf(stderr, "Couldn't get local LID\n");
            return 1;
        }

        if (ptr_kv_handle->gidx >= 0) {
            if (ibv_query_gid(ptr_kv_handle->ctx->context, ptr_kv_handle->ib_port,
                              ptr_kv_handle->gidx, &(ptr_kv_handle->my_dest.gid))) {
                fprintf(stderr, "Could not get local gid for gid index %d\n",
                        ptr_kv_handle->gidx);
                return 1;
            }
        } else {
            memset(&(ptr_kv_handle->my_dest.gid), 0,
                   sizeof ptr_kv_handle->my_dest.gid);
        }

        ptr_kv_handle->my_dest.qpn = ptr_kv_handle->ctx->qp->qp_num;
        ptr_kv_handle->my_dest.psn = lrand48() & 0xffffff;
        inet_ntop(AF_INET6, &(ptr_kv_handle->my_dest.gid), ptr_kv_handle->gid,
                  sizeof ptr_kv_handle->gid);

        /**
         * If servername is provided, then we are running main as a client, so
         * we will exchange info with remote host using pp_client_exch_dest,
         * else we are the server, so we will exchange with pp_server_exch_dest
         * connects to it.
         */
        if (servername) {
            ptr_kv_handle->rem_dest = pp_client_exch_dest(servername,
                                                          ptr_kv_handle->port,
                                                          &(ptr_kv_handle->my_dest));
        } else {
            int connfd = array_client_socket[remote_host_id];
            ptr_kv_handle->array_remote_dest [remote_host_id] = pp_server_exch_dest(ptr_kv_handle->ctx,
                                                                    ptr_kv_handle->ib_port,
                                                                    ptr_kv_handle->mtu,
                                                                    connfd,
                                                                    ptr_kv_handle->sl,
                                                                    &(ptr_kv_handle->my_dest),
                                                                    ptr_kv_handle->gidx);
            ptr_kv_handle->rem_dest = ptr_kv_handle->array_remote_dest [remote_host_id];
            if (!ptr_kv_handle->rem_dest) {
                fprintf(stderr, "kv_open: didn't get rem_dest\n");
                return 1;
            }
        }
        /**
         * If we are client, we will try to establish connection with the remote
         * If we are server, we already pp_connect_ctx with remote during
         * the pp_server_exch_dest above. But this will happen only after client
         * make connection with the server, because the server will be blocked at
         * pp_server_exch_dest until the client connect to it.
         */
        inet_ntop(AF_INET6, &ptr_kv_handle->rem_dest->gid, ptr_kv_handle->gid,
                  sizeof ptr_kv_handle->gid);
        if (servername) {
            if (pp_connect_ctx(ptr_kv_handle->ctx,
                               ptr_kv_handle->ib_port,
                               ptr_kv_handle->my_dest.psn,
                               ptr_kv_handle->mtu,
                               ptr_kv_handle->sl,
                               ptr_kv_handle->rem_dest,
                               ptr_kv_handle->gidx)) {
                return 1;
            }
        }
    }

    return 0;
}

/**
 * Store the start pointer of remote control message into
 * remote_control_message
 * @param ptr_kv_handle
 * @param remote_control_message
 * @param blocking: 0: not blocking. 1 blocking
 * @return  1: didn't poll. 0: polled success
 */
int get_remote_control_message(KVHandle *ptr_kv_handle, ControlMessage
**remote_control_message, int blocking){
    if (poll_next_receive_wc(ptr_kv_handle->ctx, blocking)) {
        return 1;
    }
    // check which receive buffer got the response
    *remote_control_message = (ControlMessage *)
            ptr_kv_handle->ctx->mr_control_receive_start_ptr;
    return 0;
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
int kv_set(void *kv_handle, const char *key, const char *value) {
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    // declare the temp_mr_rdma just in case
    struct MRInfo *temp_mr_rdma = NULL;
    size_t value_size;

    if (strlen(key) + strlen(value) + 2 <= CONTROL_MESSAGE_BUFFER_SIZE) {
        /// send a message to the server: key + value_size + value
        value_size = strlen(value) + 1;
        const void *array_messages_address[3] = {key, &value_size, value};
        set_control_message(ptr_kv_handle, CLIENT_KV_SET_EAGER,
                            array_messages_address);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set EAGER: cannot send message\n");
            return 1;
        }
        /// blocking, see if there is a control message
        ControlMessage *remote_control_message;
        int polled_message = get_remote_control_message(ptr_kv_handle,
                                                        &remote_control_message,
                                                        1);
        if (polled_message != 0){
            /// there is something wrong so we didn't get message after blocking
            return 1;
        }
        if  (remote_control_message->operation == SERVER_IN_PROGRESS){
            printf("Failed when setting %s \n", key);
            return 1;
        }else if (remote_control_message->operation ==SERVER_KV_SET_SUCCESSFUL){
            printf("Success when setting %s \n", key);
            return 0;
        }
    } else {// Rendezvous
        /// send a control message to the server: key + value size (size_t)
        value_size = strlen(value) + 1;
        const void *array_messages_address[2] = {key, &value_size};
        set_control_message(ptr_kv_handle, CLIENT_KV_SET_RENDEZVOUS,
                            array_messages_address);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set REND_KEY: cannot send message\n");
            return 1;
        }

        /// temporarily register the address of the value as a memory region.
        temp_mr_rdma = malloc(sizeof(struct MRInfo));
        temp_mr_rdma->mr_start_ptr = (void *) value;
        temp_mr_rdma->mr_size = value_size;
        temp_mr_rdma->mr = ibv_reg_mr(ptr_kv_handle->ctx->pd,
                                      temp_mr_rdma->mr_start_ptr,
                                      temp_mr_rdma->mr_size,
                                 IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_WRITE);

        /// blocking, see if there is a control message
        ControlMessage *remote_control_message;
        int polled_message = get_remote_control_message(ptr_kv_handle,
                                                        &remote_control_message,
                                                        1);
        if (polled_message != 0){
            /// there is something wrong so we didn't get message after blocking
            return 1;
        }
        if  (remote_control_message->operation == SERVER_IN_PROGRESS){
            if (ibv_dereg_mr(temp_mr_rdma->mr)) {
                fprintf(stderr, "Couldn't deregister MR_control_send\n");
                return 1;
            }
            free(temp_mr_rdma);
            printf("Failed when setting %s \n", key);
            return 1;
        }

        /// decode remote control message
        // should be key + remote va and remote key
        void *array_ptr_to_fill[2];
        decode_control_message_buffer(SERVER_KV_SET_RENDEZVOUS,
                                      remote_control_message->buf,
                                      array_ptr_to_fill);
        ptr_kv_handle->ctx->mr_rdma = temp_mr_rdma->mr;
        ptr_kv_handle->ctx->mr_rdma_size = temp_mr_rdma->mr_size;
        ptr_kv_handle->ctx->mr_rdma_start_ptr = temp_mr_rdma->mr_start_ptr;
        ptr_kv_handle->ctx->remote_buf_va = *(uint64_t*)array_ptr_to_fill[0];
        ptr_kv_handle->ctx->remote_buf_rkey = *(uint32_t *)array_ptr_to_fill[1];

        /// RDMA WRITE to server
        pp_post_rdma(ptr_kv_handle->ctx, IBV_WR_RDMA_WRITE);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set client RDMA_WRITE: cannot send message\n");
            return 1;
        }

        /// Write FIN to server
        // should specify which key
        const void *array_messages_address_1[1] = {key};
        set_control_message(ptr_kv_handle, CLIENT_KV_SET_RENDEZVOUS_FIN,
                            array_messages_address_1);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set KV_SET_FIN: cannot send message\n");
            return 1;
        }

        /// Finally deregister the mr
        if (ibv_dereg_mr(temp_mr_rdma->mr)) {
            fprintf(stderr, "Couldn't deregister MR_control_send\n");
            return 1;
        }
        free(temp_mr_rdma);
        ptr_kv_handle->ctx->mr_rdma = NULL;
        ptr_kv_handle->ctx->mr_rdma_start_ptr = NULL;
        ptr_kv_handle->ctx->mr_rdma_size = 0;
        ptr_kv_handle->ctx->remote_buf_rkey = 0;
        ptr_kv_handle->ctx->remote_buf_va = 0;
        return 0;
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
int kv_get(void *kv_handle, const char *key, char **var) {
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    /// First send a message containing the key
    const void *array_messages_address[1] = {key};
    set_control_message(ptr_kv_handle, CLIENT_KV_GET, array_messages_address);
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "kv_get CLIENT_KV_GET: cannot send message\n");
        return 1;
    }

    /// get one response from server: blocking until receive
    ControlMessage *remote_control_message;
    int polled_message = get_remote_control_message(ptr_kv_handle,
                                                    &remote_control_message,
                                                    1);
    if (polled_message != 0){
        /// there is something wrong so we didn't get message after blocking
        return 1;
    }

    if (remote_control_message->operation == SERVER_KV_GET_EAGER) {
        /// decode the response: result should contain just the value
        void *array_ptr_to_fill[1];
        decode_control_message_buffer(SERVER_KV_GET_EAGER,
                                      remote_control_message->buf,
                                      array_ptr_to_fill);
        char *buf_value_address = array_ptr_to_fill[0];
        // store to local buffer
        *var = malloc(strlen(buf_value_address) + 1);   // freed in kv_release
        strcpy(*var, buf_value_address);
        return 0;
    } else if (remote_control_message->operation == SERVER_KV_GET_RENDEZVOUS) {
        /// decode remote control message
        // should be value size + remote va and remote key
        void *array_ptr_to_fill[3];
        decode_control_message_buffer(SERVER_KV_GET_RENDEZVOUS,
                                      remote_control_message->buf,
                                      array_ptr_to_fill);
        ptr_kv_handle->ctx->mr_rdma_size = *(size_t*) array_ptr_to_fill[0];
        ptr_kv_handle->ctx->remote_buf_va = *(uint64_t*)array_ptr_to_fill[1];
        ptr_kv_handle->ctx->remote_buf_rkey = *(uint32_t *)
                array_ptr_to_fill[2];

        /// temporarily register the address of the value as a memory region.
        *var = malloc(ptr_kv_handle->ctx->mr_rdma_size);// freed in kv_release
        ptr_kv_handle->ctx->mr_rdma_start_ptr = (void *) *var;
        ptr_kv_handle->ctx->mr_rdma = ibv_reg_mr(ptr_kv_handle->ctx->pd,
                                                 ptr_kv_handle->ctx->mr_rdma_start_ptr,
                                                 ptr_kv_handle->ctx->mr_rdma_size,
                                                 IBV_ACCESS_LOCAL_WRITE);

        /// RDMA READ to server
        pp_post_rdma(ptr_kv_handle->ctx, IBV_WR_RDMA_READ);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_get client RDMA_READ: cannot send message\n");
            return 1;
        }

        /// Write FIN to server
        // should specify which key
        const void *array_messages_address_1[1] = {key};
        set_control_message(ptr_kv_handle, CLIENT_KV_GET_RENDEZVOUS_FIN,
                            array_messages_address_1);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set KV_SET_FIN: cannot send message\n");
            return 1;
        }

        /// Finally deregister the mr
        if (ibv_dereg_mr(ptr_kv_handle->ctx->mr_rdma)) {
            fprintf(stderr, "Couldn't deregister MR_control_send\n");
            return 1;
        }

        ptr_kv_handle->ctx->mr_rdma_start_ptr = NULL;
        ptr_kv_handle->ctx->mr_rdma_size = 0;
    }else if (remote_control_message->operation == SERVER_IN_PROGRESS){
        /// probably SERVER_IN_PROGRESS, but we don't need to read it.
        *var = NULL;
        fprintf(stderr, "This resource is busy \n");
        return 1;
    }
    else{
        /// probably Key doesn't exist
        *var = NULL;
        fprintf(stderr, "Key doesn't exist! \n");
        return 1;
    }

}


/**
 * Free everything
 * @param kv_handle
 * @return
 */
int kv_close(void *kv_handle) {
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    for (int i = 0; i < ptr_kv_handle->num_remote_host; i++){
        ptr_kv_handle->ctx = ptr_kv_handle->array_ctx_ptr[i];
        pp_close_ctx(ptr_kv_handle->ctx);
        ptr_kv_handle->rem_dest = ptr_kv_handle->array_remote_dest[i];
        free(ptr_kv_handle->rem_dest);
    }
    free(ptr_kv_handle->array_ctx_ptr);
    free(ptr_kv_handle->array_remote_dest);
    pp_close_ctx(ptr_kv_handle->ctx);
    ibv_free_device_list(ptr_kv_handle->dev_list);
    return 0;
}

/**
 * Free the value after kv_get() because in kv_get we need to allocate a
 * space for receiving value.
 * @param value
 */
void kv_release(char *value) {
    free(value);
}


///////////////////////////////////////////////////////////////////////////////
////////////////////// Run server code ////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/**
 * Here we must trust the client: the key exists in the database, the client
 * was processing the key (RDMA)
 * @param database
 * @param remote_control_message
 * @return
 */
int handle_CLIENT_KV_SET_RENDEZVOUS_FIN(KeyValueAddressArray *database,
                                        ControlMessage *remote_control_message){
    /// decode the messages in remote control buffer
    // It should contain key + value size
    void *array_ptr_to_fill[1];
    decode_control_message_buffer(CLIENT_KV_SET_RENDEZVOUS_FIN,
                                  remote_control_message->buf,
                                  array_ptr_to_fill);
    char *buf_key_address = array_ptr_to_fill[0];

    // deregister it's rdma memory region
    deregister_rdma_mr(database, buf_key_address);
    return 0;
}


int handle_CLIENT_KV_GET_RENDEZVOUS_FIN(KeyValueAddressArray *database,
                                        ControlMessage *remote_control_message){
    /// decode the messages in remote control buffer
    // It should contain key + value size
    void *array_ptr_to_fill[1];
    decode_control_message_buffer(CLIENT_KV_GET_RENDEZVOUS_FIN,
                                  remote_control_message->buf,
                                  array_ptr_to_fill);
    char *buf_key_address = array_ptr_to_fill[0];

    // deregister it's rdma memory region
    deregister_rdma_mr(database, buf_key_address);
    return 0;
}

int handle_RENDEZVOUS_KV_SET_KEY(KeyValueAddressArray *database,
                                 ControlMessage *remote_control_message,
                                 KVHandle *ptr_kv_handle) {


    /// decode the messages in remote control buffer
    // It should contain key + value size
    void *array_ptr_to_fill[2];
    decode_control_message_buffer(CLIENT_KV_SET_RENDEZVOUS,
                                  remote_control_message->buf,
                                  array_ptr_to_fill);
    char *buf_key_address = array_ptr_to_fill[0];
    size_t *buf_value_length_address = array_ptr_to_fill[1];

    /// handle special cases: check if the resource is busy (if exists)
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);


    /// the key pair is in progress
    if (get_kv_pair->mr_rdma != NULL){
        set_control_message(ptr_kv_handle, SERVER_IN_PROGRESS, NULL);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "handle_CLIENT_KV_GET SERVER_IN_PROGRESS: cannot send message\n");
            return 1;
        }
        return 0;
    }

    /// Copy key and value_size from remote_control_message buffer to local
    size_t key_length = strlen(buf_key_address) + 1;
    char *local_key_address = malloc(key_length);   // freed in free_array
    strcpy(local_key_address, buf_key_address);
    size_t value_length = *(size_t *) buf_value_length_address;

    /// Register a memory for client to rdma write
    char *local_value_address = malloc(value_length); // freed in free_array
    struct pingpong_context *ctx = ptr_kv_handle->ctx;
    struct MRInfo *mr_rdma = malloc(sizeof(struct MRInfo));
    mr_rdma->mr_start_ptr = (void *) (local_value_address);
    mr_rdma->mr_status = RDMA;
    mr_rdma->mr_size = value_length;
    mr_rdma->mr = ibv_reg_mr(ctx->pd,
                             mr_rdma->mr_start_ptr,
                             mr_rdma->mr_size,
                             IBV_ACCESS_LOCAL_WRITE |
                             IBV_ACCESS_REMOTE_WRITE);


    /// Send the key + remote virtual addr and remote key to client
    uint64_t remote_buf_va = bswap_64((uintptr_t)mr_rdma->mr_start_ptr);
    uint32_t remote_buf_rkey = htonl(mr_rdma->mr->rkey);
    const void *array_messages_address[2] = {&remote_buf_va,
                                             &remote_buf_rkey};
    set_control_message(ptr_kv_handle, SERVER_KV_SET_RENDEZVOUS,
                        array_messages_address);

    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "kv_set SERVER: cannot send message\n");
        return 1;
    }

    /// update the result to local database
    KeyValueAddressPair entry = {local_key_address, local_value_address,
                                 value_length, mr_rdma};
    insert_to_array(database, &entry);
    return 0;
}

/**
 * Some considerations:
 * 1. Check if the value size is indeed less than 4KB? (Trust client?)
 * 2. Check if kv_pair already exists? No need dynamic array handle this?
 * Even when previous kv pair is >4kb and now it's < 4kb, doesn't matter
 * 3. Check is kv_pair already exists and resource busy!
 * @param database
 * @param ptr_control_message
 * @return
 */
int handle_EAGER_KV_SET(KeyValueAddressArray *database,
                        ControlMessage *remote_control_message,
                        KVHandle *ptr_kv_handle) {
    /// decode the messages in remote control buffer
    // It should contain key + value size + value
    void *array_ptr_to_fill[3];
    decode_control_message_buffer(CLIENT_KV_SET_EAGER,
                                  remote_control_message->buf,
                                  array_ptr_to_fill);
    char *buf_key_address = array_ptr_to_fill[0];
    size_t *buf_value_length_address = array_ptr_to_fill[1];
    char *buf_value_address = array_ptr_to_fill[2];

    /// handle special cases: check if the resource is busy (if exists)
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);
    /// the key pair is in progress
    if (get_kv_pair != NULL){
        if (get_kv_pair->mr_rdma != NULL){
            set_control_message(ptr_kv_handle, SERVER_IN_PROGRESS, NULL);
            pp_post_send(ptr_kv_handle->ctx);
            if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
                fprintf(stderr, "handle_EAGER_KV_SET SERVER_IN_PROGRESS: cannot send message\n");
                return 1;
            }
            return 0;
        }
    }

    /// Copy key and value from remote_control_message buffer to local
    size_t key_length = strlen(buf_key_address) + 1;
    char *local_key_address = malloc(key_length);   // freed in free_array
    strcpy(local_key_address, buf_key_address);
    size_t value_length = *(size_t*) buf_value_length_address;
    char *local_value_address = malloc(value_length);   // freed in free_array
    strcpy(local_value_address, buf_value_address);

    /// insert the result to local database
    KeyValueAddressPair entry = {local_key_address, local_value_address,
                                 value_length, NULL};
    insert_to_array(database, &entry);

    /// send confirmation message to user
    set_control_message(ptr_kv_handle, SERVER_KV_SET_SUCCESSFUL, NULL);
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "handle_EAGER_KV_SET SERVER_KV_SET_SUCCESSFUL: cannot send message\n");
        return 1;
    }
    return 0;
}

int handle_CLIENT_KV_GET(KeyValueAddressArray *database, ControlMessage
*remote_control_message, KVHandle *ptr_kv_handle) {
    /// decode the messages in remote control buffer
    // It should contain key
    void *array_ptr_to_fill[1];
    decode_control_message_buffer(CLIENT_KV_GET,
                                  remote_control_message->buf,
                                  array_ptr_to_fill);
    char *buf_key_address = array_ptr_to_fill[0];

    /// handle special cases
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);
    /// the key is not found
    if (get_kv_pair == NULL){
        set_control_message(ptr_kv_handle, SERVER_KV_GET_KEY_NOT_FOUND, NULL);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "handle_CLIENT_KV_GET SERVER_KV_GET_KEY_NOT_FOUND: cannot send message\n");
            return 1;
        }
        return 0;
    }
    /// the key pair is in progress
    if (get_kv_pair->mr_rdma != NULL){
        set_control_message(ptr_kv_handle, SERVER_IN_PROGRESS, NULL);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "handle_CLIENT_KV_GET SERVER_IN_PROGRESS: cannot send message\n");
            return 1;
        }
        return 0;
    }

    /// if there is no special cases
    if (get_kv_pair->value_size < CONTROL_MESSAGE_BUFFER_SIZE) {
        /// We will send the buffer contains just the value
        size_t value_size = strlen(get_kv_pair->value_address) + 1;
        const void *array_messages_address[1] = {get_kv_pair->value_address};
        set_control_message(ptr_kv_handle, SERVER_KV_GET_EAGER,
                            array_messages_address);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "handle_CLIENT_KV_GET EAGER: cannot send message\n");
            return 1;
        }
        return 0;
    }else{
        /// Register the memory of value for client to rdma read
        char *local_value_address = get_kv_pair->value_address;
        struct pingpong_context *ctx = ptr_kv_handle->ctx;
        struct MRInfo *mr_rdma = malloc(sizeof(struct MRInfo)); // freed in handle_CLIENT_KV_GET_RENDEZVOUS_FIN
        mr_rdma->mr_start_ptr = (void *) (local_value_address);
        mr_rdma->mr_status = RDMA;
        mr_rdma->mr_size = get_kv_pair->value_size;
        mr_rdma->mr = ibv_reg_mr(ctx->pd,
                                 mr_rdma->mr_start_ptr,
                                 mr_rdma->mr_size,
                                 IBV_ACCESS_LOCAL_WRITE|
                                 IBV_ACCESS_REMOTE_READ);

        /// Send the value size + remote virtual addr + remote key to client
        uint64_t remote_buf_va = bswap_64((uintptr_t)mr_rdma->mr_start_ptr);
        uint32_t remote_buf_rkey = htonl(mr_rdma->mr->rkey);
        const void *array_messages_address[3] = {&get_kv_pair->value_size,
                                                 &remote_buf_va,
                                                 &remote_buf_rkey};
        set_control_message(ptr_kv_handle, SERVER_KV_GET_RENDEZVOUS,
                            array_messages_address);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set SERVER: cannot send message\n");
            return 1;
        }

        /// update the key_pair status
        get_kv_pair->mr_rdma = mr_rdma;
        return 0;
    }
}
/**
 * Server will just run this function, and respond to the clients requests
 * @return
 */
int run_server(KVHandle *ptr_kv_handle) {
    /// Initialize database: basically array of (key_addr, value_addr)
    set_kv_malloc();    // in server database we will malloc for key value
    KeyValueAddressArray *database = initialize_KeyValueAddressArray(20);

    while (1) {
        /// for each client, see if they send any messages
        for (int i = 0; i < ptr_kv_handle->num_remote_host; i++){
            /// switch to the context of this client
            ptr_kv_handle->ctx = ptr_kv_handle->array_ctx_ptr[i];
            // no blocking, poll the next control message
            ControlMessage *remote_control_message = NULL;
            int poll_msg_success = get_remote_control_message(ptr_kv_handle,
                                                              &remote_control_message,
                                                              0);
            if (poll_msg_success == 1){
                /// we didn't see any new information
                continue;
            }

            /// handle it
            // if we get a message from any client to tell us finish experiment
            if (remote_control_message->operation == SHUT_DOWN_SERVER) {
                printf("shutting down!\n");
                // todo: need a break flag
                break;
            }
                // eager kv_set
            else if (remote_control_message->operation == CLIENT_KV_SET_EAGER) {
                printf("run_server client %d CLIENT_KV_SET_EAGER!\n", i);
                handle_EAGER_KV_SET(database, remote_control_message,
                                    ptr_kv_handle);
                /// for testing: todo: to delete
                print_dynamic_array(database);
            }
                // rdma_kv_set client send key
            else if (remote_control_message->operation == CLIENT_KV_SET_RENDEZVOUS) {
                printf("run_server client %d CLIENT_KV_SET_RENDEZVOUS!\n", i);
                handle_RENDEZVOUS_KV_SET_KEY(database, remote_control_message,
                                             ptr_kv_handle);
                /// for testing: todo: to delete
                print_dynamic_array(database);

            }
            else if (remote_control_message->operation == CLIENT_KV_GET_RENDEZVOUS_FIN){
                printf("run_server client %d CLIENT_KV_GET_RENDEZVOUS_FIN!\n", i);
                handle_CLIENT_KV_GET_RENDEZVOUS_FIN(database,
                                                    remote_control_message);
                /// for testing: todo: to delete
                print_dynamic_array(database);
            }
                // kv_get: CLIENT_KV_GET
            else if (remote_control_message->operation == CLIENT_KV_GET) {
                printf("run_server client %d CLIENT_KV_GET!\n", i);
                handle_CLIENT_KV_GET(database, remote_control_message, ptr_kv_handle);
                /// for testing: todo: to delete
                print_dynamic_array(database);
            }
        }

    }

    /// free the database and every allocated memory inside
    free_array(database);
}