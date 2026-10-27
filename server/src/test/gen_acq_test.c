#include <stdio.h>
#include <unistd.h>
#include "../app/oscilloscope.h"
#include "../app/uio_parser.h"
#include "../app/device_interface.h"

int main() {
    Device_interface *device_interface;
    device_interface = malloc(sizeof(Device_interface));

    create_devices(device_interface);


    return 0;
}