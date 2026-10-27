#include "device_interface.h"
#include "parser.h"
#include "generator.h"
#include "oscilloscope.h"
#include "uio_parser.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "image.h"
#include "transfer.h"
#include <pthread.h>
#include "common.h"

void get_msg(int connfd, char *buff) {
    unsigned int message_len = 0, recv_bytes = 0;

    // read the message from client and copy it in buffer 
    read_n_bytes(connfd, buff, LEN_BYTES); 
    
    message_len = 0;
    // Combine bytes into an unsigned int (big-endian)
    for (size_t i = 0; i < LEN_BYTES; i++) {
        message_len = (message_len << 8) | (unsigned char)buff[i];
    }
    printf("message_len: %d \n", message_len);

    read_n_bytes(connfd, buff, message_len);
}

void read_n_bytes(int connfd, char *buff, int n_bytes){
    int recv_bytes = 0;
    while (recv_bytes < n_bytes) {
        recv_bytes += read(connfd, &buff[recv_bytes], n_bytes - recv_bytes);  
        printf("recv_bytes: %d \n", recv_bytes);
    }
    printf("recived message successfully!\n");
}

int create_devices(Device_interface *device_interface) {
    Uio_list *uio_list;

    uio_list = get_uios();

    device_interface->acq = create_acq(*find_uio_by_name(uio_list, "rp_acquire"));

    if(device_interface->acq == NULL) {
        printf("rp_acquire not found!\n");
        return -1;
    }

    device_interface->gen = create_gen(*find_uio_by_name(uio_list, "rp_generate"));

    if(device_interface->gen == NULL) {
        printf("rp_generate not found!\n");
        return -1;
    }

    printf("Devices created successfully!\n");

    return 0;
}

void stop(Device_interface* device_interface) {
    stop_acq(device_interface->acq);
    stop_gen(device_interface->gen);
}

int execute_command(Device_interface *device_interface, int command, 
    char **rest, char *buff, int connfd, Worker_args *worker_args) {

    int i_waveform;
    char *token;
    int16_t *waveform;
    int32_t *waveform_double;

    Img_params *im_par = worker_args->im_par;
    pthread_t *thread_id = worker_args->thread_id;

    uint32_t ui32;
    int16_t i16;
    uint16_t ui16;
    
    switch (command) {
        case COMMAND_SET_WAVEFORM:
            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            i_waveform = atoi(token);

            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            ui32 = atoi(token);

            printf("Command paramters %d, %u\n", i_waveform, ui32);

            set_waveform_len(device_interface->gen, ui32, i_waveform);
            printf("WFM LEN: %d\n", device_interface->gen->reg->waveform_len_1);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);     
            
            printf("krele1\n");
            bzero(buff, MAX);
            printf("krele2\n");
            get_msg(connfd, buff);
            printf("krele3\n");

            waveform = convertBigEndianToSignedIntArray(buff, ui32);

            // for (int i = 0; i < waveform_len; i++) {
            //     printf("%d\n", waveform[i]);
            // }

            // bzero(buff, MAX); 
            // strcpy(buff, "received waveform\n");   
            // write(connfd, buff, 18);  

            // waveform_double = (int32_t*)waveform;
            // for (int i = 0; i < waveform_len/2; i++) {
            //     printf("%d\n", waveform_double[i]);
            // }

            printf("Setting waveform!\n");
            // set_waveform(device_interface->gen, waveform_double, waveform_len/2, i_waveform);
            if (i_waveform == WAVEFORM_1)
                memcpy(device_interface->gen->waveform_container->waveform_1, 
                    waveform, ui32*sizeof(int16_t));
            else if (i_waveform == WAVEFORM_2)
                memcpy(device_interface->gen->waveform_container->waveform_2, 
                    waveform, ui32*sizeof(int16_t));
            // set_waveform(device_interface->gen, waveform, waveform_len, WAVEFORM_1);

            printf("WFM LEN: %d\n", device_interface->gen->reg->waveform_len_1);
            printf("Waveform set!\n");

            break;

        case COMMAND_SET_GEN_DECIMATION:
            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            i_waveform = atoi(token);

            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            ui32 = atoi(token);

            set_decimation_gen(device_interface->gen, ui32, i_waveform);
            printf("WFM LEN: %d\n", device_interface->gen->reg->waveform_len_1);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);  

            break;

        case COMMAND_START_GEN:
            start_gen(device_interface->gen);
            printf("WFM LEN: %d\n", device_interface->gen->reg->waveform_len_1);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);  

            break;

        case COMMAND_STOP_GEN:
            stop_gen(device_interface->gen);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);  

            break;

        case COMMAND_SET_ACQ_DECIMATION:
            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            ui32 = atoi(token);

            set_decimation_osc(device_interface->acq, ui32);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);  

            break;

        case COMMAND_START_ACQ:
            pthread_create(thread_id, NULL, do_work, (void *)worker_args);
            start_acq(device_interface->acq);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);  

            break;

        case COMMAND_STOP_ACQ:
            stop_acq(device_interface->acq);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);  

            break;

        case COMMAND_SET_INTEGRATOR_DECIMATION:
            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            ui32 = atoi(token);

            set_decimation_int(device_interface->acq, ui32);

            // bzero(buff, MAX); 
            // strcpy(buff, "OK\n");
            // write(connfd, buff, 3);  

            break;

        case COMMAND_SEND_BUFFER:
            send_buffer(device_interface, buff, connfd);

            break;

        case COMMAND_SET_LIMITS:
            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            i16 = atoi(token);

            im_par->x_lower = i16;

            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            i16 = atoi(token);

            im_par->x_upper = i16;

            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            i16 = atoi(token);

            im_par->y_lower = i16;

            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            i16 = atoi(token);

            im_par->y_upper = i16;

            break;

        case COMMAND_SET_RESOLUTION:
            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            size_t res = atoi(token);

            im_par->x_res = res;

            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            res = atoi(token);

            im_par->y_res = res;

            break;

            break;

        case COMMAND_SET_FRAME_TIME:
            token = strtok_r(*rest, " ", rest);
            printf("token: %s\n", token);
            uint16_t frame_time = atoi(token);

            im_par->frame_time = frame_time;

            break;
    }
}

void send_buffer(Device_interface* device_interface, char *buff, int connfd) {
    int len = device_interface->acq->data_end - device_interface->acq->data_start;

    printf("len: %d\n", len);

    bzero(buff, MAX); 
    sprintf(buff, "%0*d\n", 4, len*8);
    printf(buff);
    write(connfd, buff, 5);  

    sleep(1);

    write(connfd, (char*) device_interface->acq->data_start, len*8);
}