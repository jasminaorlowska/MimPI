/**
 * This file is for declarations of  common interfaces used in both
 * MIMPI library (mimpi.c) and mimpirun program (mimpirun.c).
 * */

#ifndef MIMPI_COMMON_H
#define MIMPI_COMMON_H

#include <assert.h>
#include <stdbool.h>
#include <stdnoreturn.h>

//macros
#define N_MAX 16
#define MESSAGE_FRAGMENT_SIZE 512
#define STR_FORMAT_SIZE 255

#define MIMPI_WORLD_SIZE "MIMPI_WORLD_SIZE"
#define MIMPI_WORLD_RANK "MIMPI_WORLD_RANK"
#define MIMPI_READ "MIMPI_READ"
#define MIMPI_WRITE "MIMPI_WRITE"
#define MIMPI_RECV_PARENT "MIMPI_RECV_PARENT"
#define MIMPI_RECV_L_CHILD "MIMPI_RECV_L_CHILD"
#define MIMPI_RECV_R_CHILD "MIMPI_RECV_R_CHILD"
#define MIMPI_SEND_PARENT "MIMPI_SEND_PARENT"
#define MIMPI_SEND_L_CHILD "MIMPI_SEND_L_CHILD"
#define MIMPI_SEND_R_CHILD "MIMPI_SEND_R_CHILD"
#define MIMPI_PROC_ROOT_SEND "MIMPI_PROC_ROOT_SEND"
#define MIMPI_PROC_ROOT_RECV "MIMPI_PROC_ROOT_RECV"

/*
    Assert that expression doesn't evaluate to -1 (as almost every system function does in case of error).

    Use as a function, with a semicolon, like: ASSERT_SYS_OK(close(fd));
    (This is implemented with a 'do { ... } while(0)' block so that it can be used between if () and else.)
*/
#define ASSERT_SYS_OK(expr)                                                                \
    do {                                                                                   \
        if ((expr) == -1)                                                                  \
            syserr(                                                                        \
                "system command failed: %s\n\tIn function %s() in %s line %d.\n\tErrno: ", \
                #expr, __func__, __FILE__, __LINE__                                        \
            );                                                                             \
    } while(0)

/* Assert that expression evaluates to zero (otherwise use result as error number, as in pthreads). */
#define ASSERT_ZERO(expr)                                                                  \
    do {                                                                                   \
        int const _errno = (expr);                                                         \
        if (_errno != 0)                                                                   \
            syserr(                                                                        \
                "Failed: %s\n\tIn function %s() in %s line %d.\n\tErrno: ",                \
                #expr, __func__, __FILE__, __LINE__                                        \
            );                                                                             \
    } while(0)

#define ASSERT_ALLOC_OK(alloced_memory) do {                      \
    if ((alloced_memory) == NULL) {                               \
        syserr(                                                   \
            "Memory allocation failed: %s\n\tIn function %s() in %s line %d.\n", \
            #alloced_memory, __func__, __FILE__, __LINE__         \
        );                                                         \
    }                                                             \
} while (0)


/* Prints with information about system error (errno) and quits. */
_Noreturn extern void syserr(const char* fmt, ...);

/* Prints (like printf) and quits. */
_Noreturn extern void fatal(const char* fmt, ...);

#define TODO fatal("UNIMPLEMENTED function %s", __PRETTY_FUNCTION__);


/////////////////////////////////////////////
// Put your declarations here

int get_descriptor_number(int i, bool mimpi_read);

struct message_header {
    int tag;
    int count;
}; typedef struct message_header message_header;

struct message {
    int tag;
    int count;
    void* data;
    struct message* next;
}; typedef struct message message;

struct message* create_message(int tag, int count);
struct message* create_message_2(int tag, int count, void const* data);
void delete_message(struct message * m);
void delete_array(struct message* arr);
void free_multiple(int count, ...);

#endif // MIMPI_COMMON_H