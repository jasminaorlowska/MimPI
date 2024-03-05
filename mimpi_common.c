/**
 * This file is for implementation of common interfaces used in both
 * MIMPI library (mimpi.c) and mimpirun program (mimpirun.c).
 * */

#include "mimpi_common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


_Noreturn void syserr(const char* fmt, ...)
{
    va_list fmt_args;

    fprintf(stderr, "ERROR: ");

    va_start(fmt_args, fmt);
    vfprintf(stderr, fmt, fmt_args);
    va_end(fmt_args);
    fprintf(stderr, " (%d; %s)\n", errno, strerror(errno));
    exit(1);
}

_Noreturn void fatal(const char* fmt, ...)
{
    va_list fmt_args;

    fprintf(stderr, "ERROR: ");

    va_start(fmt_args, fmt);
    vfprintf(stderr, fmt, fmt_args);
    va_end(fmt_args);

    fprintf(stderr, "\n");
    exit(1);
}

/////////////////////////////////////////////////
// Put your implementation here

struct message* create_message(int tag, int count) {
    if (count < 0) return NULL;
    struct message* new_message = calloc(1, sizeof(struct message));
    new_message->count = count;
    new_message->tag = tag;
    new_message->data = calloc(count, 1);
    return new_message;
}
void delete_message(struct message* m) {
    free(m->data);
    free(m);
    return;
}

void delete_array(struct message* m) {
    if (m == NULL) return;
    struct message* next = NULL;
    while (m != NULL) {
        next = m->next;
        delete_message(m);
        m=next;
    }
}

int get_descriptor_number(int i, bool mimpi_read) {
    char dsc_no_char[256];
    if (mimpi_read) sprintf(dsc_no_char, "%s_%d", MIMPI_READ, i);
    else sprintf(dsc_no_char, "%s_%d", MIMPI_WRITE, i);
    char *curr_read_descriptor = getenv(dsc_no_char);
    int curr_read_dsc_no = atoi(curr_read_descriptor);
    return curr_read_dsc_no;
}

void free_multiple(int count, ...) {
    va_list args;
    va_start(args, count);

    for (int i = 0; i < count; i++) {
        void* ptr = va_arg(args, void*);
        free(ptr);
    }

    va_end(args);
}
