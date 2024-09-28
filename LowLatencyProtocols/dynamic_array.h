//
// Created by hsiny on 9/22/24.
//

#ifndef NETWORKOPTIMIZATION_DYNAMIC_ARRAY_H
#define NETWORKOPTIMIZATION_DYNAMIC_ARRAY_H
# include <string.h>
# include <stdio.h>

/// Given that the key and value will not be bigger than 4KB

#include <malloc.h>

/**
 * Whether this current array's key and value points to a heap memory or not
 * This need to be a global variable
 */
static int kv_malloc = 0;
static void set_kv_malloc(){
    kv_malloc == 1;
}

/**
 * A pair that maps key to the memory address of the value
 * Value is a string, so it is automatically the pointer to it's array
 * of char's memory address.
 * If value is a primitive or a struct then we might need a pointer to it's
 * memory address.
 */
typedef struct KeyValueAddressPair{
    char* key_address;
    char* value_address;
    size_t value_size;
    // if this MR is registered, then there is something going on
    struct MRInfo *mr_rdma;
}KeyValueAddressPair;

/**
 * A struct that contains pointer to the first KeyValueAddressPair
 */
typedef struct KeyValueAddressArray{
    KeyValueAddressPair *head;
    size_t used_length;
    size_t total_length;
}KeyValueAddressArray;

/**
 * Initialize an array of KeyValueAddressPair
 * @param initial_size
 * @return
 */
static KeyValueAddressArray *initialize_KeyValueAddressArray(int initial_size){
    KeyValueAddressArray *my_array = malloc(sizeof(KeyValueAddressArray));
    // freed in free_array
    my_array->head = malloc(sizeof(KeyValueAddressPair) * initial_size);     // freed in free_array

    my_array->total_length = initial_size;
    my_array->used_length = 0;
    return my_array;
}



/**
 * Insert a new KeyValueAddressPair to the current array
 *
 * Behavior:
 * If the key is already in the array, then we will update the key
 * We will copy the information from my_entry to array, so where is my_entry
 * and how long does it live doesn't matter (whether it's dynamic allocated or
 * not).
 * @param my_array
 * @param my_entry
 */
static void insert_to_array(KeyValueAddressArray *my_array, KeyValueAddressPair
*my_entry){
    /// Check if key already exist in array
    for (int i = 0; i < my_array->used_length; i++){
        char *my_key = my_array->head[i].key_address;
        char *insert_key = my_entry->key_address;
        // If the key are the same string (not necessary same address)
        if (strcmp(my_key, insert_key) ==0){
            // if the new key is the same string with the old key, but not
            // the same memory address, we will free the new key
            if (my_key != insert_key){
                my_entry->key_address = my_key;
                if (kv_malloc == 1){
                    free((void*) insert_key);
                    // free the original value address
                    free((void*) my_array->head[i].value_address);
                }

            }
            // copy all the addresses to this key_pair
            my_array->head[i] = *my_entry;
            return;
        }
    }
    /// If exceeds limit, then double the array length
    if (my_array->used_length == my_array->total_length){
        my_array->total_length *= 2;
        // ignore the case if realloc fails: if it fails then mem leak
        my_array->head = realloc(my_array->head, sizeof(KeyValueAddressPair)
        * my_array->total_length);
    }
    /// Insert the entry to my_array
    my_array->head[my_array->used_length] = *my_entry;
    my_array->used_length += 1;
}

/**
 * Found the key, replace it by the last one
 *
 * @param my_array
 * @param key
 */
static void delete_from_array(KeyValueAddressArray
                              *my_array, char *key){
    /// found the key
    int key_index = -1;
    for (int i = 0; i < my_array->used_length - 1; i++){
        char *my_key = my_array->head[i].key_address;
        if (strcmp(my_key, key) ==0){
            key_index = i;
            break;
        }
    }
    if (key_index == -1){ return;}
    /// Free the resource of the key
    if (kv_malloc == 1){
        free((void*) my_array->head[key_index].key_address);
        free((void*) my_array->head[key_index].value_address);
    }
    /// put the last key to the current place
    my_array->head[key_index] = my_array->head[my_array->used_length];
    //move the pointer forward
    my_array->used_length -=1;
    // down size? Waste time, no.
}


/**
 * free the current array including the pointer itself
 * I defined the responsibility to be ours because we malloc the array
 * pointer.
 * @param my_array
 */
static void free_array(KeyValueAddressArray *my_array){
    if (kv_malloc==1){
        for (int i = 0; i < my_array->used_length; i++){
            /// free each malloc key and value address
            free((void*) my_array->head[i].key_address);
            free((void*) my_array->head[i].value_address);
        }
    }

    free(my_array->head);
    free(my_array);
}


/**
 * assign the address of corresponding kv_pair to the given pointer
 * @param my_array
 * @param key
 * @param value_ptr where to strong the head ptr of value
 * @return
 */
static KeyValueAddressPair *get_KeyValueAddressPair(KeyValueAddressArray
*my_array, char *key){
    for (int i = 0; i < my_array->used_length; i++){
        char *my_key = my_array->head[i].key_address;
        if (strcmp(my_key, key) ==0){
            return my_array->head + i;
        }
    }
    // if not found assign NULL
    return NULL;
}

/**
 * Deregister the memory region corresponding to the key-value pair
 * Trust the key exists
 * if the key is correlated to a registered rdma region the deregister it.
 * Else nothing happens
 */
static int deregister_rdma_mr(KeyValueAddressArray *database, char *key){
    /// Get the key value
    KeyValueAddressPair *get_kv_pair = get_KeyValueAddressPair(database,
                                                               key);
    /// Deregister the rdma mr
    struct MRInfo *mr_rdma = get_kv_pair->mr_rdma;
    if (mr_rdma == NULL){
        return 0;
    }
    if (ibv_dereg_mr(mr_rdma->mr)) {
        fprintf(stderr, "Couldn't deregister MR_control_send\n");
        return 1;
    }
    free(mr_rdma);

    /// Update the key_pair status
    get_kv_pair->mr_rdma = NULL;
    return 0;
}

/**
 * For debug, to see the internal state of the array
 * @param my_array
 */
static void print_dynamic_array(KeyValueAddressArray *my_array){
    printf("=============================================================================\n");
    for (int i = 0; i < my_array->used_length; i++) {
        char *my_key = my_array->head[i].key_address;
        char *my_value = my_array->head[i].value_address;
        size_t value_size = my_array->head[i].value_size;

        char* in_rdma_progress;
        if (my_array->head[i].mr_rdma == NULL){
            in_rdma_progress = "FREE";
        }else{
            in_rdma_progress = "USING";
        }
        printf("Entry: %2d, Key: %.6s, value: %-8.8s, value size: %6zu, "
               "state: %6s\n",
               i,
               my_key,
               my_value, value_size, in_rdma_progress);
    }
    printf
    ("=============================================================================\n\n");

}
//int main(){
//    KeyValueAddressPair pair1, pair2, pair3;
//    char *key1 = "key1", *key2 = "key2", *key3 = "key3";
//    char *val1 = "answer1", *val2 = "answer2", *val3 = "answer3";
//
//    pair1.key_address = key1;
//    pair1.value_address = val1;
//
//    pair2.key_address = key2;
//    pair2.value_address = val2;
//
//
//    KeyValueAddressArray *ptr_my_array = initialize_KeyValueAddressArray(1);
//    insert_to_array(ptr_my_array, &pair1);
//    insert_to_array(ptr_my_array, &pair2);
//    printf("%zu", ptr_my_array->total_length);
//    printf("%zu", ptr_my_array->used_length);
//
//
//    char *value;
//    get_value_ptr(ptr_my_array, key1, &value);
//    printf("%s", value);
//}

#endif //NETWORKOPTIMIZATION_DYNAMIC_ARRAY_H
