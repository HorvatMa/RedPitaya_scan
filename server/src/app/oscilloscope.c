#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include "oscilloscope.h"
#include "device_helper.h"
#include <poll.h>
#include <errno.h>

Acq *create_acq(Uio uio) {
    char *path = malloc(sizeof(char[100]));
    snprintf(path, 100, "%s%s", "/dev/", uio.name);

    printf("%s\n", path);
    
    int fd = open(path, O_RDWR);

    Reg_map_osc *reg = memory_map(fd, uio.maps->uio_map.size, 0);
    uint16_t *buff = memory_map(fd, uio.maps->next->uio_map.size, 1);
    Acq *acq = malloc(sizeof(Acq));

    acq -> reg = reg;
    acq -> buff = buff;
    acq -> fd = fd;
    acq -> data_start = malloc(DATA_LEN * sizeof(uint16_t));
    acq -> data_end = acq -> data_start; 

    return acq;
}

void print_buffer(Acq *acq, uint8_t N) {
    for (int i=0; i<N; i++) 
        for (int j=0; j<3; j++) 
            printf("buff[%d*3 + %d]: 0x%.4hX, %d\n", i, j, 
                acq->buff[i*3 + j], acq->buff[i*3 + j]);
}

void start_acq(Acq *acq) {
    set_reg(&(acq->reg->start_acq), 1);
    acq->active = 1;
}

void stop_acq(Acq *acq) {
    set_reg(&(acq->reg->start_acq), 0);
    acq->active = 0;
}

void set_decimation_osc(Acq *acq, uint32_t dec) {
    set_reg(&(acq->reg->dec_factor), dec);
}

void set_decimation_int(Acq *acq, uint32_t dec) {
    set_reg(&(acq->reg->integrator_dec), dec);
}

void print_reg(Acq *acq) {
    printf("dec_factor: %d\n", acq->reg->dec_factor);
    printf("start_acq: %d\n", acq->reg->start_acq);
    printf("dest_addr: %d\n", acq->reg->dest_addr);
    printf("buff_size: %d\n", acq->reg->buff_size);
    printf("fifo_count: %d\n", acq->reg->transfer_count);
    printf("fifo_min_thresh: %d\n", acq->reg->fifo_min_thresh);
    printf("fifo_dout_1: 0x%.8X\n", acq->reg->fifo_dout_1);
    printf("fifo_dout_2: 0x%.8X\n", acq->reg->fifo_dout_2);
}

int wait(Acq *acq) {
    uint32_t info;
    ssize_t nb;

    // Wait for data with poll
    struct pollfd fds = {
        .fd = acq->fd,
        .events = POLLIN,
    };

    while (acq->active) {
        // Trigger the interrupt
        info = 1;
        nb = write(acq->fd, &info, sizeof(info));
        if (nb != (ssize_t)sizeof(info)) {
            perror("write() error");
            return 1;
        }
        // Check if there is already data available
        nb = read(acq->fd, &info, sizeof(info));
        if (nb == (ssize_t)sizeof(info)) {
            // Data was already present; handle it immediately
            return 0;
        } else if (nb < 0 && errno != EAGAIN) {
            perror("Initial read() error");
            return 1;
        }

        // int ret = poll(&fds, 1, TIMEOUT_MS);
        
        // if (ret > 0) {
        //     // Read the interrupt data
        //     nb = read(acq->fd, &info, sizeof(info));
        //     if (nb == (ssize_t)sizeof(info)) {
        //         return 0; // Success
        //     } else {
        //         perror("read() error");
        //     }
        // } else if (ret == 0) {
        //     fprintf(stderr, "Timeout occurred\n");
        // } else {
        //     perror("poll() error");
        // }
    }

    return 1;
}


void clear_interrupt(Acq *acq) {
    // printf("Interrupt cleared!\n");
    set_reg(&(acq->reg->ctrl_reg), 0x00000001);
}

void request_interrupt(Acq *acq) {
    // printf("Interrupt requested!\n");
    set_reg(&(acq->reg->ctrl_reg), 0x00000002);
}

void request_and_clear_interrupt(Acq *acq) {
    // printf("Interrupt requested!\n");
    set_reg(&(acq->reg->ctrl_reg), 0x00000003);
}

void set_input_adc(Acq *acq) {
    // printf("Interrupt requested!\n");
    set_reg(&(acq->reg->input_sel), 0x00000001);
}