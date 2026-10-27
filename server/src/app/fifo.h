#ifndef FIFO_H
#define FIFO_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_FIFO_LEN 10
#define MIN_FIFO_LEN 3

typedef struct Frame Frame;

struct Frame{
    int image_size;
    int8_t *image;
    Frame *next;
};

typedef struct{
    _Atomic int n;
    Frame *first;
    Frame *last;
} Fifo_frame;

void init_fifo(Fifo_frame*);
void add_frame(Fifo_frame*, int8_t*, int);
void remove_frame(Fifo_frame*);
int get_len(Fifo_frame*);

#endif