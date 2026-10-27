/*
 * File      : data_engine.h
 * This file is vide data engine interface header  
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-1-4     fengyong    first version
 * 2021-6-22    fengyong    modify for CSI Camera and IR Camera 
 */

#ifndef _DATA_MANAGER_H
#define _DATA_MANAGER_H


#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

#include "types_def.h"
#include "ringbuffer.h"
#include "vivado_common.h"
#include "rga_interface.h"

typedef struct {
    uint32_t data_offset; /* 在一帧图像中的行偏移 */
    uint32_t frame_size;
    uint8_t rb_offset;
    RGA_IMAGE_S yuv422_data[DATA_STREAM_FRAME_MAX + 1];
    RGA_IMAGE_S rgb888_data[DATA_STREAM_FRAME_MAX];
    RGA_IMAGE_S resize_data[DATA_STREAM_FRAME_MAX];
    RGA_IMAGE_S rotate_data[DATA_STREAM_FRAME_MAX];
}chn_data_info_t;

typedef struct {
    pthread_t tid;
    unsigned char chn;
    unsigned char rga_mode;
    unsigned char init_flag[INIT_MAX];

    unsigned char stop_flag;

    unsigned int frame_num;
    unsigned int rga_rb_offset;

    unsigned char data_frame_max;

    ring_buffer_t *chn_data_rb;
    ring_buffer_t *stitchbase_data_rb;

    chn_data_info_t chn_data;
    ratio_image_info_t ratio_info;
    mpp_image_info_t mpp_info;
    resize_info_t resize_info;
    fpga_reg_val_info_t reg_val;
    
}fpga_chn_image_info_t;

typedef struct {
    unsigned char flag;
    unsigned int x;
    unsigned int y;
}stitchbase_chn_info_t;


typedef struct {
    pthread_t tid;
    pthread_t high_chn_tid;
    pthread_t low_chn_tid;
    unsigned char ratio;
    unsigned char dp_chn;
    unsigned char stitchbase_open;
    unsigned char stitchbase_stop;
    unsigned char stitchbase_osd_flag;
    unsigned int  stitchbase_position;
    unsigned int  stitchbase_index;
    unsigned int  no_signal_position;
    osd_image_info_t osd_info;

    unsigned char* rga_vir_addr;
    unsigned int rga_addr_offset;
    unsigned int rga_addr_offset_max;

    fpga_chn_image_info_t *chn_image[DATA_CHANNEL_NUM_MAX];
    stitchbase_chn_info_t stitchbase_chn_info[DATA_CHANNEL_NUM_MAX];

    unsigned char *no_signal_buffer;
    unsigned char no_signal_index;
    unsigned char no_signal_flag;

    unsigned char gui_osd_flag;
	pthread_mutex_t osd_lock;
    pthread_mutex_t swap_lock;
    pthread_mutex_t dp_lock;
    pthread_mutex_t rga_lock;
}data_stream_t;

data_stream_t *data_stream_get(void);

unsigned int data_stream_low_chn_data_init(unsigned char chn);
void data_stream_low_chn_data_destory(unsigned char chn);

int data_stream_init(void);
void data_stream_destory(void);
void rgb888_output_image_init(unsigned int rgb);
void data_stream_chn_ringbuffer_is_empty(unsigned char chn);

unsigned int data_stream_stitchbase_thread_init(void);
int data_stream_stop_stitchbase(unsigned int timeout_ms);
void data_stream_stitchbase_thread_destory(void);

unsigned int data_stream_low_chn_thread_init(unsigned char chn);
void data_stream_low_chn_thread_destory(unsigned char chn);

#ifdef __cplusplus
}
#endif

#endif /* DATA_MANAGER_H */
