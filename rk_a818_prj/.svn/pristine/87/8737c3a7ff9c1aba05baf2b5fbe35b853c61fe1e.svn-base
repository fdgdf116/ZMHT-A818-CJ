/*
 * File      : mpp_encode.h
 * This file is RK3399PRO MPP encode interface header  
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-12-18     fengyong    first version
 */

#ifndef DRM_UTILS_H
#define DRM_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "vivado_common.h"
#include "ringbuffer.h"

int drm_device_init(unsigned int front_buf_max);
void drm_device_destory(void);
void draw_image_to_queue(draw_frame_info_t *frame_info);
void drm_fill_draw_buffer(unsigned char* buffer, unsigned char front_buf, unsigned int width, unsigned int height,unsigned int src_width);
ring_buffer_t * get_draw_rb(void);

#ifdef __cplusplus
}
#endif

#endif /* ZM_MPPENCODE_H */
