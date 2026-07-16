#ifndef GPU_GPU_INTERFACE_H_
#define GPU_GPU_INTERFACE_H_

//#include "data_engine.h"
#include "hetero_interface.h"
//using namespace cv;
#include "types_def.h"
#include "ringbuffer.h"
#include "vivado_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RK_GPU_LIB_PATH      "/tmp/real_part/lib/libhetero.so"

typedef struct {

    uint32_t data_offset; /* 在一帧图像中的行偏移 */
    uint32_t frame_size;  /* 一帧图像的总大小 */
    uint8_t rb_offset;    /*ring buffer 偏移*/

    unsigned char data_frame_max; /*申请缓存区的最大个数*/
    unsigned int osd_rb_offset;   /*拼接时的ring_buffer 计数*/

    uint8_t *in_yuv_buffer[DATA_STREAM_FRAME_MAX + 1]; /*输入的yuv buffer */
    uint8_t *in_rgb_buffer[DATA_STREAM_FRAME_MAX]; /*输入的rgb buffer */
    uint8_t *gui_osd_buffer[DATA_STREAM_FRAME_MAX]; /*osd buffer */
    uint8_t *out_yuv_buffer[DATA_STREAM_FRAME_MAX]; /*输出的yuv buffer */
}gpu_image_info_t;

typedef struct {

    ring_buffer_t *osd_data_rb[DATA_CHANNEL_NUM_MAX]; /*osd 编码的ring_buffer*/
    ring_buffer_t *mpp_data_rb[DATA_CHANNEL_NUM_MAX]; /*mpp 编码的ring_buffer*/
    gpu_image_info_t gpu_image[DATA_CHANNEL_NUM_MAX]; /*图像的相关信息*/

    void* hetero_hdr; /*GPU句柄*/
    hetero_world_st htr;
}GPU_Frame_st;


GPU_Frame_st* gpu_data_stream_get(void);

void gpu_osd_to_queue(unsigned char chn, image_frame_info_t *frame_info);
void gpu_mpp_to_queue(unsigned char chn, image_frame_info_t *frame_info);

int gpu_yuv_to_rgb(uint8_t* rgb_buffer, uint8_t* yuv_buffer, unsigned int width, unsigned hight);
int gpu_osd_chn_image_init(unsigned char chn, unsigned int src_width, unsigned int src_hight);
void gpu_osd_chn_image_destory(unsigned char chn);
int gpu_cooperate_encode_yuv(uint8_t* osd_buffer,uint8_t *rgb_buffer, uint8_t* yuv_buffer, unsigned int width, unsigned int height);

void gpu_chn_ringbuffer_is_empty(unsigned char chn);

int gpu_lib_init(void);
void gpu_lib_destory(void);


#ifdef __cplusplus
}
#endif
#endif /* GPU_GPU_INTERFACE_H_ */
