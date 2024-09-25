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
    KVHandle *my_kv_handle = malloc(sizeof(KVHandle));
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

    /// todo this will be deleted later
    printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
           my_kv_handle->rem_dest->lid, my_kv_handle->rem_dest->qpn,
           my_kv_handle->rem_dest->psn, my_kv_handle->gid);

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
        // concatenate the mr_control_start_pointer as a kv_addr_pair pointer
        ControlMessage *key_value_pair = (ControlMessage *)
                ptr_kv_handle->ctx->mr_send_start_ptr;
        // We will just mem_copy to a registered memory region: mr_control
        key_value_pair->operation = EAGER_KV_SET;
        // key
        strcpy(key_value_pair->buf, key);
        // value size
        size_t value_size = strlen(value) + 1;
        *(size_t *) (key_value_pair->buf + strlen(key) + 1) = value_size;
        // value
        strcpy(key_value_pair->buf + strlen(key) + 1 + sizeof(size_t), value);
        // send the message and Wait for this send to finish
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set EAGER: cannot send message\n");
            return 1;
        }
        // (probably need one confirm from server)
    } else {
        /// send a control message to the server: key + value size (size_t)
        // concatenate the mr_control_start_pointer as a kv_addr_pair pointer
        ControlMessage *key_value_pair = (ControlMessage *)
                ptr_kv_handle->ctx->mr_send_start_ptr;
        // We will just mem_copy to a registered memory region: mr_control
        key_value_pair->operation = RENDEZVOUS_KV_SET_KEY;
        strcpy(key_value_pair->buf, key);
        size_t value_size = strlen(value) + 1;
        *(size_t *) (key_value_pair->buf + strlen(key) + 1) = value_size;
        // send the message and Wait for this send to finish
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "kv_set REND_KEY: cannot send message\n");
            return 1;
        }
        /// We will register the address of the value as a memory region.
        // todo: do we need to malloc? do we need it at all?
        struct pingpong_context *ctx = ptr_kv_handle->ctx;
        struct MRInfo *mr_rdma_write = malloc(sizeof(struct MRInfo));
        mr_rdma_write->mr_start_ptr = (void *) (value);
        mr_rdma_write->mr_status = RDMA;
        mr_rdma_write->mr_size = value_size;
        mr_rdma_write->mr = ibv_reg_mr(ctx->pd,
                                       mr_rdma_write->mr_start_ptr,
                                       mr_rdma_write->mr_size,
                                       IBV_ACCESS_LOCAL_WRITE |
                                       IBV_ACCESS_REMOTE_WRITE);
        /// We will get remote va and remote key from server
        // todo: check if the response is not busy!
        /// poll 1 receive wc from receive cq: get one response from server
        int array_mr_ids[1];
        if (poll_n_receive_wc(ptr_kv_handle->ctx, 1, array_mr_ids)) {
            fprintf(stderr, "kv_get CLIENT_KV_GET: didn't receive message\n");
            return 1;
        }
        // check which receive buffer got the response
        int mr_id = array_mr_ids[0];
        // make it a pointer because we need to set mr status to FREE later
        struct MRInfo *mr_receive_info =
                ptr_kv_handle->ctx->array_mr_receive_info + mr_id;
        // cast the memory region in form of struct Control message
        ControlMessage *control_message = (ControlMessage *)
                mr_receive_info->mr_start_ptr;
        // get the remote key and remote va
        uint64_t remote_buf_va = *(uint64_t*) control_message->buf;
        uint32_t remote_buf_rkey = *(uint32_t *) (control_message->buf + sizeof(uint64_t));
        /// todo: We will perform rdma write to the server
        /// todo: then write fin to server

        /// Finally deregister the mr and free our responsibility
        if (ibv_dereg_mr(mr_rdma_write->mr)) {
            fprintf(stderr, "Couldn't deregister MR_control_send\n");
            return 1;
        }
        free(mr_rdma_write);
    }
    return 0;
}


int handle_RENDEZVOUS_KV_SET_KEY(KeyValueAddressArray *database,
                                  ControlMessage
*ptr_control_message,  KVHandle *ptr_kv_handle) {
    // todo: check resource busy!
    // According to eager protocol: the buffer will contain key and value size
    char *control_message_buf = ptr_control_message->buf;
    /// The key will be from the start address till first null terminator
    char *buf_key_address = control_message_buf;
    int key_length = strlen(buf_key_address) + 1;
    char *local_key_address = malloc(key_length);
    strcpy(local_key_address, buf_key_address);
    /// The value size will be following right after key
    char *buf_value_size_address = control_message_buf + key_length;
    size_t value_size = *(size_t *) buf_value_size_address;
    /// Register a memory for client to rdma write
    char *local_value_address = malloc(value_size);
    struct pingpong_context *ctx = ptr_kv_handle->ctx;
    struct MRInfo *mr_rdma_write = malloc(sizeof(struct MRInfo));
    mr_rdma_write->mr_start_ptr = (void *) (local_value_address);
    mr_rdma_write->mr_status = RDMA;
    mr_rdma_write->mr_size = value_size;
    mr_rdma_write->mr = ibv_reg_mr(ctx->pd,
                                   mr_rdma_write->mr_start_ptr,
                                   mr_rdma_write->mr_size,
                                   IBV_ACCESS_LOCAL_WRITE |
                                   IBV_ACCESS_REMOTE_WRITE);
    /// Send the remote virtual addr and remote key to client
    uint64_t remote_buf_va = bswap_64((uintptr_t)mr_rdma_write->mr_start_ptr);
    uint32_t remote_buf_rkey = htonl(mr_rdma_write->mr->rkey);
    // concatenate the mr_control_start_pointer as a kv_addr_pair pointer
    ControlMessage *key_value_pair = (ControlMessage *)
            ptr_kv_handle->ctx->mr_send_start_ptr;
    key_value_pair->operation = RENDEZVOUS_KV_SET_SERVER;
    // remote va and remote key
    *(uint64_t *) key_value_pair->buf = remote_buf_va;
    *(uint32_t *) (key_value_pair->buf + sizeof(uint64_t)) = remote_buf_rkey;
    // send the message and Wait for this send to finish
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "kv_set SERVER: cannot send message\n");
        return 1;
    }
    /// insert the result to local database
    KeyValueAddressPair entry = {local_key_address, local_value_address,
                                 value_size, mr_rdma_write};
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
int handle_EAGER_KV_SET(KeyValueAddressArray *database, ControlMessage
*ptr_control_message) {
    // todo: check if resource busy!
    // According to eager protocol: the buffer will contain key and value
    char *control_message_buf = ptr_control_message->buf;
    /// The key will be from the start address till first null terminator
    char *buf_key_address = control_message_buf;
    int key_length = strlen(buf_key_address) + 1;
    char *local_key_address = malloc(key_length);
    strcpy(local_key_address, buf_key_address);
    /// The value size will be right after key
    char *buf_value_size_address = control_message_buf + key_length;
    size_t value_size = *(size_t *) buf_value_size_address;
    /// The value will be following right after value size
    char *buf_value_address = buf_value_size_address + sizeof(size_t);
    char *local_value_address = malloc(value_size);
    strcpy(local_value_address, buf_value_address);
    /// insert the result to local database
    KeyValueAddressPair entry = {local_key_address, local_value_address,
                                 value_size, NULL};
    insert_array(database, &entry);
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
    /// First send a message containing the key
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    ControlMessage *key_value_pair = (ControlMessage *)
            ptr_kv_handle->ctx->mr_send_start_ptr;
    // We will just mem_copy to a registered memory region: mr_control
    key_value_pair->operation = CLIENT_KV_GET;
    strcpy(key_value_pair->buf, key);
    // send the message and wait for the send to finish
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "kv_get CLIENT_KV_GET: cannot send message\n");
        return 1;
    }
    /// poll 1 receive wc from receive cq: get one response from server
    int array_mr_ids[1];
    if (poll_n_receive_wc(ptr_kv_handle->ctx, 1, array_mr_ids)) {
        fprintf(stderr, "kv_get CLIENT_KV_GET: didn't receive message\n");
        return 1;
    }
    // check which receive buffer got the response
    int mr_id = array_mr_ids[0];
    // make it a pointer because we need to set mr status to FREE later
    struct MRInfo *mr_receive_info =
            ptr_kv_handle->ctx->array_mr_receive_info + mr_id;
    // cast the memory region in form of struct Control message
    ControlMessage *control_message = (ControlMessage *)
            mr_receive_info->mr_start_ptr;

    /// Handle the response
    if (control_message->operation == SERVER_KV_GET_EAGER) {
        // According to eager protocol, the value should be inside the buffer.
        char *buffer_value_address = control_message->buf;
        size_t value_length = strlen(buffer_value_address) + 1;
        char *local_value_address = malloc(value_length);
        // mem copy to where the value is
        strcpy(local_value_address, buffer_value_address);
        // assign this address to var
        *var = local_value_address;
        return 0;
    } else if (control_message->operation == SERVER_KV_GET_RENDEZVOUS) {

    } else if (control_message->operation == SERVER_KV_GET_KEY_NOT_FOUND){
        fprintf(stderr, "kv_get: SERVER_KV_GET key not found! \n");
        return 1;
    }else {
        // todo: add could be resource busy
        fprintf(stderr, "kv_get: SERVER_KV_GET something went wrong\n");
        return 1;
    }
    /// Free resources in my responsibility
    // free the receive mr so we can post receive again
    mr_receive_info->mr_status = FREE;
}

int handle_CLIENT_KV_GET(KeyValueAddressArray *database, ControlMessage
*ptr_control_message, KVHandle *ptr_kv_handle) {
    // todo: check resource busy!
    // According to protocol: the buffer will contain key
    char *control_message_buf = ptr_control_message->buf;
    // The key will be from the start address till first null terminator
    char *buf_key_address = control_message_buf;
    // get the corresponding key_val_pair in database
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               buf_key_address);
    // todo: if didn't exist in database: send another opcode
    if (get_kv_pair->value_size < CONTROL_MESSAGE_BUFFER_SIZE) {
        /// We will send the value directly
        ControlMessage *key_value_pair = (ControlMessage *)
                ptr_kv_handle->ctx->mr_send_start_ptr;
        // We will just mem_copy to a registered memory region: mr_control
        key_value_pair->operation = SERVER_KV_GET_EAGER;
        strcpy(key_value_pair->buf, get_kv_pair->value_address);
        // send the message and wait for the send to finish
        pp_post_send(ptr_kv_handle->ctx);
        if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
            fprintf(stderr, "_handle_client_kv_get: cannot send message\n");
            return 1;
        }
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
 * todo: handle the free resource here!!!
 * @return
 */
int run_server(KVHandle *ptr_kv_handle) {
    /// Initialize database: basically array of (key_addr, value_addr)
    KeyValueAddressArray *database = initialize_KeyValueAddressArray(20);

    while (1) {
        /// poll 1 receive wc from receive cq: get one response from server
        int array_mr_ids[1];
        if (poll_n_receive_wc(ptr_kv_handle->ctx, 1, array_mr_ids)) {
            fprintf(stderr, "kv_get CLIENT_KV_GET: didn't receive message\n");
            return 1;
        }
        // check which receive buffer got the response
        int mr_id = array_mr_ids[0];
        // make it a pointer because we need to set mr status to FREE later
        struct MRInfo *mr_receive_info =
                ptr_kv_handle->ctx->array_mr_receive_info + mr_id;
        // cast the memory region in form of struct Control message
        ControlMessage *control_message = (ControlMessage *)
                mr_receive_info->mr_start_ptr;


        /// handle it
        // if we get a message from any client to tell us finish experiment
        if (control_message->operation == SHUT_DOWN_SERVER) {
            break;
        }
        // eager kv_set
        if (control_message->operation == EAGER_KV_SET) {
            handle_EAGER_KV_SET(database, control_message);
        }
        // kv_get: CLIENT_KV_GET
        if (control_message->operation == CLIENT_KV_GET) {
            handle_CLIENT_KV_GET(database, control_message, ptr_kv_handle);
        }
        // rdma_kv_set client send key
        if (control_message->operation == RENDEZVOUS_KV_SET_KEY) {
            handle_RENDEZVOUS_KV_SET_KEY(database, control_message,
                                         ptr_kv_handle);
        }
        /// Finish handling this receive, this region can be post receive again
        mr_receive_info->mr_status = FREE;

        /// for testing: see how many value are set
        print_dynamic_array(database);
    }

    /// free the database and every allocated memory inside
    free_array(database);
}
