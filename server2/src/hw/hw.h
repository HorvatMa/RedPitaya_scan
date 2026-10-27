#ifndef HW_H
#define HW_H

#include <stddef.h>
#include <stdint.h>

#include "../common.h"

/* IN2 is a 14-bit ADC; the FPGA shifts it left by 2, so the sensor value moves in steps of 4. */
#define SENSOR_LSB 4

/* Everything the FPGA needs for one run. Tables are int16 DAC counts. */
typedef struct {
    const int16_t *wfm1;  uint32_t len1, dec1;   /* OUT1, slow axis */
    const int16_t *wfm2;  uint32_t len2, dec2;   /* OUT2, fast axis */
    uint32_t       acq_dec;                      /* >= 2 */
    uint32_t       int_dec;                      /* >= 1 */
    double         sim_sensor_gain;              /* fake backend only: sensor = mirror / gain */
} hw_setup_t;

typedef struct hw hw_t;

/*
 * Backends: hw_fake.c (simulator, runs anywhere) and hw_uio.c (the real FPGA through UIO,
 * on the board with the overlay applied). Exactly one is linked into each server binary.
 */
hw_t       *hw_open(void);
void        hw_close(hw_t *hw);
const char *hw_name(const hw_t *hw);

int  hw_configure(hw_t *hw, const hw_setup_t *setup);   /* only while stopped; 0 = ok */
int  hw_start(hw_t *hw);
void hw_stop(hw_t *hw);

/*
 * Wait up to timeout_ms for acquisition data and copy up to max records into dst.
 * Returns the number of records, 0 on timeout, -1 on a hardware error.
 */
long hw_read(hw_t *hw, record_t *dst, size_t max, int timeout_ms);

#endif
