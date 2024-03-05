/** This file is for implementation of MIMPI library.**/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include "channel.h"
#include "mimpi.h"
#include "mimpi_common.h"

int n = 0; 
int RANK = -1; 

pthread_t* helper_threads;
bool* process_ended;
bool* waiting_for_recv; 
pthread_mutex_t *mut = NULL;
pthread_cond_t cond_message_received;

//Buffer - messages to read.
struct message* messages_received_front[N_MAX];
struct message* messages_received_rear[N_MAX];

//Point-to-point communication descriptors
int* read_descriptors; //Read_descriptoras[i] - read-end from process 'i'.
int* write_descriptors; //Write_descriptors[i] is the write-end descriptor for process 'i'.

//Collective communication descriptors
int parent = 0, l_child = 1, r_child = 2;
int size_collective_descriptors_arr = 3; //[0] - parent, [1] - l_child, [2] - r_child.
int* read_descriptors_collective; 
int* write_descriptors_collective; 

bool is_process_alive(int rank) {
    message_header header;
    header.tag = -1;
    ssize_t bytes_sent = chsend(write_descriptors[rank], &header, sizeof(message_header));
    if (bytes_sent == -1 && errno == EPIPE) return false;
    return true;
}

//Returns true if read full data, false otherwise (error in reading). 
bool read_exactly (int read_dsc, void* save_to, size_t count) {
    size_t left_to_read = count;
    size_t already_read = 0;
    bool finished_reading = false; 

    while (!finished_reading) {
        size_t should_read = left_to_read >= MESSAGE_FRAGMENT_SIZE ? MESSAGE_FRAGMENT_SIZE : left_to_read;
        ssize_t bytes_read = chrecv(read_dsc, save_to + already_read, should_read);
        if ((bytes_read == 0) || errno == EBADF) {
            //Broken pipe.
            return false;
        }
        ASSERT_SYS_OK(bytes_read);
        already_read += (size_t) bytes_read;
        left_to_read -= (size_t) bytes_read;
        if (left_to_read == 0) finished_reading = true;
    }

    return true;
}

//Returns true if write full data, false otherwise (error in writing). 
bool send_exactly (int write_dsc, void* write_from, size_t count) {
    size_t left_to_send = count;
    size_t already_sent = 0;
    bool finished_sending = false;

    while (!finished_sending) {

        size_t should_send = left_to_send >= MESSAGE_FRAGMENT_SIZE ? MESSAGE_FRAGMENT_SIZE : left_to_send;
        ssize_t bytes_sent = chsend(write_dsc, write_from + already_sent, should_send);
        if (bytes_sent == -1 && errno == EPIPE) {
                return false;
        }
        ASSERT_SYS_OK(bytes_sent);
        left_to_send -= (size_t) bytes_sent;
        already_sent += (size_t) bytes_sent;

        if (left_to_send == 0) finished_sending = true;
    }

    return true;
}

void* receive_messages(void* data) {
    int rank = *((int*)data);
    free(data);

    if (rank == RANK) {
        //Finish when rank == process rank - process can't write to itself.
        return NULL;
    }

    //Reading messages loop. 
    while (true) {
        message_header* received_msg_header = malloc(sizeof(message_header)); 
        ASSERT_ALLOC_OK(received_msg_header);
        
        ssize_t bytes_read = chrecv(read_descriptors[rank], received_msg_header, sizeof(message_header));

        //Broken pipe. Other process or my pipe had been closed.
        if (bytes_read == 0 || errno == EBADF) {
            free(received_msg_header);
            ASSERT_ZERO(pthread_mutex_lock(&mut[rank]));
            process_ended[rank] = true;
            ASSERT_ZERO(pthread_mutex_unlock(&mut[rank]));
            //Signaling main process - let him check the messages.
            ASSERT_ZERO(pthread_cond_signal(&cond_message_received));
            return NULL;
        }

        //Tag could be equal to -1 : then ignore. 
        else if (received_msg_header->tag  > 0) {
            //OK - reading message.
            ASSERT_ZERO(pthread_mutex_lock(&mut[rank]));

            //Creating message.
            message* m = create_message(received_msg_header->tag, received_msg_header->count);
            free(received_msg_header);

            //Reading the message. 
            bool read_ok = read_exactly(read_descriptors[rank], m->data, (size_t) m->count);
            if (!read_ok) {
                process_ended[rank] = true;
                ASSERT_ZERO(pthread_mutex_unlock(&mut[rank]));
                delete_message(m);
                ASSERT_ZERO(pthread_cond_signal(&cond_message_received));
                return NULL;
            }

            // Add message to the buffer.
            messages_received_rear[rank]->next = m;
            messages_received_rear[rank] = m;

            //If should signal main process - it might be waiting for this message. 
            bool should_signal = (waiting_for_recv[rank]== true);
            ASSERT_ZERO(pthread_mutex_unlock(&mut[rank]));

            if (should_signal) ASSERT_ZERO(pthread_cond_signal(&cond_message_received));
        }
    }

    return NULL;
}

MIMPI_Retcode MIMPI_Send(void const *data, int count, int destination, int tag) {
    //Wrong arguments handling.
    if (destination < 0 || destination >= n) return MIMPI_ERROR_NO_SUCH_RANK;
    if (destination == RANK) return MIMPI_ERROR_ATTEMPTED_SELF_OP;
    if (tag == 0 || count < 0) return MIMPI_SUCCESS;

    //Send header.
    message_header header; 
    header.count = count;
    header.tag = tag;
    if (chsend(write_descriptors[destination], &header, sizeof(header)) == -1 && errno == EPIPE) {
        return MIMPI_ERROR_REMOTE_FINISHED;
    }

    void* data_to_write = calloc(1, (size_t)count); 
    ASSERT_ALLOC_OK(data_to_write);
    memcpy(data_to_write, data, count); 

    //Send message.
    bool write_ok = send_exactly(write_descriptors[destination], data_to_write, (size_t) count);

    free(data_to_write);

    if (!write_ok) return MIMPI_ERROR_REMOTE_FINISHED;
    return MIMPI_SUCCESS;
}

//Find message from process ranked "source" that is count-sized and fits with tag. 
struct message* find_message(int source, int tag, int count) {
    if (tag < 0 || count < 0) return NULL;
    struct message* m = NULL;
    struct message* curr = messages_received_front[source];
    bool message_found = false;
    while (!message_found) {
        if (curr == NULL) message_found = true;
        else if (curr->count == count && ((tag == 0) || (curr->tag == tag))) {
            m = curr;
            message_found = true;
        }
        else curr = curr->next;
    }
    return m;
}

MIMPI_Retcode MIMPI_Recv( void *data, int count, int source, int tag) {
    if (source == RANK) return MIMPI_ERROR_ATTEMPTED_SELF_OP;
    if (source < 0 || source >= n) return MIMPI_ERROR_NO_SUCH_RANK;

    //Received message. 
    struct message* m = NULL;

    ASSERT_ZERO(pthread_mutex_lock(&mut[source]));
    waiting_for_recv[source] = true;
    m = find_message(source, tag, count);

    //Waiting for message to be sent.
    while (m == NULL && !process_ended[source]) {
        pthread_cond_wait(&cond_message_received, &mut[source]);
        m = find_message(source, tag, count);
        if (m == NULL && process_ended[source]) {
            break;
        }
    }
    waiting_for_recv[source] = false;

    //Message had not been received.
    if (m == NULL) {
        ASSERT_ZERO(pthread_mutex_unlock(&mut[source]));
        return MIMPI_ERROR_REMOTE_FINISHED;
    }

    //Message received.

    //Copying the data.
    memcpy(data, m->data, count);

    //Removing the message from buffer.
    struct message* curr = messages_received_front[source];
    while (curr != NULL && curr->next != m) curr = curr->next;
    if (curr == NULL) printf("ERROR in MIMPI_Recv. Message had been deleted, while process was waiting.\n");
    else curr->next = m->next;
    if (messages_received_rear[source] == m) messages_received_rear[source] = curr;
    ASSERT_ZERO(pthread_mutex_unlock(&mut[source]));
    
    //Deleting message - it has been copied to data.
    delete_message(m);
    return MIMPI_SUCCESS;
}

void send_to_children(bool have_left_child, bool have_right_child, void* message, size_t count) {
    if (have_left_child) send_exactly(write_descriptors_collective[l_child], message, count);
    if (have_right_child) send_exactly(write_descriptors_collective[r_child], message, count);
}

MIMPI_Retcode MIMPI_Barrier() {
    //Processes communicate with each other using int numbers. 
    int error_barrier = 1, succes_barrier = 0;
    size_t block_count = sizeof (int);

    int result_left_child = succes_barrier, result_right_child = succes_barrier;
    bool have_right_child = (2 * (RANK + 1)) < n;
    bool have_left_child = ((2 * RANK) + 1) < n; 

    //Waiting for children.
    if (have_left_child) {
        ssize_t bytes_read_left_child = chrecv(read_descriptors_collective[l_child], &result_left_child, block_count);
        if (bytes_read_left_child == 0 || result_left_child == error_barrier) result_left_child = error_barrier;
    }
    if (have_right_child) {
        ssize_t bytes_read_right_child = chrecv(read_descriptors_collective[r_child], &result_right_child, block_count);
        if (bytes_read_right_child == 0 || result_right_child == error_barrier) result_right_child = error_barrier;
    }

    //Error from at least one child.
    if (result_left_child == error_barrier || result_right_child == error_barrier) {
        send_to_children(have_left_child, have_right_child, &error_barrier, block_count);
        chsend(write_descriptors_collective[parent], &error_barrier, block_count);
        return MIMPI_ERROR_REMOTE_FINISHED;
    }

    //if RANK == 0 then don't wait for the parent. 
    if (RANK != 0) {
        //Children are waiting. sending signal to my parent.

        if (chsend(write_descriptors_collective[parent], &succes_barrier, block_count) == -1) {
            //Error from parent.
            send_to_children(have_left_child, have_right_child, &error_barrier, block_count);
            return MIMPI_ERROR_REMOTE_FINISHED;
        }
        
        //Waiting for a signal from parent. 
        int result_parent = 0;
        ssize_t bytes_read_parent = chrecv(read_descriptors_collective[parent], &result_parent, block_count);
        if (bytes_read_parent == 0 || result_parent == error_barrier) {
            //Error from parent.
            send_to_children(have_left_child, have_right_child, &error_barrier, block_count);
            return MIMPI_ERROR_REMOTE_FINISHED;
        }
    }
    
    //Sending success signal to my children. 
    send_to_children(have_left_child, have_right_child, &succes_barrier, block_count);

    return MIMPI_SUCCESS;
}
MIMPI_Retcode MIMPI_Bcast(void *data, int count, int root) {
    if (root >= n || root < 0) return MIMPI_ERROR_NO_SUCH_RANK;
    if (count < 0) return MIMPI_SUCCESS;

    size_t block_size = sizeof(int);
    size_t new_count = ((size_t) count) + block_size;

    int error_bcast = 1, success_bcast = 0, success_bcast_with_result = 2;
    void* error_bcast_msg = calloc(1, new_count);
    ASSERT_ALLOC_OK(error_bcast_msg);
    memcpy(error_bcast_msg, &error_bcast, block_size);

    void* result_left_child = calloc(1, new_count);
    ASSERT_ALLOC_OK(result_left_child);
    void* result_right_child = calloc(1, new_count);
    ASSERT_ALLOC_OK(result_right_child);
    int left_child_ok = success_bcast;
    int right_child_ok = success_bcast; 
    bool have_right_child = (2 * (RANK + 1)) < n;
    bool have_left_child = ((2 * RANK) + 1) < n; 

    //Waiting for children.
    if (have_left_child) {
        bool read_ok = read_exactly(read_descriptors_collective[l_child], result_left_child, new_count);
        if (!read_ok) left_child_ok = error_bcast;
        else left_child_ok = *(int*) result_left_child; 
    }
    if (have_right_child) {
        bool read_ok = read_exactly(read_descriptors_collective[r_child], result_right_child, new_count);
        if (!read_ok) right_child_ok = error_bcast;
        else right_child_ok = *(int*) result_right_child;
    }

    //Error from at least one child.
    if (left_child_ok == error_bcast || right_child_ok == error_bcast) {
        send_to_children(have_left_child, have_right_child, error_bcast_msg, new_count);
        send_exactly(write_descriptors_collective[parent], error_bcast_msg, new_count);
        free_multiple(3, error_bcast_msg, result_left_child, result_right_child);
        return MIMPI_ERROR_REMOTE_FINISHED;
    }

    void* data_to_send = calloc(1, new_count);
    ASSERT_ALLOC_OK(data_to_send); 
    memcpy(data_to_send, &success_bcast, block_size);

    if (RANK == root) {
        memcpy(data_to_send, &success_bcast_with_result, block_size);
        memcpy(data_to_send + block_size, data, count); 
    }   
    else {
        if (have_left_child && *(int*)result_left_child == success_bcast_with_result) {
            memcpy(data_to_send, result_left_child, new_count);
        }
        else if (have_right_child && *(int*)result_right_child == success_bcast_with_result) {
            memcpy(data_to_send, result_right_child, new_count);
        }
        else if (RANK == 0) printf("There is a problem. PROCESS 0 and I got no message.\n");
    }

    //if RANK != 0 then wait for the parent. 
    if (RANK != 0) {
        //Children are waiting. Sending signal to my parent.
 
        bool send_to_parent_ok = send_exactly(write_descriptors_collective[parent], data_to_send, new_count);
        if (!send_to_parent_ok) {
            //Error from parent.
            send_to_children(have_left_child, have_right_child, error_bcast_msg, new_count);
            free_multiple(4, error_bcast_msg, result_left_child, result_right_child, data_to_send);
            return MIMPI_ERROR_REMOTE_FINISHED;
        }

        //Waiting for a signal from parent. 
        void* result_parent = calloc(1, new_count);
        ASSERT_ALLOC_OK(result_parent);
        bool read_from_parent_ok = read_exactly(read_descriptors_collective[parent], result_parent, new_count);
        if (!read_from_parent_ok || *(int*)result_parent == error_bcast) {
            //Error from parent. 
            send_to_children(have_left_child, have_right_child, error_bcast_msg, new_count);
            free_multiple(5, error_bcast_msg, result_left_child, result_right_child, data_to_send, result_parent);
            return MIMPI_ERROR_REMOTE_FINISHED;
        }
        memcpy(data_to_send, result_parent, new_count);
        free(result_parent);
    }

    //Sending success signal to my children. 
    send_to_children(have_left_child, have_right_child, data_to_send, new_count);
    memcpy(data, data_to_send + sizeof(int), count);

    free_multiple(4, error_bcast_msg, result_left_child, result_right_child, data_to_send);
    
    return MIMPI_SUCCESS;
}

MIMPI_Retcode MIMPI_Reduce( void const *send_data, void *recv_data, int count, MIMPI_Op op, int root) {
    if (root >= n || root < 0) return MIMPI_ERROR_NO_SUCH_RANK;
    if (count < 0) return MIMPI_SUCCESS;
    size_t block_size = sizeof(uint8_t);
    size_t new_count = count + block_size;

    uint8_t error_reduce = 1, success_reduce = 0;
    uint8_t* error_reduce_msg = calloc(1, new_count);
    ASSERT_ALLOC_OK(error_reduce_msg);
    memcpy(error_reduce_msg, &error_reduce, block_size);

    uint8_t* result_left_child = calloc(1, new_count);
    ASSERT_ALLOC_OK(result_left_child); 
    uint8_t* result_right_child = calloc(1, new_count);
    ASSERT_ALLOC_OK(result_right_child);
    uint8_t left_child_ok = success_reduce;
    uint8_t right_child_ok = success_reduce;
    bool have_right_child = (2 * (RANK + 1)) < n;
    bool have_left_child = ((2 * RANK) + 1) < n; 

    //Waiting for children.
    if (have_left_child) {
        int read_ok = read_exactly(read_descriptors_collective[l_child], result_left_child, new_count);
        if (!read_ok) left_child_ok = error_reduce;
        else left_child_ok = *((uint8_t*)result_left_child); //conversion to uint8_t and dereferencing. 
    }
    if (have_right_child) {
        int read_ok = read_exactly(read_descriptors_collective[r_child], result_right_child, new_count);
        if (!read_ok) right_child_ok = error_reduce;
        else right_child_ok = *((uint8_t*)result_right_child);
    }

    //If children sent error message or had already ended.
    if (left_child_ok == error_reduce || right_child_ok == error_reduce) {
        send_to_children(have_left_child, have_right_child, error_reduce_msg, new_count);
        send_exactly(write_descriptors_collective[parent],  error_reduce_msg, new_count);
        free_multiple(3, result_left_child, result_right_child, error_reduce_msg);
        return MIMPI_ERROR_REMOTE_FINISHED;
    }

    uint8_t* data_to_send = calloc(1, new_count);
    ASSERT_ALLOC_OK(data_to_send);
    memcpy(data_to_send, &success_reduce, block_size);
    memcpy(data_to_send + block_size, send_data, count);

    for (int i = 1; i <= count; i++) {
        size_t idx = i * block_size; 
        uint8_t x = have_left_child ? *(result_left_child + idx) : 0;
        uint8_t y = have_right_child ? *(result_right_child + idx) : 0;
        uint8_t data_to_send_curr_value = *(data_to_send + idx);
        uint8_t res = 0;

        switch (op) {
        case MIMPI_MAX:
            res = x > y ? x : y;
            *(data_to_send + idx) = data_to_send_curr_value > res ? data_to_send_curr_value : res;
            break;
        case MIMPI_MIN:
            if (!have_left_child) x = UINT8_MAX;
            if (!have_right_child) y = UINT8_MAX;
            res = x < y ? x : y;
            *(data_to_send + idx) = data_to_send_curr_value < res ? data_to_send_curr_value : res;
            break;
        case MIMPI_SUM:
            res = x + y; //Overflow possible
            *(data_to_send + idx) = res + data_to_send_curr_value;
            break;
        case MIMPI_PROD:
            if (!have_left_child) x = 1; //1 does not change anything in multiplying
            if (!have_right_child) y = 1;
            res = x * y; //Overflow possible
            *(data_to_send + idx) = res * data_to_send_curr_value;
            break;
        default:
            printf("Error. No such MIMPI_Op: %d\n", op);
            free_multiple(4, result_left_child, result_right_child, error_reduce_msg, data_to_send);
            return MIMPI_SUCCESS;
        }
    }

    //if RANK != 0 then send a message & then wait for the parent. 
    if (RANK != 0) {
        //Sending a message to parent.

        bool write_to_parent_ok = send_exactly(write_descriptors_collective[parent], data_to_send, new_count);
        if (!write_to_parent_ok) {
            //Parent ended.
            send_to_children(have_left_child, have_right_child, error_reduce_msg, new_count);
            free_multiple(4, result_left_child, result_right_child, error_reduce_msg, data_to_send);
            return MIMPI_ERROR_REMOTE_FINISHED;
        }

        //No error from parent. Waiting for a signal from parent. 
        uint8_t* result_parent = calloc(1, new_count);
        ASSERT_ALLOC_OK(result_parent);
        bool read_from_parent_ok = read_exactly(read_descriptors_collective[parent], result_parent, new_count);
        if (!read_from_parent_ok || *(uint8_t*)result_parent == error_reduce) {
            send_to_children(have_left_child, have_right_child, error_reduce_msg, new_count);
            free_multiple(5, result_left_child, result_right_child, error_reduce_msg, data_to_send, result_parent);
            return MIMPI_ERROR_REMOTE_FINISHED;
        }

        //Copying the reduced data.
        memcpy(data_to_send, result_parent, new_count);

        free(result_parent);
    }
    
    if (RANK == root) memcpy(recv_data, data_to_send + block_size, count); 

    //Sending success signal to my children. 
    send_to_children(have_left_child, have_right_child, data_to_send, new_count);

    free_multiple(4, result_left_child, result_right_child, error_reduce_msg, data_to_send);
    return MIMPI_SUCCESS;
}

int get_env_variable(const char* env_variable_name) {
    char *value = getenv(env_variable_name);
    if (value != NULL) {
        int value_int = atoi(value);
        return value_int;
    }
    else {
        fprintf(stderr, "Error in copying env variables %s non existent!\n", env_variable_name);
        return -1;
    }
}

void MIMPI_Init(bool enable_deadlock_detection) {
    channels_init();
    n = MIMPI_World_size();
    RANK = MIMPI_World_rank();

    //Allocate memory
    read_descriptors = malloc(sizeof(int) * n); 
    ASSERT_ALLOC_OK(read_descriptors);
    write_descriptors = malloc(sizeof(int) * n); 
    ASSERT_ALLOC_OK(write_descriptors);
    read_descriptors_collective = malloc(sizeof(int) * size_collective_descriptors_arr); 
    ASSERT_ALLOC_OK(read_descriptors_collective);
    write_descriptors_collective = malloc(sizeof(int) * size_collective_descriptors_arr); 
    ASSERT_ALLOC_OK(write_descriptors_collective);
    helper_threads = malloc(sizeof(pthread_t) * n); 
    ASSERT_ALLOC_OK(helper_threads);
    process_ended = malloc(sizeof(bool) * n); 
    ASSERT_ALLOC_OK(process_ended);
    waiting_for_recv = malloc(sizeof(bool) * n); 
    ASSERT_ALLOC_OK(waiting_for_recv);

    //Initialize read, write descriptors.
    for (int i = 0; i < n; i++) read_descriptors[i] = get_descriptor_number(i, true);
    for (int i = 0; i < n; i++) write_descriptors[i] = get_descriptor_number(i, false);

    read_descriptors_collective[0] = get_env_variable(MIMPI_RECV_PARENT);
    read_descriptors_collective[1] = get_env_variable(MIMPI_RECV_L_CHILD);
    read_descriptors_collective[2] = get_env_variable(MIMPI_RECV_R_CHILD);
    write_descriptors_collective[0] = get_env_variable(MIMPI_SEND_PARENT);
    write_descriptors_collective[1] = get_env_variable(MIMPI_SEND_L_CHILD);
    write_descriptors_collective[2] = get_env_variable(MIMPI_SEND_R_CHILD);

    //Allocate memory and fill array-data for receiving buffers.
    for (int i = 0; i < N_MAX; i++) {
        messages_received_front[i] = create_message(-1, 0);
        messages_received_rear[i] = messages_received_front[i];
    }

    //Creating global mutex and condition variable. 
    ASSERT_ZERO(pthread_cond_init(&cond_message_received, NULL));

    //Creating threads.
    //Mutex array for threads.
    mut = malloc(n * sizeof(pthread_mutex_t));
    ASSERT_ALLOC_OK(mut);

    //Arrays for thread's usage.
    for (int i = 0; i < n; i++) {
        process_ended[i] = false;
        waiting_for_recv[i] = false;
    }
    //Attributes
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    for(int rank = 0; rank < n; rank++) {
        //thread_arg = rank
        int* thread_arg = malloc(sizeof(int));
        *thread_arg = rank;
        ASSERT_ZERO(pthread_mutex_init(&mut[rank], NULL));
        ASSERT_ZERO(pthread_create(&helper_threads[rank], &attr, receive_messages, thread_arg));
    }
    pthread_attr_destroy(&attr);
}

void MIMPI_Finalize() {
    int n = MIMPI_World_size();

    //Close pipes: point-to-point, collective & process root. 
    for (int i = 0; i < n; ++i) {
        ASSERT_SYS_OK(close(write_descriptors[i]));
        ASSERT_SYS_OK(close(read_descriptors[i]));
    }
    for (int i = 0; i < size_collective_descriptors_arr; i++) {
        if (read_descriptors_collective[i] != 0) ASSERT_SYS_OK(close(read_descriptors_collective[i]));
        if (write_descriptors_collective[i] != 0) ASSERT_SYS_OK(close(write_descriptors_collective[i]));
    }
    
    //Waiting for helper threads to end.
    for (int i = 0; i < n; i++) pthread_join(helper_threads[i], NULL);
    //Destroying mutexes after threads finalize (for safety reasons).
    for (int i = 0; i < n; i++) ASSERT_ZERO(pthread_mutex_destroy(&mut[i]));
    free(mut);
    //Condition variable destroy. 
    ASSERT_ZERO(pthread_cond_destroy(&cond_message_received));

    //Free memory.
    free_multiple(7, read_descriptors, write_descriptors, read_descriptors_collective, write_descriptors_collective,
                     helper_threads, process_ended, waiting_for_recv );

    //Delete message buffer.
    for (int i = 0; i < N_MAX; ++i) delete_array(messages_received_front[i]);

    channels_finalize();
}

int MIMPI_World_size() {
    return get_env_variable(MIMPI_WORLD_SIZE);
}

int MIMPI_World_rank() {
    return get_env_variable(MIMPI_WORLD_RANK);
}
