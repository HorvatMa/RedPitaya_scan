#include "fifo.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "image.h"

void init_fifo(Fifo_frame *fifo) {
    fifo->n = ATOMIC_VAR_INIT(1);

    Frame *first_frame = malloc(sizeof(Frame));

    first_frame->next = NULL;
    first_frame->image = calloc(MAX_IMAGE_SIZE, sizeof(int8_t));
    first_frame->image_size = MAX_IMAGE_SIZE;

    fifo->first = first_frame;
    fifo->last = first_frame;
}

void add_frame(Fifo_frame *fifo, int8_t *image, int img_size) {
    printf("Adding frame!\n");
    int len = get_len(fifo);
    printf("Len = %d\n", len);

    if (len > MAX_FIFO_LEN){
        printf("Dropping frames!\n");
        Frame *new_frame = fifo->last;

        free(new_frame->image);
        new_frame->image = malloc(sizeof(int8_t)*img_size);
        memcpy(new_frame->image, image, img_size*sizeof(int8_t));
        new_frame->image_size = img_size;
    }
    else{
        Frame *new_frame = malloc(sizeof(Frame));
        new_frame->next = NULL;

        new_frame->image = malloc(sizeof(int8_t)*img_size);
        memcpy(new_frame->image, image, img_size*sizeof(int8_t));
        new_frame->image_size = img_size;

        fifo->last->next = new_frame;
        fifo->last = new_frame;
    
        atomic_fetch_add(&fifo->n, 1);
    }
}

void remove_frame(Fifo_frame *fifo) {
    Frame *first = fifo->first->next;
    free(fifo->first->image);
    free(fifo->first);
    fifo->first = first; 

    atomic_fetch_sub(&fifo->n, 1);
}

int get_len(Fifo_frame *fifo) {
    return atomic_load(&fifo->n);
}