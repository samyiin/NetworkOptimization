//
// Created by hsiny on 9/16/24.
//
#include <time.h>
# include "kv_api.h"

int perform_eager_test(const char *servername, void *kv_handle){
    if (!servername){
        run_server(kv_handle);
    }else{
        char *my_value;
        printf("client kv_set start\n");
        char *key1 = "key1", *value1 = "value1";
        kv_set(kv_handle, key1, value1);
        printf("Client: kv_set key: %s, value: %s\n", key1, value1);

        kv_get(kv_handle, key1, &my_value);
        printf("Got value: %s\n", my_value);

        char *key2 = "key2", *value2 = "value2";
        kv_set(kv_handle, key2, value2);
        printf("Client: kv_set key: %s, value: %s\n", key2, value2);


        kv_get(kv_handle, key2, &my_value);
        printf("Got value: %s\n", my_value);

        char *key3 = "key3", *value3 = "value3";
        kv_set(kv_handle, key3, value3);
        printf("Client: kv_set key: %s, value: %s\n", key3, value3);

        kv_get(kv_handle, key3, &my_value);
        printf("Got value: %s\n", my_value);

        value1 = "new value!!!";
        kv_set(kv_handle, key1, value1);
        printf("Client: kv_set key: %s, value: %s\n", key1, value1);

        kv_get(kv_handle, key1, &my_value);
        printf("Got value: %s\n", my_value);


        kv_get(kv_handle, "non-exist-key", &my_value);
        printf("Got value: %s\n", my_value);
    }
    return 0;
}

int perform_rendezvous_test(const char *servername, void *kv_handle){
    if (!servername){
        run_server(kv_handle);
    }else {
        char *my_value;
        const size_t large_value_size = 50000;

        char *key1 = "key1";
        char value1[large_value_size];
        memset(value1, 'a', sizeof(value1) - 1);
        value1[large_value_size - 1] = '\0';
        kv_set(kv_handle, key1, value1);
        printf("Client: kv_set key: %s, value: %-10.10s\n", key1, value1);

        kv_get(kv_handle, key1, &my_value);
        printf("Got value: %-10.10s\n", my_value);
        kv_release(my_value);

        char *key2 = "key2";
        char value2[large_value_size];
        memset(value2, 'b', sizeof(value1) - 1);
        value1[large_value_size - 1] = '\0';
        kv_set(kv_handle, key2, value2);

        printf("Client: kv_set key: %s, value: %-10.10s\n", key2, value2);
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
    if (kv_open(servername, (void*) &kv_handle) == 1){
        printf("in main: kv_open failed!");
        return -1;
    };

    perform_eager_test(servername, kv_handle);

    /// test kv set
    perform_rendezvous_test(servername, kv_handle);

    /// free everything
    kv_close(kv_handle);

    // todo: if exist by error, the free pointers might get affected
    return 0;
}


