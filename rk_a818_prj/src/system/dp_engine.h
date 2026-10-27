#ifndef DP_ENGINE_H_
#define DP_ENGINE_H_

#include "vivado_common.h"
#include "types_def.h"
#include "drm_utils.h"
#include "ringbuffer.h"
#include "rga_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dp_send_info {
    pthread_t cvtColor_tid;
    unsigned char open_flag;
    unsigned char resize_flag;
    unsigned int width;
    unsigned int height;
    unsigned int src_width;
    unsigned int width_max;
    unsigned int height_max;
    unsigned char dp_rb_offset;
    unsigned int front_buf;
    unsigned char data_frame_max;
    unsigned int front_buf_max;

    ring_buffer_t *data_rb;

    RGA_IMAGE_S yuv_data[DATA_STREAM_FRAME_MAX];
    RGA_IMAGE_S bgra8888_data[DATA_STREAM_FRAME_MAX];
    RGA_IMAGE_S resize_data;
};

struct dp_send_info *dp_stream_get(void);
void fill_image_to_queue(image_frame_info_t *frame_info);
unsigned int drm_dp_chn_image_init(unsigned int width, unsigned int height);
int drm_dp_chn_image_destory(void);
int drm_dp_data_init(void);
void drm_dp_data_destory(void);
void dp_discard_queues(void);
void dp_chn_ringbuffer_is_empty(void);

#ifdef __cplusplus
}
#endif
#endif /* GPU_GPU_INTERFACE_H_ */
