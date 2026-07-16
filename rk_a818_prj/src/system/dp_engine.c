#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <memory.h>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <sys/time.h>

#include "pcie_reg_rw.h"
#include "drm_utils.h"
#include "data_engine.h"
#include "dp_engine.h"
#include "xutils.h"
#include "log.h"
#include "a818_error.h"

static struct dp_send_info *dp_send = NULL;
static const char *dp_modes = "/sys/class/drm/card0-DP-1/modes";

static int dp_c = 'x';
static unsigned char mode_buffer[12] = {0};

struct dp_send_info *dp_stream_get(void)
{
    return dp_send;
}

void fill_image_to_queue(image_frame_info_t *frame_info)
{
    image_frame_info_t event_param, tmp_param;
    ring_buffer_t *rb = dp_send->data_rb;
	
	if (ringbuffer_is_full(rb)) {					
		ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));
        SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}
	memcpy(&event_param,frame_info,sizeof(image_frame_info_t));
    		
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}

unsigned int drm_dp_rga_selftest(unsigned int width, unsigned int height)
{
    #if 0    
    int ret, index;
    unsigned int ret_Val;
    for(index = 0; index < dp_send->data_frame_max; index++){
        ret = rga_comm_create_image(&dp_send->yuv_data[index],RK_FORMAT_YCrCb_422_SP, width,height);
        if(ret != 0){
            printf("[ERROR] init rgb888_data[%d] error \n", index);
            ret_Val = A818_DP_RGB_DATA;
            ret_Val |= (ret << 8);
            goto err_1;
        }
        ret = rga_comm_create_image(&dp_send->bgra8888_data[index],RK_FORMAT_RGBA_8888, width,height);
        if(ret != 0){
            printf("[ERROR] init BGRA_8888[%d] error \n", index);
            ret_Val = A818_DP_BGRA_DATA;
            ret_Val |= (ret << 8);
            goto err_1;
        }
    }

    ret_Val = 0;

err_1:
    for(index = 0; index < dp_send->data_frame_max; index++){
        if(dp_send->yuv_data[index].vir_addr){
            rga_release_frame_image(&dp_send->yuv_data[index]);
            dp_send->yuv_data[index].vir_addr = NULL;
        }

        if(dp_send->bgra8888_data[index].vir_addr){
            rga_release_frame_image(&dp_send->bgra8888_data[index]);
            dp_send->bgra8888_data[index].vir_addr = NULL;
        }
    }

    return ret_Val;
    #else
        return 0;
    #endif
}

/*
	函数: unsigned int drm_dp_chn_image_init(unsigned int width, unsigned int height)
	作用: dp显示的相关ring buffer的申请
	参数:
		width：显示图像的宽
        height：显示图像的高
	返回值:
		正确返回0，失败返回其他
*/
unsigned int drm_dp_chn_image_init(unsigned int width, unsigned int height)
{
    int ret, index;
    unsigned int ret_Val;
    unsigned char* tmp_buffer;

    ret_Val = 0;

    dp_send->width = 0;
    dp_send->height = 0;

    if(dp_send->open_flag){
        return 200;
    }

    /*获取最大的显示宽高*/
    ret = read_size_data_to_buf(mode_buffer, dp_modes, sizeof(mode_buffer));
    if(!ret){
        printf("[ERROR] Open %s failed \n",dp_modes);
        return A818_DP_READ_MODE;
    }

    tmp_buffer = strchr((char*)mode_buffer, dp_c);
    dp_send->width_max = atol(mode_buffer);
	dp_send->height_max = atol(tmp_buffer + 1);

#if 0
    /*超出最大的宽高，不做处理*/
    if((width > dp_send->width_max) || (height > dp_send->height_max)){
        return 201;
    }

    /*这部分原本是，当宽/高 大于最大的宽高的时候，会进行缩放，目前暂时没做*/
    if(dp_send->width_max > VIDEO_DP_WIDTH_MAX){
        dp_send->width_max = VIDEO_DP_WIDTH_MAX;
    }

    if(dp_send->height_max > VIDEO_DP_HEIGHT_MAX){
        dp_send->height_max = VIDEO_DP_HEIGHT_MAX;
    }

    dp_send->resize_flag = 0;

    if(width > dp_send->width_max){
        dp_send->width = dp_send->width_max;
        dp_send->resize_flag = 1;
    } else {
        dp_send->width = width;
    }

    if(height > dp_send->height_max){
        dp_send->height = dp_send->height_max;
        dp_send->resize_flag = 1;
    } else {
        dp_send->height = height;
    }

    /*如果想要在中间显示，就需要将起点坐标计算出来，目前是以坐上角为起点的*/
    dp_send->x = (dp_send->width_max - dp_send->width) / 2;
    dp_send->y = (dp_send->height_max - dp_send->height) / 2;
#else
    if(width > dp_send->width_max){
        dp_send->width = dp_send->width_max;
    } else {
        dp_send->width = width;
    }

    if(height > dp_send->height_max){
        dp_send->height = dp_send->height_max;
    } else {
        dp_send->height = height;
    }
    dp_send->src_width = width;
#endif

    memset(dp_send->yuv_data, 0x0, sizeof(dp_send->yuv_data));
    memset(&dp_send->resize_data, 0x0, sizeof(dp_send->resize_data));
    memset(&dp_send->bgra8888_data, 0x0, sizeof(dp_send->bgra8888_data));


    dp_send->data_frame_max = 0;
    if((width== VIDEO_FRAME_WIDTH_4k) && (height == VIDEO_FRAME_HEIGHT_4k)){
        dp_send->data_frame_max = DATA_4K_STREAM_FRAME_MAX;
        dp_send->front_buf_max = 2;
    } else {
        dp_send->data_frame_max = DATA_STREAM_FRAME_MAX;
        dp_send->front_buf_max = 8;
    }

    ret = drm_dp_rga_selftest(width,height);
    if(ret != 0){
        return ret;
    }   

    /*相关资源的申请*/
    for(index = 0; index < dp_send->data_frame_max; index++){
        ret = rga_comm_create_image(&dp_send->yuv_data[index],RK_FORMAT_YCrCb_422_SP, width,height);
        if(ret != 0){
            printf("[ERROR] init rgb888_data[%d] error \n", index);
            ret_Val = A818_DP_RGB_DATA;
            ret_Val |= (ret << 8);
            goto err_1;
        }
        ret = rga_comm_create_image(&dp_send->bgra8888_data[index],RK_FORMAT_RGBA_8888, width,height);
        if(ret != 0){
            printf("[ERROR] init BGRA_8888[%d] error \n", index);
            ret_Val = A818_DP_BGRA_DATA;
            ret_Val |= (ret << 8);
            goto err_1;
        }

    }

    /*drm区域初始化*/
    ret = drm_device_init(dp_send->front_buf_max);
    if(ret != 0){
        printf("[ERROR] drm init error \n");
        return A818_DRM_DEVICE_INIT;
    }

    dp_send->open_flag = 1;
    SLOGI("[DEBUG] %s width:%d height:%d open_flag:%d \n", __func__, width, height, dp_send->open_flag);

    return A818_SUCCESS;

err_1:
    for(index = 0; index < dp_send->data_frame_max; index++){
        if(dp_send->yuv_data[index].vir_addr){
            rga_release_frame_image(&dp_send->yuv_data[index]);
            dp_send->yuv_data[index].vir_addr = NULL;
        }
        if(dp_send->bgra8888_data[index].vir_addr){
            rga_release_frame_image(&dp_send->bgra8888_data[index]);
            dp_send->bgra8888_data[index].vir_addr = NULL;
        }
    }
    return ret_Val;
}

/*
	函数: void dp_chn_ringbuffer_is_empty(void)
	作用: dp显示的相关ring buffer的判断函数
	参数:
		无
	返回值:
		无
*/
void dp_chn_ringbuffer_is_empty(void)
{
    int i;
    int chn;
    image_frame_info_t frame_info = {0};
    ring_buffer_t *draw_rb = get_draw_rb();

    while(!ringbuffer_is_empty(dp_send->data_rb)){
        xt_nanosleep(0, 100);
    }

    while(!ringbuffer_is_empty(draw_rb)){
        xt_nanosleep(0, 100);
    }

    usleep(100*1000);
}

/*
	函数: int drm_dp_chn_image_destory(void)
	作用: dp显示的相关资源的释放
	参数:
		无
	返回值:
		正确返回0，失败返回其他
*/
int drm_dp_chn_image_destory(void)
{
    if((dp_send->width == 0) || (dp_send->height == 0)){
        return 1;
    }

    if(dp_send->open_flag){
        drm_device_destory();
    }   

    for(int i = 0; i < dp_send->data_frame_max; i++){
        if(dp_send->yuv_data[i].vir_addr){
            rga_release_frame_image(&dp_send->yuv_data[i]);
            dp_send->yuv_data[i].vir_addr = NULL;
        }

        if(dp_send->bgra8888_data[i].vir_addr){
            rga_release_frame_image(&dp_send->bgra8888_data[i]);
            dp_send->bgra8888_data[i].vir_addr = NULL;
        }

    }

    memset(dp_send->yuv_data, 0x0, sizeof(dp_send->yuv_data));
     memset(dp_send->bgra8888_data, 0x0, sizeof(dp_send->bgra8888_data));

    dp_send->open_flag = 0;

    return A818_SUCCESS;
}

/*
	函数: static void *drm_data_cvtColor_thread(void* parameter)
	作用: dp显示需要RGBA_8888格式，进行格式的转换和数据的拷贝
	参数:
		线程参数
	返回值:
		正确返回0，失败返回其他
*/
static void *drm_data_cvtColor_thread(void* parameter)
{
    struct timeval time;
    ring_buffer_t *draw_rb;
    image_frame_info_t frame_info = {0};
    draw_frame_info_t draw_frame_info = {0};
    ring_buffer_t *rb = dp_send->data_rb;
    data_stream_t *data_stream = data_stream_get();

    while(1){
        while(ringbuffer_is_empty(rb)){
            time.tv_sec=0;
            time.tv_usec=(1);
            select(0,NULL,NULL,NULL,&time); 
        }

        ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));

        /*格式转换*/
        pthread_mutex_lock(&data_stream->rga_lock);
        rga_cvtColor(&dp_send->yuv_data[frame_info.frame_offset],&dp_send->bgra8888_data[frame_info.frame_offset]);
        pthread_mutex_unlock(&data_stream->rga_lock);

        if(dp_send->front_buf >= dp_send->front_buf_max){
            dp_send->front_buf = 0;
        }

        /*拷贝到DP显示的buffer中*/
        draw_rb = get_draw_rb();
        if(!ringbuffer_is_full(draw_rb)){
            drm_fill_draw_buffer(dp_send->bgra8888_data[frame_info.frame_offset].vir_addr, dp_send->front_buf,dp_send->width,dp_send->height, dp_send->src_width);
            draw_frame_info.front_buf = dp_send->front_buf;
            draw_image_to_queue(&draw_frame_info);
            dp_send->front_buf += 1;
        } else {
            SLOGI("[Debug]:draw rb ringbuffer_is_full \n");
        }
    }

    return NULL;
}

/*
	函数: int drm_dp_data_init(void)
	作用: dp显示相关的数据初始化
	参数:
		无
	返回值:
		正确返回0，失败返回其他
*/
int drm_dp_data_init(void)
{
    int ret;

    dp_send = (struct dp_send_info *)malloc(sizeof(struct dp_send_info));
    if(!dp_send){
        printf("[ERROR] dp send info malloc error \n");
        return A818_DP_DATA_MALLOC;
    }

    memset(dp_send, 0x0, sizeof(struct dp_send_info));


    dp_send->data_rb = ringbuffer_create(RINGBUFFER_FRAME_MAX*sizeof(image_frame_info_t), sizeof(image_frame_info_t));
    if (!dp_send->data_rb) {
        printf("[ERROR] create dp data rb buffer failed\n");
        ret = A818_DP_DATA_RB;
        goto err_1;
    }


    ret = pthread_create(&dp_send->cvtColor_tid, NULL, &drm_data_cvtColor_thread, NULL);
    if (ret != 0) {
        printf("[ERROR] Create drm_dp_output_event_thread failed %d\n",ret);
        ret = A818_DRM_EVENTE_THREAD;
        goto err_2;
    }

    return 0;

err_2:
    ringbuffer_destory(dp_send->data_rb);
err_1:
    free(dp_send);
    return ret;
}

/*
	函数: void drm_dp_data_destory(void)
	作用: dp显示销毁函数
	参数:
		无
	返回值:
		无
*/
void drm_dp_data_destory(void)
{
    if(dp_send){
        pthread_cancel(dp_send->cvtColor_tid);
        ringbuffer_destory(dp_send->data_rb);
        free(dp_send);
    }
}
