#include <stdlib.h>
#include <stdio.h>
#include "oscilloscope.h"
#include <float.h>
#include <string.h>
#include <omp.h>

// Update or create a 2D image and auxiliary count array from signal arrays
// Parameters:
// x_signal, y_signal, val_signal: input arrays of length n
// n: length of input arrays
// x_lower, x_upper: limits for x values
// y_lower, y_upper: limits for y values
// x_res, y_res: resolution (number of pixels in x and y direction)
// image: pointer to a pointer to the image array (flattened 1D, size x_res*y_res)
// count: pointer to a pointer to the count array (size x_res*y_res)
// Returns: 0 on success, -1 on failure
int update_or_create_image_from_struct(
    const Buff_data* data,
    size_t n,
    int16_t x_lower,
    int16_t x_upper,
    int16_t y_lower,
    int16_t y_upper,
    size_t x_res,
    size_t y_res,
    double** image,
    int** count)
{
    // Allocate memory if image or count is NULL
    if (!*image) {
        *image = calloc(x_res * y_res, sizeof(double));
        if (!*image) {
            perror("Failed to allocate memory for image");
            return -1;
        }
    }

    // if (!*count) {
    //     *count = calloc(x_res * y_res, sizeof(int));
    //     if (!*count) {
    //         perror("Failed to allocate memory for count");
    //         free(*image);
    //         *image = NULL;
    //         return -1;
    //     }
    // }

    // Update the image and count arrays
    float inv_x_range = 1.0f / (x_upper - x_lower);
    float inv_y_range = 1.0f / (y_upper - y_lower);

    #pragma omp parallel for
    for (size_t i = 0; i < n; i++) {
        int16_t x = data[i].osc1;
        int16_t y = data[i].osc2;

        // Check bounds
        if (x < x_lower || x > x_upper || y < y_lower || y > y_upper) {
            continue;
        }

        // Map x, y to indices
        size_t x_idx = (size_t)((x - x_lower) * inv_x_range * (x_res - 1));
        size_t y_idx = (size_t)((y - y_lower) * inv_y_range * (y_res - 1));

        // Clamp indices
        x_idx = x_idx < x_res ? x_idx : x_res - 1;
        y_idx = y_idx < y_res ? y_idx : y_res - 1;

        // Compute the 1D index
        size_t index = y_idx * x_res + x_idx;

        // Update image and count atomically
        // #pragma omp atomic
        // (*image)[index] = ((*image)[index] * (*count)[index] + (double)(data[i].integrator)) / ((*count)[index] + 1);
        #pragma omp atomic
        (*image)[index] = (double)(data[i].integrator);

        // #pragma omp atomic
        // (*count)[index]++;
    }

    return 0;
}

int rescale_image_to_int8(const double* image, uint8_t* rescaled_image, size_t size) {
    if (!image || !rescaled_image || size == 0) {
        fprintf(stderr, "Error: Invalid input to rescale_image_to_int8.\n");
        return -1;
    }

    // Find the minimum and maximum values in the image
    double min_value = image[0];
    double max_value = image[0];
    for (size_t i = 1; i < size; i++) {
        if (image[i] < min_value) {
            min_value = image[i];
        }
        if (image[i] > max_value) {
            max_value = image[i];
        }
    }

    printf("%.2f %.2f \n", min_value, max_value);

    // Handle the edge case where all values in the image are the same
    if (max_value == min_value) {
        memset(rescaled_image, 0, size * sizeof(uint8_t));
        return 0;
    }

    // Translate and rescale the image to int8_t range (0-255)
    double range = max_value - min_value;
    for (size_t i = 0; i < size; i++) {
        // printf("%.2f %.2f\n", image[i], ((image[i] - min_value) / range) * 255);
        rescaled_image[i] = (uint8_t)(((image[i] - min_value) / range) * 255);
    }

    return 0;
}