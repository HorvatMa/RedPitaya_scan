#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define FPGA_CLK_HZ        125000000.0
#define WFM1_MAX_LEN       16384
#define WFM2_MAX_LEN       12288   /* longer tables alias onto the control registers */

/* One acquisition record as written by the FPGA (little-endian, 8 bytes). */
typedef struct {
    int16_t  x;      /* slow axis: OUT1 command          */
    int16_t  y;      /* fast axis: IN2 sensor (INPUT_SEL=1) */
    uint32_t v;      /* detector: integral of IN1          */
} record_t;

_Static_assert(sizeof(record_t) == 8, "record_t must match the FPGA layout");

#endif
