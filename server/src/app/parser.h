#ifndef PARSER_H
#define PARSER_H

#define UNKOWN_COMMAND_ERROR -1

#define COMMAND_EXIT 0

#define COMMAND_SET_WAVEFORM 1
#define COMMAND_SET_GEN_DECIMATION 2
#define COMMAND_START_GEN 3
#define COMMAND_STOP_GEN 4
#define COMMAND_STOP_ACQ 5 
#define COMMAND_START_ACQ 6
#define COMMAND_SET_ACQ_DECIMATION 7
#define COMMAND_SET_INTEGRATOR_DECIMATION 8
#define COMMAND_SET_LIMITS 9
#define COMMAND_SET_RESOLUTION 10
#define COMMAND_SET_FRAME_TIME 11

#define COMMAND_SEND_BUFFER 12

#include <stdint.h>

int parse_msg(char **);
int16_t convertBigEndianToSignedInt(const uint8_t *);
int16_t* convertBigEndianToSignedIntArray(uint8_t *, int);

#endif