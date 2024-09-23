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
 * A pair that maps key to the memory address of the value
 * Value is a string, so it is automatically the pointer to it's array
 * of char's memory address.
 * If value is a primitive or a struct then we might need a pointer to it's
 * memory address.
 */
typedef struct KeyValueAddressPair{
    char* key_address;
    char* value_address;
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
KeyValueAddressArray *initialize_KeyValueAddressArray(int initial_size){
    KeyValueAddressArray *my_array = malloc(sizeof(KeyValueAddressArray));
    my_array->head = malloc(sizeof(KeyValueAddressPair) * initial_size);
    my_array->total_length = initial_size;
    my_array->used_length = 0;
    return my_array;
}



/**
 * Insert a new KeyValueAddressPair to the current array
 *
 * Behavior:
 * If the key is already in the array, then we will update the key
 * @param my_array
 * @param my_entry
 */
void insert_array(KeyValueAddressArray *my_array, KeyValueAddressPair
*my_entry){
    // if the key is already in the array, then we update the key
    for (int i = 0; i < my_array->used_length; i++){
        char *my_key = my_array->head[i].key_address;
        char *insert_key = my_entry->key_address;
        if (strcmp(my_key, insert_key) ==0){
            my_array->head[i].value_address = my_entry->value_address;
            return;
        }
    }
    if (my_array->used_length == my_array->total_length){
        my_array->total_length *= 2;
        // ignore the case if realloc fails: if it fails then mem leak
        my_array->head = realloc(my_array->head, sizeof(KeyValueAddressPair)
        * my_array->total_length);
    }
    // This will copy addresses to my_array->head
    my_array->head[my_array->used_length] = *my_entry;
    my_array->used_length += 1;
}

/**
 * free the current array including the pointer itself
 * I defined the responsibility to be ours because we malloc the array
 * pointer.
 * @param my_array
 */
void free_array(KeyValueAddressArray *my_array){
    free(my_array->head);
    free(my_array);
}


/**
 * Assign the pointer to string the address of the string
 * @param my_array
 * @param key
 * @param value_ptr where to strong the head ptr of value
 * @return
 */
void get_value_ptr(KeyValueAddressArray *my_array, char *key, char
**value_ptr){
    for (int i = 0; i < my_array->used_length; i++){
        char *my_key = my_array->head[i].key_address;
        if (strcmp(my_key, key) ==0){
            *value_ptr = my_array->head[i].value_address;
            return;
        }
    }
    // if not found assign NULL
    *value_ptr = NULL;
}

int main(){
    KeyValueAddressPair pair1, pair2, pair3;
    char *key1 = "key1", *key2 = "key2", *key3 = "key3";
    char *val1 = "answer1", *val2 = "answer2", *val3 = "answer3";

    pair1.key_address = key1;
    pair1.value_address = val1;

    pair2.key_address = key2;
    pair2.value_address = val2;


    KeyValueAddressArray *ptr_my_array = initialize_KeyValueAddressArray(1);
    insert_array(ptr_my_array, &pair1);
    insert_array(ptr_my_array, &pair2);
    printf("%zu", ptr_my_array->total_length);
    printf("%zu", ptr_my_array->used_length);


    char *value;
    get_value_ptr(ptr_my_array, key1, &value);
    printf("%s", value);
}

#endif //NETWORKOPTIMIZATION_DYNAMIC_ARRAY_H
