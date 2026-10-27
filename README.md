# RedPitaya SiPM

RedPitaya system for image acquisition with scalar detector.

## Connection

OUT1 -- y galvo (Higher frequency)
OUT2 -- x galvo
IN1 -- y galvo position sensor
IN2 -- detector

The connection of the y galvo position sensor to the output is neccesary since at high frequancies (few 100 Hz) the lag between actual mirror position and signal output is significant (1/4 period @ 1 kHz) for Thorlabs GVS012.

## FPGA image

The FPGA program is written with vivado 2020.2 in verilog/SystemVerilog. The settings for generation of signals and acquisiton are handeled by writing to apropriate RAM positions inside the OS.

## Device Tree Overlay

The device tree overlay source file fpga.dtso is needed to tell the OS which parts of the memeory are mapped to FPGA functions. By applying the overlay with 
    ./apply_overlay.sh rp
OS then creates UIO devices which can be accessed and properly mapped inside the C application.

## C server

The server is written in C. It listens on port 5000 and supports commands:

- set_waveform {0 for OUT1 or 1 for OUT2} {WAVEFORM LENGTH} -- Sets the waveform length register. Command should be followed immediately by the same number int16 values which interpolate the availbale range +- 1 V.
- set_gen_decimation {0 for OUT1 or 1 for OUT2} {DECIMATION VALUE}
- start_gen -- Starts the generators
- stop_gen -- Stops the generators
- exit -- exits server
- start_acq -- Starts acquisition
- stop_acq -- Stops acquisition
- set_acq_decimation {DECIMATION VALUE} -- Sets acquisition decimation register
- set_int_decimation {DECIMATION VALUE} -- Sets integration decimation register

## Compiling

Server can be compiled by running the
    make server
command inside ./server directory.

fpga.dtbo needed in the apply_overlay.sh can be compiled from fpga.dtso using compile_dt.sh script.

## Workflow 

After setting both waveforms the generator decimation should be set. It sets the amount of the 125 MHz cycles which need to pass for the generator to move to the next waveform sample. After that the acquisition decimation should be set, which works the same as generators. (NOTE: too high of an acquisition will clog the TCP). The default value for integrator decimation is 1 which takes avery sameple into integration using parallelograms, it can be optionally changed. After that the generators can be started following by a start of acquisition. The server will the continiously stream samples (x: int16, y: int16, integrated_input uint32) (NOTE: integrator takes the input as an unsigned value, meaning that instead of +- 1 V in works as if the input was 0 V - 2 V). When finished the acquisition and generation can be stopped.

Test program of how the image is constructed is inside the ./client/src/image_test_2.py