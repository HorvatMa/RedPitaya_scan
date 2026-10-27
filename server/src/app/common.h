#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include "fifo.h"
#include "image.h"
#include <pthread.h>

#define MAX_DATA 1000000

typedef struct {
    Acq *acq;
    int connfd;
    Fifo_frame *fifo;
    Img_params *im_par;
    pthread_t *thread_id;
} Worker_args;

#endif // COMMON_H
