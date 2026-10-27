#ifndef IMAGE_H
#define IMAGE_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "oscilloscope.h"

#define MAX_IMAGE_SIZE 1000000

typedef struct {
    int16_t x_lower;
    int16_t x_upper;
    int16_t y_lower;
    int16_t y_upper;
    size_t x_res;
    size_t y_res;
    uint16_t frame_time;
} Img_params;

int rescale_image_to_int8(const double*, uint8_t*, size_t);
int update_or_create_image_from_struct(
    const Buff_data*, size_t,
    int16_t, int16_t, int16_t, int16_t, size_t, size_t, double**, int**);

#endif