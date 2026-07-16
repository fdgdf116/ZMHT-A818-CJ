#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <dlfcn.h>

#include "gpu_interface.h"
#include "xutils.h"
#include "log.h"
#include "a818_error.h"


static GPU_Frame_st *g_gpu_frame_stream = NULL;

GPU_Frame_st* gpu_data_stream_get(void)
{
    return g_gpu_frame_stream;
}

void gpu_osd_to_queue(unsigned char chn, image_frame_info_t *frame_info)
{
    GPU_Frame_st* frame_stream = gpu_data_stream_get();
	ring_buffer_t *rb = frame_stream->osd_data_rb[chn];
	image_frame_info_t event_param, tmp_param;

	if (ringbuffer_is_full(rb)) {					
		ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));
        SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}

	memcpy(&event_param,frame_info,sizeof(image_frame_info_t));	
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}

void gpu_mpp_to_queue(unsigned char chn, image_frame_info_t *frame_info)
{
    GPU_Frame_st* frame_stream = gpu_data_stream_get();
	ring_buffer_t *rb = frame_stream->mpp_data_rb[chn];
	image_frame_info_t event_param, tmp_param;

	if (ringbuffer_is_full(rb)) {					
		ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));
        SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}

	memcpy(&event_param,frame_info,sizeof(image_frame_info_t));
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}

/*******************************************************/
/*
	函数: gpu_yuv_to_rgb(uint8_t* rgb_buffer, uint8_t* yuv_buffer, unsigned int width, unsigned hight)
	作用: yuv转rgb
	参数:
		rgb_buffer: 存放rgb的buffer
		yuv_buffer:存放yuv的buffer
		width:转换的图像宽度
        hight:转换的图像高度
	返回值:
		正确返回0，错误返回其他
*/
int gpu_yuv_to_rgb(uint8_t* rgb_buffer, uint8_t* yuv_buffer, unsigned int width, unsigned hight)
{
    GPU_Frame_st* frame_stream = gpu_data_stream_get();
    return frame_stream->htr.cooperate_decode_yuv(width,hight, 0, e_yuv_yuyv, yuv_buffer, 0, rgb_buffer, 0);
}

/*
	函数: gpu_cooperate_encode_yuv(uint8_t* osd_buffer,uint8_t *rgb_buffer, uint8_t* yuv_buffer, unsigned int width, unsigned int height)
	作用: 继续图像叠加，叠加后的图像转成yuv格式
	参数:
		osd_buffer:存放osd的buffer
        rgb_buffer:存放rgb的buffer
		yuv_buffer:存放yuv的buffer
		width:图像宽度
        hight:图像高度
	返回值:
		正确返回0，错误返回其他
*/
int gpu_cooperate_encode_yuv(uint8_t* osd_buffer,uint8_t *rgb_buffer, uint8_t* yuv_buffer, unsigned int width, unsigned int height)
{
    GPU_Frame_st* frame_stream = gpu_data_stream_get();
    //return frame_stream->htr.cooperate_encode_yuv(width, height, e_yuv_uyvy, (uint16_t*)osd_buffer, rgb_buffer, yuv_buffer, 0);
    return frame_stream->htr.cooperate_encode_yuv(width, height, e_yuv_nv61, (uint16_t*)osd_buffer, rgb_buffer, yuv_buffer, 0);
}

static void gpu_chn_image_free(gpu_image_info_t *gpu_image)
{

    unsigned int index;
    gpu_image->data_offset = 0;
    gpu_image->frame_size = 0;
    gpu_image->rb_offset = 0;
    gpu_image->data_offset = 0;

    for(index = 0; index < gpu_image->data_frame_max; index++)
    {
        if(gpu_image->out_yuv_buffer[index]){
            free(gpu_image->out_yuv_buffer[index]);
            gpu_image->out_yuv_buffer[index] = NULL;
        }

        if(gpu_image->gui_osd_buffer[index]){
            free(gpu_image->gui_osd_buffer[index]);
            gpu_image->gui_osd_buffer[index] = NULL;
        }

        if(gpu_image->in_rgb_buffer[index]){
            free(gpu_image->in_rgb_buffer[index]);
            gpu_image->in_rgb_buffer[index] = NULL;
        }
    }

    for(index = 0; index < (gpu_image->data_frame_max + 1); index++){
        if(gpu_image->in_yuv_buffer[index]){
            free(gpu_image->in_yuv_buffer[index]);
            gpu_image->in_yuv_buffer[index] = NULL;
        }
    }

}

/*
	函数: int gpu_osd_chn_image_init(unsigned char chn, unsigned int src_width, unsigned int src_hight)
	作用: 初始化通道相关的gpu buffer
	参数:
		chn:需要初始化的通道
		src_width:图像宽度
        src_hight:图像高度
	返回值:
		正确返回0，错误返回其他
*/
int gpu_osd_chn_image_init(unsigned char chn, unsigned int src_width, unsigned int src_hight)
{
    int ret, i;
    int index;
    GPU_Frame_st* frame_stream = gpu_data_stream_get();
    gpu_image_info_t *gpu_image = &frame_stream->gpu_image[chn];

    SLOGI("[Debug] %s chn:%d width:%d hight:%d \n", __func__, chn, src_width, src_hight);

    //memset(gpu_image, 0x0, sizeof(gpu_image_info_t));
    gpu_image->osd_rb_offset = 0;
    gpu_image->data_frame_max = 0;

    /* 根据分辨率决定初始化的每种buffer的个数 */
    if((src_width== VIDEO_FRAME_WIDTH_4k) && (src_hight == VIDEO_FRAME_HEIGHT_4k)){
        gpu_image->data_frame_max = DATA_4K_STREAM_FRAME_MAX;
    } else {
        gpu_image->data_frame_max = DATA_STREAM_FRAME_MAX;
    }

    /*判断是否有上次没有释放的*/
    //gpu_chn_image_free(gpu_image);
    /**************************************************************/
    /* 初始化输入的yuv buffer */
    for(index = 0; index < (gpu_image->data_frame_max + 1); index++)
    {
        gpu_image->in_yuv_buffer[index] = (uint8_t*)aligned_alloc(4096, (src_width * src_hight*2));
        if(gpu_image->in_yuv_buffer[index] == NULL){
            printf("[ERROR] osd gpu buffer(%d) acquire error \n", index);
            ret = A818_GUI_OSD_DATA_BUFFER;
            goto err_1;
        }
        memset(gpu_image->in_yuv_buffer[index], 0x0, (src_width * src_hight*2));
    }

    /* 初始化输入的rgb buffer, osd buffer 和输出的yuv buffer */
    for(index = 0; index < gpu_image->data_frame_max; index++)
    {
        gpu_image->in_rgb_buffer[index] = (uint8_t*)aligned_alloc(4096, (src_width * src_hight*3));
        if(gpu_image->in_rgb_buffer[index] == NULL){
            printf("[ERROR] osd gpu buffer(%d) acquire error \n", index);
            ret = A818_GUI_OSD_DATA_BUFFER;
            goto err_2;
        }
        memset(gpu_image->in_rgb_buffer[index], 0x0, (src_width * src_hight*3));

        gpu_image->gui_osd_buffer[index] = (uint8_t*)aligned_alloc(4096, (src_width * src_hight*2));
        if(gpu_image->gui_osd_buffer[index] == NULL){
            printf("[ERROR] osd gpu buffer(%d) acquire error \n", index);
            ret = A818_GUI_OSD_DATA_BUFFER;
            goto err_2;
        }
        memset(gpu_image->gui_osd_buffer[index], 0x0, (src_width * src_hight*2));

        gpu_image->out_yuv_buffer[index] = (uint8_t*)aligned_alloc(4096, (src_width * src_hight*2));
        if(gpu_image->out_yuv_buffer[index] == NULL){
            printf("[ERROR] osd gpu buffer(%d) acquire error \n", index);
            ret = A818_GUI_OSD_DATA_BUFFER;
            goto err_2;
        }
        memset(gpu_image->out_yuv_buffer[index], 0x0, (src_width * src_hight*2));
    }

    gpu_image->frame_size = (src_width * src_hight*2);
    gpu_image->rb_offset = 0;
    gpu_image->data_offset = 0;

    return A818_SUCCESS;
err_2:
    for(index = 0; index < gpu_image->data_frame_max; index++)
    {
        if(gpu_image->out_yuv_buffer[index]){
            free(gpu_image->out_yuv_buffer[index]);
        }

        if(gpu_image->gui_osd_buffer[index]){
            free(gpu_image->gui_osd_buffer[index]);
        }

        if(gpu_image->in_rgb_buffer[index]){
            free(gpu_image->in_rgb_buffer[index]);
        }
    }
err_1:
    for(index = 0; index < (gpu_image->data_frame_max + 1); index++){
        if(gpu_image->in_yuv_buffer[index]){
            free(gpu_image->in_yuv_buffer[index]);
        }
    }

    return ret;

}

/*
	函数: void gpu_chn_ringbuffer_is_empty(unsigned char chn)
	作用: 判断本通道的 ring buffer是否处理完
	参数:
		chn:需要初始化的通道
	返回值:
		无
*/
void gpu_chn_ringbuffer_is_empty(unsigned char chn)
{
   
    GPU_Frame_st* frame_stream = gpu_data_stream_get();
    while(!ringbuffer_is_empty(frame_stream->osd_data_rb[chn])){
        xt_nanosleep(0, 100);
    }
    
    while(!ringbuffer_is_empty(frame_stream->mpp_data_rb[chn])){
        xt_nanosleep(0, 100);
    }
}

/*
	函数: void gpu_osd_chn_image_destory(unsigned char chn)
	作用: 释放通道申请的各种buffer
	参数:
		chn:需要初始化的通道
	返回值:
		无
*/
void gpu_osd_chn_image_destory(unsigned char chn)
{
    int index;
    GPU_Frame_st* frame_stream = gpu_data_stream_get();
    gpu_image_info_t *gpu_image = &frame_stream->gpu_image[chn];

    for(index = 0; index < gpu_image->data_frame_max; index++)
    {
        if(gpu_image->out_yuv_buffer[index]){
            free(gpu_image->out_yuv_buffer[index]);
            gpu_image->out_yuv_buffer[index] = NULL;
        }

        if(gpu_image->gui_osd_buffer[index]){
            free(gpu_image->gui_osd_buffer[index]);
            gpu_image->gui_osd_buffer[index] = NULL;
        }

        if(gpu_image->in_rgb_buffer[index]){
            free(gpu_image->in_rgb_buffer[index]);
            gpu_image->in_rgb_buffer[index] = NULL;
        }
    }

    for(index = 0; index < (gpu_image->data_frame_max + 1); index++){
        if(gpu_image->in_yuv_buffer[index]){
            free(gpu_image->in_yuv_buffer[index]);
            gpu_image->in_yuv_buffer[index] = NULL;
        }
    }

    gpu_image->data_frame_max = 0;
    SLOGI("[DEBUG] %s chn:%d end \n", __func__, chn);
    return ;
}

/*
	函数: int gpu_lib_init(void)
	作用: gpu 初始化
	参数:
		无
	返回值:
		正确返回0，错误返回其他
*/
int gpu_lib_init(void)
{
    int ret;
    int chn;
    int index;

    PATH_TO_HETERO* f_path_to_hetero;

    /* 构建GPU相关结构体 */
    g_gpu_frame_stream = (GPU_Frame_st*)malloc(sizeof(GPU_Frame_st));
    if(!g_gpu_frame_stream){
        printf("[ERROR] malloc GPU frame error \n");
        return A818_GUI_FRAME_MALLOC;
    }

    memset(g_gpu_frame_stream, 0x0, sizeof(GPU_Frame_st));

    void* libhetero = dlopen(RK_GPU_LIB_PATH, RTLD_NOW | RTLD_DEEPBIND);
    if (libhetero == 0) {
        printf("[ERROR] Could not find libhetero.so.\n");
        ret = A818_GUI_DLOPEN;
        goto err_1;
    }

    f_path_to_hetero = (PATH_TO_HETERO*)dlsym(libhetero, "path_to_hetero");
    if (f_path_to_hetero == 0) {
        printf("[ERROR] Loading function failed.\n");
        ret = A818_GUI_DLSYM;
        goto err_1;
    } 

    g_gpu_frame_stream->htr.library_version =HETERO_INTERFACE_VER;
        // 联通异构计算库
    if (f_path_to_hetero(&(g_gpu_frame_stream->htr)) < 0) {
        printf("[ERROR] Library hetero interface not match.\n");
        ret = A818_GUI_LIBRARY;
        goto err_1;
    }

    // 获取异构计算环境句柄
    g_gpu_frame_stream->hetero_hdr = g_gpu_frame_stream->htr.compute_acquire(0);
    if (g_gpu_frame_stream->hetero_hdr == 0) {
        printf("[ERROR] get htr compute acquire error.\n");
        ret = A818_GUI_GET_HDL;
        goto err_1;
    }

    /* 对每个通道的队列进行初始化 */
    for(chn = 0; chn < DATA_CHANNEL_NUM_MAX; chn++){
        g_gpu_frame_stream->osd_data_rb[chn]= ringbuffer_create(RINGBUFFER_FRAME_MAX*sizeof(image_frame_info_t), sizeof(image_frame_info_t));
        if (!g_gpu_frame_stream->osd_data_rb[chn]) {
            printf("ERROR] create osd data rb failed\n");
            ret = A818_GUI_OSD_DATA_RB;
            goto err_2;
        }

        g_gpu_frame_stream->mpp_data_rb[chn]= ringbuffer_create(RINGBUFFER_FRAME_MAX*sizeof(image_frame_info_t), sizeof(image_frame_info_t));
        if (!g_gpu_frame_stream->mpp_data_rb[chn]) {
            printf("ERROR] create mpp data rb failed\n");
            ret = A818_GUI_MPP_DATA_RB;
            goto err_2;
        }
    }


    return A818_SUCCESS;

err_2:
    for(chn = 0; chn < DATA_CHANNEL_NUM_MAX; chn++){
        if(g_gpu_frame_stream->osd_data_rb[chn]){
            ringbuffer_destory(g_gpu_frame_stream->osd_data_rb[chn]); 
            g_gpu_frame_stream->osd_data_rb[chn] = NULL;

            ringbuffer_destory(g_gpu_frame_stream->mpp_data_rb[chn]); 
            g_gpu_frame_stream->mpp_data_rb[chn] = NULL;
        }
    }

    g_gpu_frame_stream->htr.compute_release(g_gpu_frame_stream->hetero_hdr);
err_1:
    free(g_gpu_frame_stream);
    g_gpu_frame_stream = NULL;
    return ret;
}

/*
	函数: void gpu_lib_destory(void)
	作用: gpu 释放
	参数:
		无
	返回值:
		无
*/
void gpu_lib_destory(void)
{
    int chn;
    GPU_Frame_st* gpu_frame_stream = gpu_data_stream_get();
    if(gpu_frame_stream){
        for(chn = 0; chn < DATA_CHANNEL_NUM_MAX; chn++){
            if(gpu_frame_stream->osd_data_rb[chn]){
                ringbuffer_destory(gpu_frame_stream->osd_data_rb[chn]); 
                gpu_frame_stream->osd_data_rb[chn] = NULL;

                ringbuffer_destory(g_gpu_frame_stream->mpp_data_rb[chn]); 
                g_gpu_frame_stream->mpp_data_rb[chn] = NULL;
            }
        }
        
        gpu_frame_stream->htr.compute_release(gpu_frame_stream->hetero_hdr);
        free(gpu_frame_stream);
        gpu_frame_stream = NULL;
    }

    return ;
}
