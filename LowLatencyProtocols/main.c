//
// Created by hsiny on 9/16/24.
//
#include <time.h>
# include "kv_api.h"
# define NUM_THROUGHPUT_ITERATIONS 1024

int shut_down_server(void *kv_handle){
    KVHandle *ptr_kv_handle = (KVHandle *) kv_handle;
    // should specify which key
    set_control_message(ptr_kv_handle, SHUT_DOWN_SERVER, NULL);
    pp_post_send(ptr_kv_handle->ctx);
    if (poll_n_send_wc(ptr_kv_handle->ctx, 1) != 0) {
        fprintf(stderr, "Shut down server failed!\n");
        return 1;
    }
    return 0;
}

int throughput_test(void *kv_handle, int test_type){
    int list_message_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
                                2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144,
                                524288, 1048576};
    int len_list_message_sizes = 21;
    for (int i = 0; i < len_list_message_sizes; i++){
        /// set the message size for this trial
        int message_size = list_message_sizes[i];

        /// preprocessing: Set key and set_value
        char key[30];
        sprintf(key, "test_key_%d", i);
        char *set_value;
        char *get_value[NUM_THROUGHPUT_ITERATIONS];

        if (test_type == 0){
            // kv_set test: set key and set_value
            set_value = malloc(message_size);
            memset(set_value, 'a', message_size);
            set_value[message_size -1] = '\0';
        }

        /// strat timer
        struct timeval start_time, end_time;
        long time_elapse;
        gettimeofday(&start_time, NULL);

        /// Perform one test
        for (int k = 0; k < NUM_THROUGHPUT_ITERATIONS; k++){
            if (test_type ==0){
                kv_set(kv_handle, key, set_value);
            }else{
                kv_get(kv_handle, key, &get_value[k]);
            }
        }

        /// end timer: calculate time elapsed
        gettimeofday(&end_time, NULL);
        // calculate time: in seconds
        double const total_second = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec)/1000000.0;

        /// calculate throughput: afraid of overflow
        double const total_sent_Mb = NUM_THROUGHPUT_ITERATIONS * (message_size * 8 / 1048576.0);
        double const throughput = total_sent_Mb / total_second;

        /// print the throughput: In MegaBytes per second
        printf("%d\t\t%.2f\t\tMbits/sec\n", message_size, throughput);

        /// Free all resources
        if (test_type ==0){
            free(set_value);
        }else{
            for (int j = 0; j < NUM_THROUGHPUT_ITERATIONS; j++){
                if (get_value[j] != NULL){
                    free(get_value[j]);
                }
            }
        }
    }
    return 0;
}

void generate_random_value(char **value_address, size_t value_length){
    /// randomly generated a value with a give length
    // I did malloc on the fly but I don't have to....
    *value_address = malloc(value_length);
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int char_idx = rand() % sizeof(charset) -1;
    memset(*value_address, charset[char_idx], value_length);
}

void execute_input_command(FILE *input_file, void *kv_handle){
    char line[CONTROL_MESSAGE_BUFFER_SIZE];
    while (fgets(line, sizeof(line), input_file)){
        char key[CONTROL_MESSAGE_BUFFER_SIZE];
        size_t value_length;
        if (sscanf(line, "SET \"%[^\"]\" %zu", key, &value_length) == 2){
            char *value;
            generate_random_value(&value, value_length);
            printf("kv_set: key: %-10.10s value: %10.10s\n", key, value);
            if (kv_set(kv_handle, key, value) != 0){
                printf("SET KEY FAILED\n");
            }
            free(value);
        }else if (sscanf(line, "GET \"%[^\"]\"", key)==1){
            printf("kv_get: key: %-10.10s\n", key);
            char *value;
            if (kv_get(kv_handle, key, &value) != 0){
                printf("GET KEY FAILED\n");
            } else{
                printf("kv_get: key: %-10.10s value: %10.10s\n", key, value);
                kv_release(value);
            }

        }else if (sscanf(line, "PRINT %[^\n]", key)){
            printf("%s\n", key);
        }else{
            printf("There is problem in the input file! \n");
        }
        printf("=================================================\n");
    }
}


/**
 * Funtion that provides usage to user
 * @param argv0
 */
static void usage(const char *argv0){
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("  %s <host> <input_file>    connect to server at <host>\n", argv0);
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
    FILE *input_file;
    int perform_throughput_test;
    if (argc > 1){
        servername = argv[1];
        if (argc == 2){
            perform_throughput_test = 1;
        }else if (argc == 3){
            perform_throughput_test = 0;
            input_file = fopen(argv[2], "r");
            if (!input_file){
                fprintf(stderr, "Cannot read the input file!\n");
                return 1;
            }
        }else{
            usage(argv[0]);
        }

    }else{
        servername == NULL;
    }

    /// Create an empty pointer, kv_open will add stuff to it
    KVHandle *kv_handle;
    if (kv_open(servername, (void*) &kv_handle) == 1){
        printf("in main: kv_open failed!");
        return -1;
    };


    if (perform_throughput_test){
        if (!servername){
            run_server(kv_handle, 0);
        } else{
            printf("=====================================================\n");
            printf("Test kv_set\n");
            printf("=====================================================\n");
            throughput_test(kv_handle, 0);
            printf("=====================================================\n");
            printf("Test kv_get\n");
            printf("=====================================================\n");
            throughput_test(kv_handle, 1);
            shut_down_server(kv_handle);
        }
    }else {
        if (!servername){
            run_server(kv_handle, 1);
        } else{
            execute_input_command(input_file, kv_handle);
            shut_down_server(kv_handle);
        }
    }

    /// free everything
    kv_close(kv_handle);

    // todo: if exist by error, the free pointers might get affected
    return 0;
}


