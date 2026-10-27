#ifndef UIO_LIB_OSCILLOSCOPE_H
#define UIO_LIB_OSCILLOSCOPE_H

#ifndef ACQ_BUF_SIZE
#define ACQ_BUF_SIZE (65536) / 2.0
#endif

#ifndef TIMEOUT_MS
#define TIMEOUT_MS 5
#endif

#ifndef DATA_LEN
#define DATA_LEN 1000000
#endif

#include <stddef.h>
#include <stdint.h>
#include "uio_parser.h"

typedef volatile struct {
    uint32_t dec_factor;
    uint32_t start_acq;
    uint32_t dest_addr;
    uint32_t buff_size;
    uint32_t transfer_count;
    uint32_t fifo_min_thresh;
    uint32_t fifo_dout_1;
    uint32_t fifo_dout_2;
    uint32_t ctrl_reg;
    uint32_t integrator_dec;
    uint32_t buff_n;
    uint32_t input_sel;
} Reg_map_osc;

typedef struct{
    int16_t osc1;
    int16_t osc2;
    uint32_t integrator;
} Buff_data;

typedef struct {
    Reg_map_osc *reg;
    Buff_data *buff;
    Buff_data *data_end;
    Buff_data *data_start;
    int fd;
    volatile int active;
} Acq;

void print_reg(Acq *);
void print_buffer(Acq *, uint8_t);
void start_acq(Acq *);
void stop_acq(Acq *);
Acq *create_acq(Uio);
int wait(Acq *);
void clear_interrupt(Acq *);
void request_interrupt(Acq *);
void request_and_clear_interrupt(Acq *);
void set_decimation_osc(Acq *, uint32_t);
void set_decimation_int(Acq *, uint32_t);
void set_input_adc(Acq *);

#endif