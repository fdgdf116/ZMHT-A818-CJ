#include "stream_lifecycle.h"
/*
 * File      : data_engine.c
 * This file is data engine file  
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-1-4     fengyong    first version
 */

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

#include "types_def.h"
#include "data_engine.h"
#include "gpu_interface.h"
#include "xutils.h"
#include "log.h"
#include "rga_interface.h"
#include "pcie_reg_rw.h"
#include "a818_error.h"

#include "opencv2/core.hpp"
#include "opencv2/opencv.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/features2d.hpp"

using namespace cv;
using namespace std;

static data_stream_t *gs_data_stream = NULL;
static Mat output_image;
static unsigned char output_image_memset[VIDEO_FRAME_HEIGHT * VIDEO_FRAME_WIDTH * 3] = {0};

data_stream_t *data_stream_get(void)
{
    return gs_data_stream;
}

static void data_stream_stitchbase_image_to_queue(unsigned char chn, image_frame_info_t *frame_info)
{
    image_frame_info_t event_param, tmp_param;
    data_stream_t *data_stream = data_stream_get();
    ring_buffer_t *rb = data_stream->chn_image[chn]->stitchbase_data_rb;
	
	if (ringbuffer_is_full(rb)) {					
		ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));
        SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}
	memcpy(&event_param,frame_info,sizeof(image_frame_info_t));
    		
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}
/*
	函数: int data_stream_fill_chn_resize_info(unsigned char chn)
	作用: 计算拼接小图通道所需要的各种信息
	参数:
		小图通道
	返回值:
		正确返回0，错误返回其他
*/
int data_stream_fill_chn_resize_info(unsigned char chn)
{
    unsigned int resize_width;
    unsigned int resize_height;
    unsigned int width;
    unsigned int height;

    data_stream_t *data_stream = data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];

    chn_image->rga_mode = RGB_BUFF_MODE;

    /*判断是不是小图*/
    if(chn < PCIE_SRC_START_CHANNEL){

        /*判断是那种拼接模式，从而获取到拼接的最大的小图宽高*/
        switch (chn_image->ratio_info.mode)
        {
            case VIDEO_RATIO_1:
                resize_width = VIDEO_FRAME_WIDTH;
                resize_height = VIDEO_FRAME_HEIGHT;
                break;

            case VIDEO_RATIO_3:
            case VIDEO_RATIO_9:
                resize_width = VIDEO_RATIO_3x3_WIDTH;
                resize_height = VIDEO_RATIO_3x3_HEIGHT;
                break;

            case VIDEO_RATIO_4:
                resize_width = VIDEO_RATIO_2x2_WIDTH;
                resize_height = VIDEO_RATIO_2x2_HEIGHT;
                break;

            case VIDEO_RATIO_6:
                resize_width = VIDEO_RATIO_3x2_WIDTH;
                resize_height = VIDEO_RATIO_3x2_HEIGHT;
                break;

            default:
                printf("fpga ratio %d error \n", chn_image->ratio_info.mode);
                return A818_DATA_RATIO_TYPE;
        }

        data_stream->ratio = chn_image->ratio_info.mode;

        width = chn_image->resize_info.src_width = chn_image->ratio_info.width;
        height = chn_image->resize_info.src_height = chn_image->ratio_info.height;

        /*判断是需不需进行旋转*/
        if((chn_image->ratio_info.rotate != 0)){
            chn_image->rga_mode = ROTATE_BUFF_MODE;
            /*如果是90度和270度的话，宽高需要对换*/
            if((chn_image->ratio_info.rotate == 1) || (chn_image->ratio_info.rotate == 4)){
                height = chn_image->resize_info.src_width;
                width = chn_image->resize_info.src_height;
            }
        }

        SLOGI("[debug] (%d-%d) (%d-%d)\n", width, height, chn_image->resize_info.src_width, chn_image->resize_info.src_height);


        chn_image->resize_info.dst_width = width;
        chn_image->resize_info.dst_height = height;
        /*判断宽或者高是不是比最大的宽或者高大，如果大的话就需要进行缩放*/
        if((width > resize_width) || (height > resize_height)){
            if(width > resize_width){
                chn_image->resize_info.dst_width = resize_width;
            }else {
                chn_image->resize_info.dst_width = width;
            }
            
            if(height > resize_height){
                chn_image->resize_info.dst_height = resize_height;
            } else {
                chn_image->resize_info.dst_height = height;
            }

            chn_image->rga_mode = RESIZE_BUFF_MODE;
        }

        /*1920x1080这个比较的特殊，因为rga需要对齐操作，所以需要将填充的部分去掉*/
        if((chn_image->ratio_info.width == VIDEO_SPECIAL_FRAME_WIDTH) && (chn_image->ratio_info.height == VIDEO_SPECIAL_FRAME_HEIGHT)){
            chn_image->chn_data.frame_size = (VIDEO_FRAME_WIDTH * VIDEO_FRAME_HEIGHT * 2);
            if((chn_image->ratio_info.rotate == 0)){
                chn_image->rga_mode = RGB_BUFF_MODE;
            }else {
                //chn_image->resize_info.dst_width = 1080;
            }
        } else {
            chn_image->chn_data.frame_size = chn_image->ratio_info.width * chn_image->ratio_info.height * 2;  
        }
    
    } else {
        chn_image->resize_info.src_width = chn_image->mpp_info.width;
        chn_image->resize_info.src_height = chn_image->mpp_info.height;
        chn_image->resize_info.dst_width = chn_image->mpp_info.width;
        chn_image->resize_info.dst_height = chn_image->mpp_info.height;
        chn_image->chn_data.frame_size = chn_image->mpp_info.width * chn_image->mpp_info.height * 2;
    }

    chn_image->chn_data.rb_offset = 0;
    chn_image->chn_data.data_offset = 0;

    return A818_SUCCESS;
}

/*
	函数: unsigned int data_stream_low_chn_data_init(unsigned char chn)
	作用: 对拼接小图的通道进行初始化
	参数:
		小图通道
	返回值:
		正确返回0，错误返回其他
*/
unsigned int data_stream_low_chn_data_init(unsigned char chn)
{
    int ret, index;
    unsigned int buffer_len;
    unsigned int ret_val = 0;
    data_stream_t *data_stream = data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];

    ret_val = 0;

    chn_image->chn = chn;
    chn_image->rga_mode = 0;
    chn_image->init_flag[DATA_INIT] = 0;
    chn_image->frame_num = 0;

    memset(&chn_image->chn_data, 0x0, sizeof(chn_image->chn_data));
    chn_image->chn_data.data_offset = 0;
    chn_image->chn_data.frame_size = 0;
    chn_image->chn_data.rb_offset = 0;
    
    /*计算本通道小图的各种信息*/
    ret = data_stream_fill_chn_resize_info(chn);
    if(ret != 0){
        SLOGI("[ERROR] chn resize info init error \n");
        return ret;
    }

    SLOGI("[DEBUG] %s chn:%d frame_size:%d (%d-%d-%d-%d)\n", __func__, chn, chn_image->chn_data.frame_size,chn_image->resize_info.src_width,chn_image->resize_info.src_height,\
        chn_image->resize_info.dst_width, chn_image->resize_info.dst_height);

    /*初始化各种缓存的申请，这里将输入的yuv数据多申请一帧的图像，因为防止在特殊情况下(ring buffer满)，导致
        缓冲区图像被覆盖
    */
    for(index = 0; index < (chn_image->data_frame_max + 1); index++){
        buffer_len = get_rga_vir_buffer_len(RK_FORMAT_YUYV_422, chn_image->resize_info.src_width,chn_image->resize_info.src_height);
        ret = rga_comm_create_image2(&chn_image->chn_data.yuv422_data[index],data_stream->rga_vir_addr +  data_stream->rga_addr_offset, RK_FORMAT_YUYV_422, \
            chn_image->resize_info.src_width,chn_image->resize_info.src_height);
        if(ret != 0) {
            SLOGI("[ERROR] int rga yuv422_data[%d] error \n", index);
            ret_val = A818_DATA_YUV_DATA;
            ret_val |= (ret << 8);
            goto err_1;
        }

        data_stream->rga_addr_offset += buffer_len;
        if(data_stream->rga_addr_offset >= data_stream->rga_addr_offset_max){
            SLOGI("[ERROR] int rga yuv422_data[%d] rga_addr_offset error \n", index);
            ret_val = A818_DATA_RGA_ADDR_OFFSET;
            goto err_1;
        }
    }

    for(index = 0; index < chn_image->data_frame_max; index++){
        buffer_len = get_rga_vir_buffer_len(RK_FORMAT_RGB_888, chn_image->resize_info.src_width,chn_image->resize_info.src_height);
        ret = rga_comm_create_image2(&chn_image->chn_data.rgb888_data[index],data_stream->rga_vir_addr +  data_stream->rga_addr_offset,RK_FORMAT_RGB_888, \
            chn_image->resize_info.src_width,chn_image->resize_info.src_height);
        if(ret != 0){
            printf("[ERROR] int rga rgb888_data[%d] error \n", index);
            ret_val = A818_DATA_RGB_DATA;
            ret_val |= (ret << 8);
            goto err_2;
        }
        data_stream->rga_addr_offset += buffer_len;
        if(data_stream->rga_addr_offset >= data_stream->rga_addr_offset_max){
            SLOGI("[ERROR] int rga yuv422_data[%d] rga_addr_offset error \n", index);
            ret_val = A818_DATA_RGA_ADDR_OFFSET;
            goto err_1;
        }

        buffer_len = get_rga_vir_buffer_len(RK_FORMAT_RGB_888, chn_image->resize_info.src_width,chn_image->resize_info.src_height);
        ret = rga_comm_create_image2(&chn_image->chn_data.rotate_data[index],data_stream->rga_vir_addr +  data_stream->rga_addr_offset,RK_FORMAT_RGB_888, \
            chn_image->resize_info.src_width,chn_image->resize_info.src_height);
        if(ret != 0){
            printf("[ERROR] int rga rotate_data[%d] error \n", index);
            ret_val = A818_DATA_ROTATE_DATA;
            ret_val |= (ret << 8);
            goto err_2;
        }
        data_stream->rga_addr_offset += buffer_len;
        if(data_stream->rga_addr_offset >= data_stream->rga_addr_offset_max){
            SLOGI("[ERROR] int rga yuv422_data[%d] rga_addr_offset error \n", index);
            ret_val = A818_DATA_RGA_ADDR_OFFSET;
            goto err_1;
        }


        buffer_len = get_rga_vir_buffer_len(RK_FORMAT_RGB_888, chn_image->resize_info.dst_width, chn_image->resize_info.dst_height);
        ret = rga_comm_create_image2(&chn_image->chn_data.resize_data[index],data_stream->rga_vir_addr +  data_stream->rga_addr_offset,RK_FORMAT_RGB_888, \
            chn_image->resize_info.dst_width, chn_image->resize_info.dst_height);
        if(ret != 0){
            printf("[ERROR] int rga resize_data[%d] error \n", index);
            ret_val = A818_DATA_RESIZE_DATA;
            ret_val |= (ret << 8);
            goto err_2;
        }
        data_stream->rga_addr_offset += buffer_len;
        if(data_stream->rga_addr_offset >= data_stream->rga_addr_offset_max){
            SLOGI("[ERROR] int rga yuv422_data[%d] rga_addr_offset error \n", index);
            ret_val = A818_DATA_RGA_ADDR_OFFSET;
            goto err_1;
        }
    }
   
    usleep(50*1000);

    return A818_SUCCESS;

err_2:
    for(index = 0; index < chn_image->data_frame_max; index++){
        if(chn_image->chn_data.resize_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.resize_data[index]);
            chn_image->chn_data.resize_data[index].vir_addr = NULL;
        }

        if(chn_image->chn_data.rotate_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.rotate_data[index]);
            chn_image->chn_data.rotate_data[index].vir_addr = NULL;
        }

        if(chn_image->chn_data.rgb888_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.rgb888_data[index]);
            chn_image->chn_data.rgb888_data[index].vir_addr = NULL;
        }
    }
err_1:

    for(index = 0; index < (chn_image->data_frame_max + 1); index++){
        if(chn_image->chn_data.yuv422_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.yuv422_data[index]);
        }
    }
    data_stream->rga_addr_offset = 0;
    return ret_val;
}


/*
	函数: void data_stream_chn_ringbuffer_is_empty(unsigned char chn)
	作用: 判断本通道的ring buffer是否被使用完成
	参数:
		小图通道
	返回值:
		无
*/
void data_stream_chn_ringbuffer_is_empty(unsigned char chn)
{
    data_stream_t *data_stream = data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];

    while(!ringbuffer_is_empty(chn_image->chn_data_rb)){
        xt_nanosleep(0, 100);
    }

    while(!ringbuffer_is_empty(chn_image->stitchbase_data_rb)){
        xt_nanosleep(0, 100);
    }
}

/*
	函数: void data_stream_low_chn_data_destory(unsigned char chn)
	作用: 对通道小图的资源进行释放
	参数:
		小图通道
	返回值:
		无
*/
void data_stream_low_chn_data_destory(unsigned char chn)
{
    int index;
    data_stream_t *data_stream = data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];

    data_stream_chn_ringbuffer_is_empty(chn);

    for(index = 0; index < chn_image->data_frame_max; index++){
        if(chn_image->chn_data.resize_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.resize_data[index]);
            chn_image->chn_data.resize_data[index].vir_addr = NULL;
        }

        if(chn_image->chn_data.rotate_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.rotate_data[index]);
            chn_image->chn_data.rotate_data[index].vir_addr = NULL;
        }

        if(chn_image->chn_data.rgb888_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.rgb888_data[index]);
            chn_image->chn_data.rgb888_data[index].vir_addr = NULL;
        }
    }

    for(index = 0; index < (chn_image->data_frame_max + 1); index++){
        if(chn_image->chn_data.yuv422_data[index].vir_addr){
            rga_release_frame_image2(&chn_image->chn_data.yuv422_data[index]);
            chn_image->chn_data.yuv422_data[index].vir_addr = NULL;
        }
    }

    chn_image->data_frame_max = 0;
    data_stream->rga_addr_offset = 0;
    data_stream->stitchbase_chn_info[chn].flag = 0;

    SLOGI("[DEBUG] %s chn:%d end \n", __func__, chn);
}

/*
	void data_stream_ratio1_chage_src_to_new(unsigned int index, unsigned char *src, unsigned char *dec)
	作用: 对1920x1080图像旋转进行单独的赋值
	参数:
		index:旋转的标志,90度还是270度
        src：源图像buffer
        dec：目的图像buffer

	返回值:
		无
*/
void data_stream_ratio1_chage_src_to_new(unsigned int index, unsigned char *src, unsigned char *dec)
{
	int i = 0;
    unsigned int offset;

    if(!index){
        offset = 0;
    }else {
        offset = 8;/*因为对齐填充宽/高*/
    }

	for(i = 0; i < 1080; i++) {
        memcpy(dec + (i*1920*3), src + (i *1088*3) + (offset*3), 1080*3);
	}

	return ;
}

/*
	void static void data_stream_ratio_image_stitchbase(unsigned char chn, int rb_offset)
	作用: 进行图像的拼接
	参数:
		chn:小图通道
        rb_offset:小图数据的rb 偏移
	返回值:
		无
*/

static void data_stream_ratio_image_stitchbase(unsigned char chn, int rb_offset)
{
    int ret;
    unsigned int x;
    unsigned int y;
    unsigned int width;
    unsigned int height;
    unsigned char *rgb_data;
    data_stream_t *data_stream = data_stream_get();
    GPU_Frame_st *gpu_frame_stream = gpu_data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];
    unsigned char position = chn_image->ratio_info.position;
    width = chn_image->resize_info.dst_width;
    height = chn_image->resize_info.dst_height;
    
    /* 根据缩放比和在的位置，算出来所在的x和y以及对应的宽高 */
    switch(data_stream->ratio){
        case VIDEO_RATIO_1:
            x = 0;
            y = 0;
        break;

        case VIDEO_RATIO_3:
        case VIDEO_RATIO_9:
            x = (position % 3) * VIDEO_RATIO_3x3_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x3_HEIGHT;
        break;

        case VIDEO_RATIO_4:
            x = (position % 2) * VIDEO_RATIO_2x2_WIDTH;
            y = (position / 2) * VIDEO_RATIO_2x2_HEIGHT;
        break;

        case VIDEO_RATIO_6:
            x = (position % 3) * VIDEO_RATIO_3x2_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x2_HEIGHT;
        break;
    }
    data_stream->stitchbase_chn_info[chn].flag = 1;
    data_stream->stitchbase_chn_info[chn].x = x;
    data_stream->stitchbase_chn_info[chn].y = y;

    /*根据不同的模式从不同的buffer中取出数据*/
    switch(chn_image->rga_mode){
        case RGB_BUFF_MODE:
            rgb_data = chn_image->chn_data.rgb888_data[rb_offset].vir_addr;
        break;
        case ROTATE_BUFF_MODE:
            rgb_data = chn_image->chn_data.rotate_data[rb_offset].vir_addr;
        break;
        case RESIZE_BUFF_MODE:
            rgb_data = chn_image->chn_data.resize_data[rb_offset].vir_addr;
        break;
        default:
            printf("get rga mode error \n");
            return ;
    }
    
    /*因为有1080p的，所以需要进行特殊的处理*/
    if((chn_image->ratio_info.width == VIDEO_SPECIAL_FRAME_WIDTH) && (chn_image->ratio_info.height == VIDEO_SPECIAL_FRAME_HEIGHT)){
        if ((chn_image->ratio_info.rotate == 1)){
           data_stream_ratio1_chage_src_to_new(1, rgb_data, (uint8_t*)output_image.data);
        } else if((chn_image->ratio_info.rotate == 4)) {
            data_stream_ratio1_chage_src_to_new(0, rgb_data, (uint8_t*)output_image.data);
        }else {
            Mat tmp_image(height, width, CV_8UC3, rgb_data);
            Mat fuse_image = output_image(Rect(x, y, width, height));
            tmp_image.copyTo(fuse_image);  
        }
    } else {
        Mat tmp_image(height, width, CV_8UC3, rgb_data);
        Mat fuse_image = output_image(Rect(x, y, width, height));
        tmp_image.copyTo(fuse_image);  
    } 
    

    // Mat tmp_image(height, width, CV_8UC3, rgb_data);
    // Mat fuse_image = output_image(Rect(x, y, width, height));
    // tmp_image.copyTo(fuse_image);
    
    return ;
};


static void data_stream_set_stitchbase_chn_info(unsigned char chn)
{
    unsigned int x;
    unsigned int y;
    data_stream_t *data_stream = data_stream_get();
    GPU_Frame_st *gpu_frame_stream = gpu_data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];
    unsigned char position = chn_image->ratio_info.position;

    
    /* 根据缩放比和在的位置，算出来所在的x和y以及对应的宽高 */
    switch(data_stream->ratio){
        case VIDEO_RATIO_1:
            x = 0;
            y = 0;
        break;

        case VIDEO_RATIO_3:
        case VIDEO_RATIO_9:
            x = (position % 3) * VIDEO_RATIO_3x3_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x3_HEIGHT;
        break;

        case VIDEO_RATIO_4:
            x = (position % 2) * VIDEO_RATIO_2x2_WIDTH;
            y = (position / 2) * VIDEO_RATIO_2x2_HEIGHT;
        break;

        case VIDEO_RATIO_6:
            x = (position % 3) * VIDEO_RATIO_3x2_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x2_HEIGHT;
        break;
    }
    data_stream->stitchbase_chn_info[chn].flag = 1;
    data_stream->stitchbase_chn_info[chn].x = x;
    data_stream->stitchbase_chn_info[chn].y = y;
}


/*
	void static void data_stream_no_signal_image_stitchbase(unsigned char position)
	作用: 对no sognal区域进行填充
	参数:
        position：填充的位置
	返回值:
		无
*/


static void data_stream_no_signal_image_stitchbase(unsigned char position)
{
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    char filename[128];
    data_stream_t *data_stream = data_stream_get();
    
    /* 根据缩放比和在的位置，算出来所在的x和y以及对应的宽高 */
    switch(data_stream->ratio){
        case VIDEO_RATIO_1:
            x = 0;
            y = 0;
            width = VIDEO_FRAME_WIDTH;
            height = VIDEO_FRAME_HEIGHT;
        break;
        case VIDEO_RATIO_3:
        case VIDEO_RATIO_9:
            x = (position % 3) * VIDEO_RATIO_3x3_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x3_HEIGHT;
            width = VIDEO_RATIO_3x3_WIDTH;
            height = VIDEO_RATIO_3x3_HEIGHT;
        break;
        case VIDEO_RATIO_4:
            x = (position % 2) * VIDEO_RATIO_2x2_WIDTH;
            y = (position / 2) * VIDEO_RATIO_2x2_HEIGHT;
            width = VIDEO_RATIO_2x2_WIDTH;
            height = VIDEO_RATIO_2x2_HEIGHT;
        break;
        case VIDEO_RATIO_6:
            x = (position % 3) * VIDEO_RATIO_3x2_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x2_HEIGHT;

            width = VIDEO_RATIO_3x2_WIDTH;
            height = VIDEO_RATIO_3x2_HEIGHT;
        break;
    }
    /*打开对应的no signal 文件*/
    sprintf(filename, "/tmp/real_part/data/NOSIGNAL/%dx%d/%d.Raw", width, height, data_stream->no_signal_index);
    read_data_to_buf(data_stream->no_signal_buffer, filename);

    Mat tmp_image(height, width, CV_8UC3, data_stream->no_signal_buffer);
    Mat fuse_image = output_image(Rect(x, y, width, height));
    tmp_image.copyTo(fuse_image);

    return ;
};


static void data_stream_no_signal_image_stitchbase_init(unsigned char position)
{
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    data_stream_t *data_stream = data_stream_get();
    /* 根据缩放比和在的位置，算出来所在的x和y以及对应的宽高 */
    switch(data_stream->ratio){
        case VIDEO_RATIO_1:
            x = 0;
            y = 0;
            width = VIDEO_FRAME_WIDTH;
            height = VIDEO_FRAME_HEIGHT;
        break;
        case VIDEO_RATIO_3:
        case VIDEO_RATIO_9:
            x = (position % 3) * VIDEO_RATIO_3x3_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x3_HEIGHT;
            width = VIDEO_RATIO_3x3_WIDTH;
            height = VIDEO_RATIO_3x3_HEIGHT;
        break;
        case VIDEO_RATIO_4:
            x = (position % 2) * VIDEO_RATIO_2x2_WIDTH;
            y = (position / 2) * VIDEO_RATIO_2x2_HEIGHT;
            width = VIDEO_RATIO_2x2_WIDTH;
            height = VIDEO_RATIO_2x2_HEIGHT;
        break;
        case VIDEO_RATIO_6:
            x = (position % 3) * VIDEO_RATIO_3x2_WIDTH;
            y = (position / 3) * VIDEO_RATIO_3x2_HEIGHT;

            width = VIDEO_RATIO_3x2_WIDTH;
            height = VIDEO_RATIO_3x2_HEIGHT;
        break;
    }

    Mat tmp_image(height, width, CV_8UC3, output_image_memset);
    Mat fuse_image = output_image(Rect(x, y, width, height));
    tmp_image.copyTo(fuse_image);
}

/*
	函数：static void *data_stream_image_stitchbase_thread(void* parameter)
	作用: 拼接线程
	参数:
        parameter:传入的线程参数
	返回值:
		线程返回值
*/

static int stitchbase_thread_created;
static void *data_stream_image_stitchbase_thread(void* parameter)
{

    ring_buffer_t *rb;
    ring_buffer_t *osd_rb;
    gpu_image_info_t *gpu_image;
    fpga_chn_image_info_t* chn_image;
    unsigned long stitchbase_time, sleep_time, timeout;
    unsigned long timeout_m_max1= (15000000);
    unsigned long timeout_m_max2= (16300000);
    unsigned long timeout_m_max = (16300000);//因为是60fps，一帧的最大的时间;

    unsigned int frame_cnt = 0;

    image_frame_info_t frame_info = {0};
    image_frame_info_t gpu_frame_info = {0};

    data_stream_t *data_stream = data_stream_get();
    GPU_Frame_st *gpu_frame_stream = gpu_data_stream_get();
    if((!data_stream) || (!gpu_frame_stream)){
        printf("get data error \n");
        return NULL;
    }
    struct timeval time;
    pcie_timestamp_t t0, t1, t2, t3;

    frame_cnt = 0;
    data_stream->stitchbase_open = 0;


    unsigned char stitchbase_count[DATA_RATIO_CHANNEL_MAX];

    memset(stitchbase_count, 0x0, sizeof(stitchbase_count));

    while(1){

        if(!__atomic_load_n(&data_stream->stitchbase_stop, __ATOMIC_ACQUIRE)) {
            break;
        }

        STREAM_WORK_GUARD(work, STREAM_WORK);
        if (!work.active) { usleep(1000); continue; }
        stitchbase_time = 0;

        if((frame_cnt %30) == 0){
            timeout_m_max = timeout_m_max2;
        } else {
            timeout_m_max = timeout_m_max1;
        }

        get_pcie_timestamp(&t2);
        pthread_mutex_lock(&data_stream->swap_lock);
        get_pcie_timestamp(&t0);

    #if 0
        /*循环遍历每个通道，有通道数据就进行拼接*/
        for(int chn = 0; chn < DATA_RATIO_CHANNEL_MAX; chn++){
            chn_image = data_stream->chn_image[chn];
            rb = chn_image->stitchbase_data_rb;
            if(!ringbuffer_is_empty(rb)){
                ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));
                data_stream_ratio_image_stitchbase(chn, frame_info.frame_offset); /*进行拼接*/
                data_stream->stitchbase_open = 1;
            }
        }
    #else
        /*循环遍历每个通道，有通道数据就进行拼接*/
        for(int chn = 0; chn < DATA_RATIO_CHANNEL_MAX; chn++){
            chn_image = data_stream->chn_image[chn];
            rb = chn_image->stitchbase_data_rb;
            if(!ringbuffer_is_empty(rb)){
                ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));
                if(stitchbase_count[chn] >= TIMEOUT_FRAME_COUNT){
                    data_stream_no_signal_image_stitchbase_init(chn_image->ratio_info.position);
                }
                data_stream_ratio_image_stitchbase(chn, frame_info.frame_offset); /*进行拼接*/
                //data_stream->stitchbase_open = 1;
                stitchbase_count[chn] = 0;
                data_stream->no_signal_position &= ~(1 << chn_image->ratio_info.position);
            } else {
                stitchbase_count[chn] += 1;
                if(stitchbase_count[chn] >= TIMEOUT_FRAME_COUNT){
                    stitchbase_count[chn] = TIMEOUT_FRAME_COUNT;
                }
            }
        }
    #endif

        get_pcie_timestamp(&t1);
        stitchbase_time += pcie_diff_timestamp(&t1,&t0);

        if(data_stream->stitchbase_open){
            /*进行no signal的拼接*/
            if(((frame_cnt % 10) == 0)) {
            //if(((frame_cnt % 10) == 0) && (data_stream->ratio != 1)) {
                if(data_stream->no_signal_index == NO_SIGNAL_BUF_MAX){
                    data_stream->no_signal_flag = 1;
                } else if(data_stream->no_signal_index == 0){
                    data_stream->no_signal_flag = 0;
                }

                for(int chn = 0; chn < DATA_RATIO_CHANNEL_MAX; chn++){
                    chn_image = data_stream->chn_image[chn];
                    if((stitchbase_count[chn] >= TIMEOUT_FRAME_COUNT) && (chn_image->ratio_info.position != 0xff)){
                        data_stream->no_signal_position |= (1 << chn_image->ratio_info.position);
                        data_stream_set_stitchbase_chn_info(chn);
                    }
                }

                for(int i = 0; i < VIDEO_RATIO_9; i++){
                    get_pcie_timestamp(&t0);
                    if(((data_stream->no_signal_position >> i) & 1) == 1){
                        data_stream_no_signal_image_stitchbase(i);
                    }
                    get_pcie_timestamp(&t1);
                    stitchbase_time += pcie_diff_timestamp(&t1,&t0);
                    /*超过了最大的时间，则不进行后续no signal的拼接*/
                    if(stitchbase_time >= timeout_m_max){
                        break;
                    }
                } 

                if(!data_stream->no_signal_flag){
                    data_stream->no_signal_index += 1;
                } else {
                    data_stream->no_signal_index -= 1;
                }
            }

            gpu_image = &gpu_frame_stream->gpu_image[GPU_STITCHBASE_CHN];
            osd_rb = gpu_frame_stream->osd_data_rb[GPU_STITCHBASE_CHN];

            if((!ringbuffer_is_full(osd_rb))){
                if(gpu_image->osd_rb_offset >= gpu_image->data_frame_max){
                    gpu_image->osd_rb_offset = 0;
                }

                /*将拼接后的图像进行拷贝，后续进行osd的叠加*/
                memcpy(gpu_image->in_rgb_buffer[gpu_image->osd_rb_offset],(uint8_t*)output_image.data,(VIDEO_FRAME_WIDTH*VIDEO_FRAME_HEIGHT*3));
                gpu_frame_info.frame_offset = gpu_image->osd_rb_offset;
                gpu_frame_info.chn = GPU_STITCHBASE_CHN;
                gpu_frame_info.t0 = what_time_is_it_now();
                gpu_osd_to_queue(GPU_STITCHBASE_CHN, &gpu_frame_info); /* 送入字符叠加 */
                gpu_image->osd_rb_offset += 1;
                frame_cnt += 1;
            } else {
                SLOGI("[DEBUG] %s ringbuffer_is_full \n", __func__);
            }
        }

        pthread_mutex_unlock(&data_stream->swap_lock);
        get_pcie_timestamp(&t3);
        timeout = pcie_diff_timestamp(&t3,&t2);
    #if 0
        while(1){
            get_pcie_timestamp(&t0);
            // time.tv_sec=0;
            // time.tv_usec=1;
            // select(0,NULL,NULL,NULL,&time);
            xt_nanosleep(0, 1000);
            get_pcie_timestamp(&t1);

            timeout += pcie_diff_timestamp(&t1,&t0);
            if(timeout >= timeout_m_max){
                break;
            }
        }

    #else
        /*没有达到最大的时间，则进行等待*/
        if(timeout < timeout_m_max){
            //sleep_time = ((timeout_m_max - timeout) / 1000);
            //xt_nanosleep(0, sleep_time);
            sleep_time = (timeout_m_max - timeout);
            sleep_time /= 1000;
            time.tv_sec=0;
            time.tv_usec=(sleep_time);
            select(0,NULL,NULL,NULL,&time);
        }
    #endif
    }

    SLOGI("[DEBUG] %s stop \n", __func__);

    return NULL;
}
/*
	函数：void rgb888_output_image_init(unsigned int rgb)
	作用: 拼接背景色的填充
	参数:
        rgb:背景色
	返回值:
		无
*/
void rgb888_output_image_init(unsigned int rgb)
{
    data_stream_t *data_stream = data_stream_get();
    char r = (rgb >> 16) & 0xff;
    char g = (rgb >> 8) & 0xff;
    char b = rgb & 0xff;
    int size = VIDEO_FRAME_HEIGHT * VIDEO_FRAME_WIDTH * 3;

    for(int i = 0; i < (size / 3); i++){
        output_image.data[i*3] = r;
        output_image.data[i*3 + 1] = g;
        output_image.data[i*3 + 2] = b;
    }

    /*填充no signal*/
    for(int i = 0; i < VIDEO_RATIO_9; i++){
        if(((data_stream->no_signal_position >> i) & 1) == 1){
            data_stream_no_signal_image_stitchbase(i);
        }
    } 
}

#if 0
static void *data_stream_low_chn_rga_thread(void* parameter)
{
	unsigned char data_chn;
    struct timeval time;

    ring_buffer_t *rb;
    ring_buffer_t *stitchbase_rb;
    unsigned int width;
    unsigned int height;

    fpga_chn_image_info_t* chn_image;
    GPU_Frame_st *gpu_frame_stream;
    image_frame_info_t frame_info = {0};
    image_frame_info_t stitchbase_frame_info = {0};
    gpu_frame_stream = gpu_data_stream_get();
    data_stream_t *data_stream = data_stream_get();


    data_chn = *((unsigned char*)parameter);
    SLOGI("[debug] %s %d chn:%d \n", __func__, __LINE__, data_chn);

    rb = data_stream->chn_image[data_chn]->chn_data_rb; /* PCIE通道的rb */
    chn_image = data_stream->chn_image[data_chn];
    stitchbase_rb = chn_image->stitchbase_data_rb;


    while(1){

        if(!chn_image->stop_flag){
            break;
        }
        
        STREAM_WORK_GUARD(work, STREAM_WORK);
        if (!work.active) { usleep(1000); continue; }
        if (ringbuffer_is_empty(rb)) { usleep(1); continue; }
        ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));

        if(chn_image->rga_rb_offset >= chn_image->data_frame_max){
            chn_image->rga_rb_offset = 0;
        }

        pthread_mutex_lock(&data_stream->rga_lock);
        
        /* 通过rga将yuv转换成rgb888, 如果不旋转，则会直接进行后续的拼接*/
        rga_cvtColor(&chn_image->chn_data.yuv422_data[frame_info.frame_offset],\
            &chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset]);

        width = chn_image->resize_info.src_width;
        height = chn_image->resize_info.src_height;

        /* 判断是否进行旋转，进行旋转需要先旋转在缩放 */
        if((chn_image->ratio_info.rotate != 0)){
            /* 进行旋转 */
            chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.width = chn_image->resize_info.src_width;
            chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.height = chn_image->resize_info.src_height;
            chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.wstride = chn_image->resize_info.src_width;
            chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.hstride = chn_image->resize_info.src_height;
            
            rga_rotate(&chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset], &chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], \
                chn_image->ratio_info.rotate);
        }

        if(chn_image->rga_mode == RESIZE_BUFF_MODE){
            chn_image->chn_data.resize_data[chn_image->rga_rb_offset].Width = chn_image->resize_info.dst_width;
            chn_image->chn_data.resize_data[chn_image->rga_rb_offset].Height = chn_image->resize_info.dst_height;
            if((chn_image->ratio_info.rotate != 0)){
                if((chn_image->ratio_info.width == VIDEO_SPECIAL_FRAME_WIDTH) && (chn_image->ratio_info.height == VIDEO_SPECIAL_FRAME_HEIGHT)){
                    if(chn_image->ratio_info.rotate == 2){
                        memcpy(chn_image->chn_data.resize_data[chn_image->rga_rb_offset].vir_addr, chn_image->chn_data.rotate_data[chn_image->rga_rb_offset].vir_addr + (8*1920*3),
                            (chn_image->resize_info.dst_width*chn_image->resize_info.dst_height*3));
                    } else {
                        rga_resize(&chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
                    }

                } else {
                    rga_resize(&chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
                }

                //rga_resize(&chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
            } else {
                rga_resize(&chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
            }
        }

        if(!ringbuffer_is_full(stitchbase_rb)){
            if(data_stream->stitchbase_stop){
                stitchbase_frame_info.chn = data_chn;
                stitchbase_frame_info.frame_offset = chn_image->rga_rb_offset;
                data_stream_stitchbase_image_to_queue(data_chn, &stitchbase_frame_info);
                chn_image->rga_rb_offset += 1;
            } else {
                SLOGI("[ERROR] stitchbase THREAD ERROR \n");
            }

        } else {
            SLOGI("[DEBUG] %s %d ringbuffer_is_full \n", __func__, __LINE__);
        }

        pthread_mutex_unlock(&data_stream->rga_lock);
    }

stop:
    SLOGI("[Debug] chn:%d %s exit \n", data_chn, __func__);
    return NULL;
}

#else
/*
	函数: static void *data_stream_low_chn_rga_thread(void* parameter)
	作用: 小图处理函数
	参数:
		线程参数
	返回值:
		线程返回值
*/
static void *data_stream_low_chn_rga_thread(void* parameter)
{
    unsigned char data_chn;
    struct timeval time;

    ring_buffer_t *rb;
    ring_buffer_t *stitchbase_rb;
    unsigned int width;
    unsigned int height;

    fpga_chn_image_info_t* chn_image;
    GPU_Frame_st *gpu_frame_stream;
    image_frame_info_t frame_info = {0};
    image_frame_info_t stitchbase_frame_info = {0};
    gpu_frame_stream = gpu_data_stream_get();
    data_stream_t *data_stream = data_stream_get();

    while(1){
        STREAM_WORK_GUARD(work, STREAM_WORK);
        if (!work.active) { usleep(1000); continue; }
        for(data_chn = 0; data_chn < DATA_RATIO_CHANNEL_MAX; data_chn++){
            rb = data_stream->chn_image[data_chn]->chn_data_rb; /* PCIE通道的rb */
            chn_image = data_stream->chn_image[data_chn];
            stitchbase_rb = chn_image->stitchbase_data_rb;
            if(!ringbuffer_is_empty(rb)) {
                ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));

                if(chn_image->rga_rb_offset >= chn_image->data_frame_max){
                    chn_image->rga_rb_offset = 0;
                }

                pthread_mutex_lock(&data_stream->rga_lock);
                
                /* 通过rga将yuv转换成rgb888, 如果不旋转，则会直接进行后续的拼接*/
                rga_cvtColor(&chn_image->chn_data.yuv422_data[frame_info.frame_offset],\
                    &chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset]);

                width = chn_image->resize_info.src_width;
                height = chn_image->resize_info.src_height;

                /* 判断是否进行旋转，进行旋转需要先旋转在缩放 */
                if((chn_image->ratio_info.rotate != 0)){
                    /* 进行旋转 */
                    chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.width = chn_image->resize_info.src_width;
                    chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.height = chn_image->resize_info.src_height;
                    chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.wstride = chn_image->resize_info.src_width;
                    chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset].handle_buff.hstride = chn_image->resize_info.src_height;
                    
                    rga_rotate(&chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset], &chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], \
                        chn_image->ratio_info.rotate);
                }

                /*是否进行缩放*/
                if(chn_image->rga_mode == RESIZE_BUFF_MODE){
                    chn_image->chn_data.resize_data[chn_image->rga_rb_offset].Width = chn_image->resize_info.dst_width;
                    chn_image->chn_data.resize_data[chn_image->rga_rb_offset].Height = chn_image->resize_info.dst_height;
                    if((chn_image->ratio_info.rotate != 0)){
                        if((chn_image->ratio_info.width == VIDEO_SPECIAL_FRAME_WIDTH) && (chn_image->ratio_info.height == VIDEO_SPECIAL_FRAME_HEIGHT)){
                            if(chn_image->ratio_info.rotate == 2){
                                memcpy(chn_image->chn_data.resize_data[chn_image->rga_rb_offset].vir_addr, chn_image->chn_data.rotate_data[chn_image->rga_rb_offset].vir_addr + (8*1920*3),
                                    (chn_image->resize_info.dst_width*chn_image->resize_info.dst_height*3));
                            } else {
                                rga_resize(&chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
                            }

                        } else {
                            rga_resize(&chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
                        }

                        //rga_resize(&chn_image->chn_data.rotate_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
                    } else {
                        rga_resize(&chn_image->chn_data.rgb888_data[chn_image->rga_rb_offset], &chn_image->chn_data.resize_data[chn_image->rga_rb_offset]);
                    }
                }

                if(!ringbuffer_is_full(stitchbase_rb)){
                    if(data_stream->stitchbase_stop){
                        stitchbase_frame_info.chn = data_chn;
                        stitchbase_frame_info.frame_offset = chn_image->rga_rb_offset;
                        data_stream_stitchbase_image_to_queue(data_chn, &stitchbase_frame_info);
                        chn_image->rga_rb_offset += 1;
                    } else {
                        SLOGI("[ERROR] stitchbase THREAD ERROR \n");
                    }

                } else {
                    SLOGI("[DEBUG] %s %d ringbuffer_is_full \n", __func__, __LINE__);
                }

                pthread_mutex_unlock(&data_stream->rga_lock);
            }

        }
    }

    return NULL;
}

#endif


unsigned int data_stream_low_chn_thread_init(unsigned char chn)
{
    int ret;
    data_stream_t *data_stream = data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];

    chn_image->chn = chn;
    chn_image->stop_flag = 1;
    // ret = pthread_create(&chn_image->tid, NULL, &data_stream_low_chn_rga_thread, &chn_image->chn);
    // if (ret != 0) {
    //     printf("[ERROR] Create data_stream_high_chn_rga_thread failed %d\n",__func__,ret);
    //     return 77;
    // }

    return 0;
}

void data_stream_low_chn_thread_destory(unsigned char chn)
{
    data_stream_t *data_stream = data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];

    data_stream_chn_ringbuffer_is_empty(chn);

    if(chn_image->stop_flag) {
        chn_image->stop_flag = 0;
        xt_nanosleep(0, 1000*1000);
    }
}

/*
	函数: unsigned int data_stream_stitchbase_thread_init(void)
	作用: 拼接线程初始化函数
	参数:
		无
	返回值:
		正确返回0，失败返回其他
*/
unsigned int data_stream_stitchbase_thread_init(void)
{
    int ret;
    data_stream_t *data_stream = data_stream_get();

    __atomic_store_n(&gs_data_stream->stitchbase_stop, 1, __ATOMIC_RELEASE);
    ret = pthread_create(&gs_data_stream->tid, NULL, &data_stream_image_stitchbase_thread, NULL);
    if (ret != 0) {
        printf("[ERROR] Create data_stream_image_stitchbase_thread failed %d\n",ret);
        return A818_DATA_STITCHBASE_THREAD;
    }

    stitchbase_thread_created = 1;
    return 0;
}

int data_stream_stop_stitchbase(unsigned int timeout_ms)
{
    __atomic_store_n(&gs_data_stream->stitchbase_stop, 0, __ATOMIC_RELEASE);
    if (!stitchbase_thread_created) return 0;
    int ret = stream_join_thread(gs_data_stream->tid, timeout_ms);
    if (!ret) stitchbase_thread_created = 0;
    return ret;
}

/*
	函数: void data_stream_stitchbase_thread_destory(void)
	作用: 拼接线程停止函数
	参数:
		无
	返回值:
		无
*/
void data_stream_stitchbase_thread_destory(void)
{
    data_stream_t *data_stream = data_stream_get();
    if(gs_data_stream->stitchbase_stop) {
        __atomic_store_n(&gs_data_stream->stitchbase_stop, 0, __ATOMIC_RELEASE);
        xt_nanosleep(0, 1000*1000);
    }
}



/**************************************************************/
/*
	函数: void data_stream_destory(void)
	作用: 数据流初始化函数
	参数:
		无
	返回值:
		正确返回0，失败返回其他
*/
int data_stream_init(void)
{
    int ret;
    int chn;

    gs_data_stream = (data_stream_t *)malloc(sizeof(*gs_data_stream));
    if (!gs_data_stream) {
        printf("[ERROR] malloc data stream failed\n");
        return A818_DATA_STREAM_MALLOC;
    }
    memset(gs_data_stream, 0,sizeof(*gs_data_stream));
    memset(output_image_memset, 0x0, sizeof(output_image_memset));

    /* 初始化各个需要拼接通道的 ringbuffer, 这个是不需要跟着停止释放的 */
    for(chn = 0; chn < DATA_CHANNEL_NUM_MAX;chn++){
        gs_data_stream->chn_image[chn] = (fpga_chn_image_info_t*)malloc(sizeof(fpga_chn_image_info_t));
        if(!gs_data_stream->chn_image[chn]){
            printf("[ERROR] chn:%d malloc chn image failed \n", chn);
            ret = A818_DATA_CHN_IMAGE_MALLOC;
            goto err_1; 
        }

        memset(gs_data_stream->chn_image[chn], 0x0, sizeof(fpga_chn_image_info_t));
        gs_data_stream->chn_image[chn]->ratio_info.position = 0xff;

        gs_data_stream->chn_image[chn]->chn_data_rb = ringbuffer_create(RINGBUFFER_FRAME_MAX*sizeof(image_frame_info_t),sizeof(image_frame_info_t));
        if (!gs_data_stream->chn_image[chn]->chn_data_rb) {
            printf("[ERROR] chn:%d init rga data ringbuffer failed\n", chn);
            ret = A818_DATA_RGA_DATA_RB;
            goto err_1;
        }

        gs_data_stream->chn_image[chn]->stitchbase_data_rb = ringbuffer_create(RINGBUFFER_FRAME_MAX*sizeof(image_frame_info_t), sizeof(image_frame_info_t));
        if (!gs_data_stream->chn_image[chn]->stitchbase_data_rb) {
            printf("[ERROR] chn:%d init stitchbase data ringbuffer failed\n", chn);
            ret = A818_DATA_STITCHBASE_DATA_RB;
            goto err_1;
        }

    }

    /*保存拼接后的图像数据*/
    output_image = Mat::zeros(VIDEO_FRAME_HEIGHT, VIDEO_FRAME_WIDTH, CV_8UC3);
    rgb888_output_image_init(0xFF6347);

    /*初始化 no signal 区域*/
    gs_data_stream->no_signal_buffer = (unsigned char*)malloc(VIDEO_FRAME_HEIGHT*VIDEO_FRAME_WIDTH*3);
    if(!gs_data_stream->no_signal_buffer){
        printf("[ERROR] init no signal buf error \n");
        ret = A818_DATA_INIT_NO_SIGNAL;
        goto err_2;
    }

    gs_data_stream->no_signal_index = 0;


    gs_data_stream->rga_addr_offset_max = (200*1024*1024);
    gs_data_stream->rga_addr_offset = 0;
    gs_data_stream->rga_vir_addr = (unsigned char*)malloc(gs_data_stream->rga_addr_offset_max);
    if(!gs_data_stream->rga_vir_addr){
        printf("malloc error \n");
        goto err_3;
    }


    ret = pthread_create(&gs_data_stream->low_chn_tid, NULL, &data_stream_low_chn_rga_thread, NULL);
    if (ret != 0) {
        printf("[ERROR] Create data_stream_high_chn_rga_thread failed %d\n",ret);
        return 77;
    }

    printf("[DEBUG] RGA MALLOC MAX:%d \n", gs_data_stream->rga_addr_offset_max);

    pthread_mutex_init(&gs_data_stream->osd_lock,NULL);
    pthread_mutex_init(&gs_data_stream->swap_lock,NULL);
    pthread_mutex_init(&gs_data_stream->dp_lock,NULL);
    pthread_mutex_init(&gs_data_stream->rga_lock,NULL);

    return A818_SUCCESS;
err_3:
    free(gs_data_stream->no_signal_buffer);
err_2:
err_1:
    for(chn = 0; chn < DATA_CHANNEL_NUM_MAX;chn++){
        if(gs_data_stream->chn_image[chn]){

            if(gs_data_stream->chn_image[chn]->chn_data_rb){
                ringbuffer_destory(gs_data_stream->chn_image[chn]->chn_data_rb);
            }

            if(gs_data_stream->chn_image[chn]->stitchbase_data_rb){
                ringbuffer_destory(gs_data_stream->chn_image[chn]->stitchbase_data_rb);
            }

            free(gs_data_stream->chn_image[chn]);
            gs_data_stream->chn_image[chn] = NULL;
        }
    }

    free(gs_data_stream);
    return ret;
}

/*
	函数: void data_stream_destory(void)
	作用: 数据流释放函数
	参数:
		无
	返回值:
		无
*/
void data_stream_destory(void)
{
    int chn;
    data_stream_t *data_stream = data_stream_get();
    if(data_stream) {
        free(gs_data_stream->rga_vir_addr);
        free(gs_data_stream->no_signal_buffer);
        for(chn = 0; chn < DATA_CHANNEL_NUM_MAX;chn++){
            if(data_stream->chn_image[chn]){

                if(data_stream->chn_image[chn]->chn_data_rb){
                    ringbuffer_destory(data_stream->chn_image[chn]->chn_data_rb);
                }

                if(data_stream->chn_image[chn]->stitchbase_data_rb){
                    ringbuffer_destory(data_stream->chn_image[chn]->stitchbase_data_rb);
                }

                free(data_stream->chn_image[chn]);
                data_stream->chn_image[chn] = NULL;
            }
        }

        free(data_stream);
        data_stream = NULL;
    }

    return ;
}
