#include <string.h>
#include <stdio.h>
#include "transfer.h"
#include <pthread.h>
#include "image.h"
#include <time.h>
#include "fifo.h"
#include "common.h"
// #include <device_interface.h>

#define SLEEP_TIME 10000
#define BUFF_LEN 100
#define MAX_BUFF_LEN 16385

void send_data_tcp(int len, int connfd, char *data) {
    char buff[MAX];

    bzero(buff, MAX); 
    sprintf(buff, "%0*d", 7, len);
    write(connfd, buff, 7);  

    write(connfd, data, len);
}

// void send_frame_tcp

void *do_work(void *vargp) {
    Worker_args *worker_args;
    worker_args = (Worker_args *) vargp;
    int connfd = worker_args->connfd;
    Acq *acq = worker_args->acq;
    Fifo_frame *fifo = worker_args->fifo;
    Img_params *im_par = worker_args->im_par;

    int16_t x_lower = im_par->x_lower;
    int16_t x_upper = im_par->x_upper;
    int16_t y_lower = im_par->y_lower;
    int16_t y_upper = im_par->y_upper;
    size_t x_res = im_par->x_res;
    size_t y_res = im_par->y_res;
    uint16_t frame_time = im_par->frame_time;

    size_t img_size = x_res*y_res;

    // Buff_data *buffer;
    // buffer = malloc(sizeof(Buff_data)*MAX_BUFF_LEN);

    char *data[MAX_DATA];

    double* image = NULL;
    int* count = NULL;
    double elapsed;
    size_t len;
    char *buff[BUFF_LEN];

    uint8_t *rescaled_image = calloc(MAX_IMAGE_SIZE, sizeof(int8_t));

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (wait(acq) == 0) {
        // clock_gettime(CLOCK_MONOTONIC, &ts_end);
        // double elapsed = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
        //                 (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
        // printf("Elapsed: %.2f ms\n", elapsed);
        // printf("interrupted!\n");

        // printf("%d \n", acq->reg->buff_n);
        // len = acq->reg->buff_n;
        // memcpy(buffer, acq->buff, len);
        // request_and_clear_interrupt(acq);
        // clear_interrupt(acq);
        // update_or_create_image_from_struct(buffer, len,
        //     x_lower, x_upper, y_lower, y_upper, x_res, y_res, &image, &count);

        // update_or_create_image_from_struct(acq->buff, acq->reg->buff_n,
        //     x_lower, x_upper, y_lower, y_upper, x_res, y_res, &image, &count);

        // if (elapsed > frame_time){
        //     // printf("Triggered\n");
        //     printf("Elapsed: %.2f ms\n", elapsed);
        //     clock_gettime(CLOCK_MONOTONIC, &ts_start);
        //     rescale_image_to_int8(image, rescaled_image, img_size);
        //     add_frame(fifo, rescaled_image, img_size);
        //     free(image);
        //     free(count);
        //     image = NULL;
        //     count = NULL;
        // }

        // request_interrupt(acq);

        len = acq->reg->buff_n*8;

        // bzero(buff, BUFF_LEN); 
        // sprintf(buff, "%0*d", 10, len);
        // // printf(buff);
        // write(connfd, buff, 10);  

        // printf("Writing\n");
        write(connfd, acq->buff, len);
        request_and_clear_interrupt(acq);
    }

    free(rescaled_image);
    // free(buffer);

    if (!image) {
        free(image);
        free(count);
    }

    return NULL;
}

void transfer(Acq *acq, uint16_t N) {
    memcpy(acq->data_end, acq->buff, N*8);
    printf("N = %d\n", N);
    acq->data_end += N;

    // for (int i=0; i<N; i++){
    //     printf("osc1: %d, osc2: %d, integrator: %d\n", 
    //         acq->data_start[i].osc1, acq->data_start[i].osc2, acq->data_start[i].integrator);
    // }
}

void *tcp_worker(void *vargp){
    const int msec = 100;
    struct timespec ts;
    char buff[BUFF_LEN];
    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    Worker_args *worker_args;
    worker_args = (Worker_args *) vargp;

    Acq *acq = worker_args->acq;
    int connfd = worker_args->connfd;

    Fifo_frame *fifo = worker_args->fifo;

    while(acq->active) {
        if (get_len(fifo) < MIN_FIFO_LEN) {
            nanosleep(&ts, &ts);
            continue;
        }
        
        send_image_tcp(connfd, fifo->first->image, fifo->first->image_size, buff);
        remove_frame(fifo);
    }
}

void send_image_tcp(int connfd, int8_t *img, size_t len, char *buff){
    printf("Sending image!\n");
    bzero(buff, BUFF_LEN); 
    sprintf(buff, "%0*d", 10, len);
    write(connfd, buff, 10);  

    int8_t maxValue = img[0]; // Start with the first element as the maximum
    for (size_t i = 1; i < len; ++i) {
        if (img[i] > maxValue) {
            maxValue = img[i]; // Update the maximum if a larger value is found
        }
    }

    // printf("maxValue = %d\n", maxValue);

    write(connfd, img, len);
}