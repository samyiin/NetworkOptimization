#include "kv_api.h"
# include "dynamic_array.h"


/**
 * Basically the main in the template, get rid off all the options
 * @param servername
 * @param kv_handle
 * @return
 */
int kv_open(char *servername, void **kv_handle) {
    /// Initialize KV handle
    KVHandle *my_kv_handle = malloc(sizeof(KVHandle));  // freed in kv_close
    my_kv_handle->ib_devname = NULL;
    my_kv_handle->port = 12345;
    my_kv_handle->ib_port = 1;
    my_kv_handle->mtu = pp_mtu_to_enum(MTU);
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
            if (!strcmp(ibv_get_device_name(my_kv_handle->dev_list[i]),
                        my_kv_handle->ib_devname))
                break;
        my_kv_handle->ib_dev = my_kv_handle->dev_list[i];
        if (!my_kv_handle->ib_dev) {
            fprintf(stderr, "IB device %s not found\n",
                    my_kv_handle->ib_devname);
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
    if (pp_post_recv(my_kv_handle->ctx) == 0) {
        my_kv_handle->ctx->routs = my_kv_handle->ctx->rx_depth;
    } else {
        fprintf(stderr, "kv_open Couldn't post receive (%d)\n",
                my_kv_handle->ctx->routs);
        return 1;
    }

    if (my_kv_handle->use_event) {
        if (ibv_req_notify_cq(my_kv_handle->ctx->send_cq, 0)) {
            fprintf(stderr, "Couldn't request send_cq notification\n");
            return 1;
        }
        if (ibv_req_notify_cq(my_kv_handle->ctx->receive_cq, 0)) {
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
    if (pp_get_port_info(my_kv_handle->ctx->context,
                         my_kv_handle->ib_port,
                         &(my_kv_handle->ctx->portinfo))) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    my_kv_handle->my_dest.lid = my_kv_handle->ctx->portinfo.lid;
    if (my_kv_handle->ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND &&
        !my_kv_handle->my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }

    if (my_kv_handle->gidx >= 0) {
        if (ibv_query_gid(my_kv_handle->ctx->context, my_kv_handle->ib_port,
                          my_kv_handle->gidx, &(my_kv_handle->my_dest.gid))) {
            fprintf(stderr, "Could not get local gid for gid index %d\n",
                    my_kv_handle->gidx);
            return 1;
        }
    } else {
        memset(&(my_kv_handle->my_dest.gid), 0,
               sizeof my_kv_handle->my_dest.gid);
    }

    my_kv_handle->my_dest.qpn = my_kv_handle->ctx->qp->qp_num;
    my_kv_handle->my_dest.psn = lrand48() & 0xffffff;
    inet_ntop(AF_INET6, &(my_kv_handle->my_dest.gid), my_kv_handle->gid,
              sizeof my_kv_handle->gid);

    /*
     * If servername is provided, then we are running main as a client, so
     * we will exchange info with remote host using pp_client_exch_dest,
     * else we are the server, so we will exchange with pp_server_exch_dest
     * todo: pp_server_exch_dest will block the process until the client
     * connects to it.
     */
    if (servername) {
        my_kv_handle->rem_dest = pp_client_exch_dest(servername,
                                                     my_kv_handle->port,
                                                     &(my_kv_handle->my_dest));
    } else {
        my_kv_handle->rem_dest = pp_server_exch_dest(my_kv_handle->ctx,
                                                     my_kv_handle->ib_port,
                                                     my_kv_handle->mtu,
                                                     my_kv_handle->port,
                                                     my_kv_handle->sl,
                                                     &(my_kv_handle->my_dest),
                                                     my_kv_handle->gidx);
    }


    if (!my_kv_handle->rem_dest) {
        return 1;
    }

    inet_ntop(AF_INET6, &my_kv_handle->rem_dest->gid, my_kv_handle->gid,
              sizeof my_kv_handle->gid);

    /**
     * If we are client, we will try to establish connection with the remote
     * If we are server, we already establish connection with remote during
     * the pp_server_exch_dest above. But this will happen only after client
     * make connection with the server, because the server will be blocked at
     * pp_server_exch_dest until the client connect to it.
     */
    if (servername) {
        if (pp_connect_ctx(my_kv_handle->ctx,
                           my_kv_handle->ib_port,
                           my_kv_handle->my_dest.psn,
                           my_kv_handle->mtu,
                           my_kv_handle->sl,
                           my_kv_handle->rem_dest,
                           my_kv_handle->gidx)) {
            return 1;
        }
    }
}

/**
 * Store the start pointer of remote control message into
 * remote_control_message
 * @param ptr_kv_handle
 * @param remote_control_message
 * @return
 */
int get_remote_control_message(KVHandle *ptr_kv_handle, ControlMessage
**remote_control_message){
    int array_mr_ids[1];
    if (poll_n_receive_wc(ptr_kv_handle->ctx, 1, array_mr_ids)) {
        fprintf(stderr, "get_remote_control_message: didn't receive message\n");
        return 1;
    }
    // check which receive buffer got the response
    int mr_id = array_mr_ids[0];
    // make it a pointer because we need to set mr status to FREE later
    struct MRInfo *mr_receive_info =
            ptr_kv_handle->ctx->array_mr_receive_info + mr_id;
    *remote_control_message = (ControlMessage *)
            mr_receive_info->mr_start_ptr;
    // free the receive mr (It's a single thread program, so we can free it
    // earlier.
    mr_receive_info->mr_status = FREE;
}

/**
 * Set my_control_message to send
 * This will mem copy from the input pointers to the send control message mr
 * @return
 */
int set_control_message(KVHandle *ptr_kv_handle, enum Operation
        operation, const void **array_messages_address, const size_t
        *array_message_sizes, int array_size){
    ControlMessage *my_control_message = (ControlMessage *)
            ptr_kv_handle->ctx->mr_send_start_ptr;
    my_control_message->operation = operation;
    void *current_buf_ptr = my_control_message->buf;
    for (int i = 0; i < array_size; i++){
        memcpy(current_buf_ptr, array_messages_address[i],
               array_message_sizes[i]);
        current_buf_ptr += array_message_sizes[i];
    }
    return 0;
}

/**
 * Decode the message in control message buffer
 * If it's a string, provide message size == 0 (unknown)
 */
int decode_control_message_buffer(void *control_message_buf, void
**array_ptr_to_fill, const size_t *array_ptr_sizes, int array_size){
    if (array_size == 1){
        array_ptr_to_fill[0] = control_message_buf;
        return 0;
    }
    void *current_buf_ptr = control_message_buf;
    for (int i = 0; i < array_size; i++){
        array_ptr_to_fill[i] = current_buf_ptr;
        if (array_ptr_sizes[i] == 0){
            // It's a string, so we don't know it's size
            size_t string_length = strlen(current_buf_ptr) + 1;
            current_buf_ptr += string_length;
        }else{
            current_buf_ptr += array_ptr_sizes[i];
        }
    }
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
    if (strlen(key) + strlen(value) + 2 <= CONTROL_MESSAGE_BUFFER_SIZE) {
        /// send a message to the server: key + value_size + value
        size_t key_size = strlen(key) + 1;
        size_t value_size = strlen(value) + 1;
        const void *array_messages_address[3] = {key, &value_size, value};
        const size_t array_message_sizes[3] = {key_size, sizeof(size_t),
                                          value_size};
        set_control_message(ptr_kv_handle, CLIENT_KV_SET_EAGER,
                            array_messages_address,
                            array_message_sizes, 3);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set EAGER: cannot send message\n");
            return 1;
        }

        /// check if successfully set
        ControlMessage *remote_control_message;
        get_remote_control_message(ptr_kv_handle, &remote_control_message);

        // the resource is in progress
        if (remote_control_message->operation == SERVER_IN_PROGRESS){
            fprintf(stderr, "kv_set_eager: SERVER_IN_PROGRESS\n");
            return 2;
        }

        if (remote_control_message->operation != SERVER_KV_SET_SUCCESSFUL){
            fprintf(stderr, "kv_set_eager: no SERVER_KV_SET_SUCCESSFUL\n");
            return 1;
        }
    } else {
        /// send a control message to the server: key + value size (size_t)
        size_t key_size = strlen(key) + 1;
        size_t value_size = strlen(value) + 1;
        const void *array_messages_address[2] = {key, &value_size};
        const size_t array_message_sizes[2] = {key_size, sizeof(size_t)};
        set_control_message(ptr_kv_handle, CLIENT_KV_SET_RENDEZVOUS,
                            array_messages_address,
                            array_message_sizes, 2);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set REND_KEY: cannot send message\n");
            return 1;
        }

        /// temporarily register the address of the value as a memory region.
        ptr_kv_handle->ctx->mr_rdma_start_ptr = (void *) value;
        ptr_kv_handle->ctx->mr_rdma_size = value_size;
        ptr_kv_handle->ctx->mr_rdma = ibv_reg_mr(ptr_kv_handle->ctx->pd,
                                                 ptr_kv_handle->ctx->mr_rdma_start_ptr,
                                                 ptr_kv_handle->ctx->mr_rdma_size,
                                                 IBV_ACCESS_LOCAL_WRITE |
                                                 IBV_ACCESS_REMOTE_WRITE);


        /// We will get remote va and remote key from server
        ControlMessage *remote_control_message;
        get_remote_control_message(ptr_kv_handle, &remote_control_message);

        // the resource is in progress
        if (remote_control_message->operation == SERVER_IN_PROGRESS){
            fprintf(stderr, "kv_set_eager: SERVER_IN_PROGRESS\n");
            return 2;
        }
        if (remote_control_message->operation != SERVER_KV_SET_RENDEZVOUS){
            fprintf(stderr, "kv_set: no SERVER_KV_SET_RENDEZVOUS\n");
            return 1;
        }


        /// decode remote control message
        // should be remote va and remote key
        void *array_ptr_to_fill[2];
        const size_t array_ptr_sizes[2] = {sizeof(uint64_t), sizeof(uint32_t)};
        decode_control_message_buffer(remote_control_message->buf,
                                      array_ptr_to_fill,
                                      array_ptr_sizes, 2);
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
        const size_t array_message_sizes_1[1] = {key_size};
        set_control_message(ptr_kv_handle, CLIENT_RENDEZVOUS_FIN,
                            array_messages_address_1,
                            array_message_sizes_1, 1);
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
    }
    return 0;
}

/**
 * Here we must trust the client: the key exists in the database, the client
 * was processing the key (RDMA)
 * @param database
 * @param remote_control_message
 * @return
 */
int handle_CLIENT_RENDEZVOUS_FIN(KeyValueAddressArray *database,
                             ControlMessage *remote_control_message){
    /// decode the messages in remote control buffer
    // It should contain key + value size
    void *array_ptr_to_fill[1];
    const size_t array_ptr_sizes[1] = {0};
    decode_control_message_buffer(remote_control_message->buf,
                                  array_ptr_to_fill,
                                  array_ptr_sizes, 2);
    char *buf_key_address = array_ptr_to_fill[0];

    /// Get the key value
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);
    /// Deregister the rdma mr
    struct MRInfo *mr_rdma = get_kv_pair->in_progress;
    if (ibv_dereg_mr(mr_rdma->mr)) {
        fprintf(stderr, "Couldn't deregister MR_control_send\n");
        return 1;
    }
    free(mr_rdma);

    /// Update the key_pair status
    get_kv_pair->in_progress = NULL;

    return 0;
}

int handle_RENDEZVOUS_KV_SET_KEY(KeyValueAddressArray *database,
                                 ControlMessage *remote_control_message,
                                 KVHandle *ptr_kv_handle) {
    /// decode the messages in remote control buffer
    // It should contain key + value size
    void *array_ptr_to_fill[2];
    const size_t array_ptr_sizes[2] = {0, sizeof(size_t)};
    decode_control_message_buffer(remote_control_message->buf,
                                  array_ptr_to_fill,
                                  array_ptr_sizes, 2);
    char *buf_key_address = array_ptr_to_fill[0];
    size_t *buf_value_length_address = array_ptr_to_fill[1];

    /// handle special cases: check if the resource is busy (if exists)
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);
    /// the key pair is in progress
    if (get_kv_pair->in_progress != NULL){
        set_control_message(ptr_kv_handle, SERVER_IN_PROGRESS, NULL,
                            NULL, 0);
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
    struct MRInfo *mr_rdma = malloc(sizeof(struct MRInfo)); // freed in handle_CLIENT_RENDEZVOUS_FIN
    mr_rdma->mr_start_ptr = (void *) (local_value_address);
    mr_rdma->mr_status = RDMA;
    mr_rdma->mr_size = value_length;
    mr_rdma->mr = ibv_reg_mr(ctx->pd,
                             mr_rdma->mr_start_ptr,
                             mr_rdma->mr_size,
                                   IBV_ACCESS_LOCAL_WRITE |
                                   IBV_ACCESS_REMOTE_WRITE);

    /// Send the remote virtual addr and remote key to client
    uint64_t remote_buf_va = bswap_64((uintptr_t)mr_rdma->mr_start_ptr);
    uint32_t remote_buf_rkey = htonl(mr_rdma->mr->rkey);
    const void *array_messages_address[2] = {&remote_buf_va, &remote_buf_rkey};
    const size_t array_message_sizes[2] = {sizeof(uint64_t), sizeof(uint32_t)};
    set_control_message(ptr_kv_handle, SERVER_KV_SET_RENDEZVOUS,
                        array_messages_address, array_message_sizes, 2);
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "kv_set SERVER: cannot send message\n");
        return 1;
    }

    /// insert the result to local database
    KeyValueAddressPair entry = {local_key_address, local_value_address,
                                 value_length, mr_rdma};
    insert_array(database, &entry);
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
    const size_t array_ptr_sizes[3] = {0, sizeof(size_t), 0};
    decode_control_message_buffer(remote_control_message->buf,
                                  array_ptr_to_fill,
                                  array_ptr_sizes, 3);
    char *buf_key_address = array_ptr_to_fill[0];
    size_t *buf_value_length_address = array_ptr_to_fill[1];
    char *buf_value_address = array_ptr_to_fill[2];

    /// handle special cases: check if the resource is busy (if exists)
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);
    /// the key pair is in progress
    if (get_kv_pair != NULL){
        if (get_kv_pair->in_progress != NULL){
            set_control_message(ptr_kv_handle, SERVER_IN_PROGRESS, NULL,
                                NULL, 0);
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
    insert_array(database, &entry);

    /// send confirmation message to user
    set_control_message(ptr_kv_handle, SERVER_KV_SET_SUCCESSFUL, NULL,
                        NULL, 0);
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "handle_EAGER_KV_SET SERVER_KV_SET_SUCCESSFUL: cannot send message\n");
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
int kv_get(void *kv_handle, const char *key, char **var) {
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    /// First send a message containing the key
    size_t key_size = strlen(key) + 1;
    const void *array_messages_address[1] = {key};
    const size_t array_message_sizes[1] = {key_size};
    set_control_message(ptr_kv_handle, CLIENT_KV_GET, array_messages_address,
                        array_message_sizes, 1);
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "kv_get CLIENT_KV_GET: cannot send message\n");
        return 1;
    }

    /// get one response from server
    ControlMessage *remote_control_message;
    get_remote_control_message(ptr_kv_handle, &remote_control_message);

    if (remote_control_message->operation == SERVER_KV_GET_EAGER) {
        /// decode the response: result should contain just the value
        void *array_ptr_to_fill[1];
        const size_t array_ptr_sizes[1] = {0};
        decode_control_message_buffer(remote_control_message->buf,
                                      array_ptr_to_fill,
                                      array_ptr_sizes, 1);
        char *buf_value_address = array_ptr_to_fill[0];
        // store to local buffer
        *var = malloc(strlen(buf_value_address) + 1);   // freed in kv_release
        strcpy(*var, buf_value_address);
        return 0;
    } else if (remote_control_message->operation == SERVER_KV_GET_RENDEZVOUS) {
        /// decode remote control message
        // should be value size + remote va and remote key
        void *array_ptr_to_fill[3];
        const size_t array_ptr_sizes[3] = {sizeof(size_t),
                                           sizeof(uint64_t),
                                           sizeof(uint32_t)};
        decode_control_message_buffer(remote_control_message->buf,
                                      array_ptr_to_fill,
                                      array_ptr_sizes, 3);
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
        const size_t array_message_sizes_1[1] = {key_size};
        set_control_message(ptr_kv_handle, CLIENT_RENDEZVOUS_FIN,
                            array_messages_address_1,
                            array_message_sizes_1, 1);
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
    }else {
        *var = NULL;
        fprintf(stderr, "kv_get: SERVER_KV_GET something went wrong\n");
        return 1;
    }

}

int handle_CLIENT_KV_GET(KeyValueAddressArray *database, ControlMessage
*remote_control_message, KVHandle *ptr_kv_handle) {
    /// decode the messages in remote control buffer
    // It should contain key
    void *array_ptr_to_fill[1];
    const size_t array_ptr_sizes[1] = {0};
    decode_control_message_buffer(remote_control_message->buf,
                                  array_ptr_to_fill,
                                  array_ptr_sizes, 1);
    char *buf_key_address = array_ptr_to_fill[0];

    /// handle special cases
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);
    /// the key is not found
    if (get_kv_pair == NULL){
        set_control_message(ptr_kv_handle, SERVER_KV_GET_KEY_NOT_FOUND, NULL,
                            NULL, 0);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "handle_CLIENT_KV_GET SERVER_KV_GET_KEY_NOT_FOUND: cannot send message\n");
            return 1;
        }
        return 0;
    }
    /// the key pair is in progress
    if (get_kv_pair->in_progress != NULL){
        set_control_message(ptr_kv_handle, SERVER_IN_PROGRESS, NULL,
                            NULL, 0);
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
        const size_t array_message_sizes[1] = {value_size};
        set_control_message(ptr_kv_handle, SERVER_KV_GET_EAGER,
                            array_messages_address,
                            array_message_sizes, 1);
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
        struct MRInfo *mr_rdma = malloc(sizeof(struct MRInfo)); // freed in handle_CLIENT_RENDEZVOUS_FIN
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
        const size_t array_message_sizes[3] = {sizeof(size_t),
                                               sizeof(uint64_t),
                                               sizeof(uint32_t)};
        set_control_message(ptr_kv_handle, SERVER_KV_GET_RENDEZVOUS,
                            array_messages_address, array_message_sizes, 3);
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set SERVER: cannot send message\n");
            return 1;
        }

        /// update the key_pair status
        get_kv_pair->in_progress = mr_rdma;
        return 0;
    }
}

/**
 * Free everything
 * @param kv_handle
 * @return
 */
int kv_close(void *kv_handle) {
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    pp_close_ctx(ptr_kv_handle->ctx);
    ibv_free_device_list(ptr_kv_handle->dev_list);
    free(ptr_kv_handle->rem_dest);
}

/**
 * Free the value after kv_get() because in kv_get we need to allocate a
 * space for receiving value.
 * @param value
 */
void kv_release(char *value) {
    free(value);
}


/**
 * Server will just run this function, and respond to the clients requests
 * @return
 */
int run_server(KVHandle *ptr_kv_handle) {
    /// Initialize database: basically array of (key_addr, value_addr)
    KeyValueAddressArray *database = initialize_KeyValueAddressArray(20);

    while (1) {
        // todo: handle get remoste control message fail case
        ControlMessage *remote_control_message = NULL;
        get_remote_control_message(ptr_kv_handle, &remote_control_message);

        /// handle it
        // if we get a message from any client to tell us finish experiment
        if (remote_control_message->operation == SHUT_DOWN_SERVER) {
            printf("shutting down!\n");
            break;
        }
        // eager kv_set
        else if (remote_control_message->operation == CLIENT_KV_SET_EAGER) {
            printf("run_server CLIENT_KV_SET_EAGER!\n");

            handle_EAGER_KV_SET(database, remote_control_message,
                                ptr_kv_handle);
        }
        // rdma_kv_set client send key
        else if (remote_control_message->operation == CLIENT_KV_SET_RENDEZVOUS) {
            handle_RENDEZVOUS_KV_SET_KEY(database, remote_control_message,
                                         ptr_kv_handle);
            printf("run_server CLIENT_KV_SET_RENDEZVOUS!\n");

        }
        else if (remote_control_message->operation == CLIENT_RENDEZVOUS_FIN){
            handle_CLIENT_RENDEZVOUS_FIN(database, remote_control_message);
            printf("run_server CLIENT_RENDEZVOUS_FIN!\n");

        }
        // kv_get: CLIENT_KV_GET
        else if (remote_control_message->operation == CLIENT_KV_GET) {
            handle_CLIENT_KV_GET(database, remote_control_message, ptr_kv_handle);
            printf("run_server CLIENT_KV_GET!\n");
        }

        else{
            fprintf(stderr, "run server: unknown operation!\n");
            break;
        }
        /// for testing: todo: to delete
        print_dynamic_array(database);
    }

    /// free the database and every allocated memory inside
    free_array(database);
}
