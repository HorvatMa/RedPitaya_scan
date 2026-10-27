#ifndef TRANSFER_H
#define TRANSFER_H

#include <stdint.h>
#include "oscilloscope.h"
#include "device_interface.h"
#include "fifo.h"
#include "image.h"
#include <pthread.h>

#define MAX_DATA 1000000

void *do_work(void *);
void *tcp_worker(void *);
void transfer(Acq *, uint16_t);
void send_data_tcp(int, int, char*);
void send_image_tcp(int, int8_t*, size_t, char*);

#endif