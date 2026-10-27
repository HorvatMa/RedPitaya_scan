#include <stdio.h>
#include <unistd.h>
#include "../app/oscilloscope.h"
#include "../app/uio_parser.h"
#include "../app/device_interface.h"

int main() {
    Device_interface *device_interface;
    device_interface = malloc(sizeof(Device_interface));

    create_devices(device_interface);

    int N = 10;
    int16_t wfm[] = {0, 21056, 32272, 28384, 11256, -11192, -28352, -32272, -21136, -63};
    // int16_t wfm[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    printf("Setting waveform len!\n");
    set_waveform_len(device_interface->gen, N, WAVEFORM_1);
    set_waveform_len(device_interface->gen, N, WAVEFORM_2);

    printf("Setting waveform 1!\n");
    sleep(1);
    set_waveform(device_interface->gen, wfm, N, WAVEFORM_1);

    printf("Setting waveform 2!\n");
    sleep(1);
    set_waveform(device_interface->gen, wfm, N, WAVEFORM_2);

    printf("Waveform set!\n");

    printf("Setting decimation!\n");
    set_decimation_gen(device_interface->gen, 12500000, WAVEFORM_1);
    set_decimation_gen(device_interface->gen, 12500000, WAVEFORM_2);
    printf("Decimation set!\n");

    printf("Setting setart_gen!\n");
    start_gen(device_interface->gen);
    printf("Start_gen set!\n");

    printf("Setting start_acq!\n");
    start_acq(device_interface->acq);
    printf("Start_acq set!\n");

    return 0;
}