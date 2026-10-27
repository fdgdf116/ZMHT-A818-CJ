#include "stream_lifecycle.h"
#include "vfifo_capacity.h"
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

#include "log.h"
#include "xutils.h"
#include "pcie_engine.h"
#include "data_engine.h"
#include "pcie_impl.h"
#include "pcie_reg_rw.h"
#include "mpi_encode.h"
#include "a818_error.h"
#include "gpu_interface.h"

#define PCIE_READ_IMAGE_DATA_ADDRESS (0x40000000)
#define PCIE_WRITE_IMAGE_DATA_ADDRESS (0x80000000)


#define FPGA2RK_VFIFO_SIZE_OFFSET (0x100)
#define RK2FPGA_VFIFO_SIZE_OFFSET (0x104)

#define FPGA_READ_DATA_ROW_MAX (200)
#define FPGA_READ_ONE_DATA_MAX (FPGA_ROW_DATA_SIZE * FPGA_READ_DATA_ROW_MAX)


#define FPGA_PCIE_RESET_OFFSET (0x3c)
#define FPGA_PCIE_ENABLE_OFFSET (0x40)
#define FPGA_PCIE_CHN_OFFSET (0x400)


#define FPGA_SET_CHN_LENGTH_OFFSET (0x0)
#define FPGA_SET_CHN_INTERVAL_OFFSET (0x04)
#define FPGA_SET_CHN_TYPE_OFFSET (0x08)
#define FPGA_SET_CHN_MODE_OFFSET (0xc)
#define FPGA_SET_CHN_ENABLE_OFFSET (0x10)
#define FPGA_SET_CHN_IMAGE_RATIO_OFFSET (0x14)
#define FPGA_SET_CHN_IMAGE_EXTRACTION_OFFSET (0x18)

#define FPGA_SET_CHN_IMAGE_WIDTH_OFFSET (0x1c)
#define FPGA_SET_CHN_IMAGE_HIGHT_OFFSET (0x20)

#define FPGA_SET_CHN_IMAGE_FILL_COLUMN_OFFSET (0x24)
#define FPGA_SET_CHN_IMAGE_FILL_ROW_OFFSET (0x28)
#define FPGA_SET_CHN_IMAGE_RATIO_WIDTH_OFFSET (0x2c)
#define FPGA_SET_CHN_IMAGE_RATIO_HIGHT_OFFSET (0x30)



#define FPGA_GET_IMAGE_WIDTH_OFFSET (0x50)
#define FPGA_GET_IMAGE_HIGHT_OFFSET (0x54)
#define FPGA_GET_IMAGE_FPS_OFFSET (0x58)


#define FPGA_PCIE_REG_PHY (0x10000)
#define FPGA_PCIE_PC_REG_PHY (0x20000)
#define FPGA_PCIE_REG_LEN (0x10000)

#define FPGA_PCIE_DATA_HEAD_SIZE (64)
#define PCIE_HEAD_CHN_OFFSET (17)

#define PCIE_GET_DATA_HEAD(buffer) ({ \
	unsigned char *bd_buffer = buffer;	            \
    unsigned int val;               \
    val = bd_buffer[0] << 24 | bd_buffer[1] << 16 | bd_buffer[2] << 8 | bd_buffer[3]; val;})

#define SET_INTERNAL_IN_RESOLUTION(src_width, src_height, src_fps, set_row_width, set_interval) { 	             \
	.width = src_width,   				                       \
	.height = src_height,					                   \
    .fps = src_fps, \
    .row_width = set_row_width, \
    .interval = set_interval, \
}

typedef struct {
	uint8_t *pcie_data;
    ring_buffer_t *pcie_data_rb;
    void* pcie_reg;
    void* pcie_pc_reg;
} pcie_fpga_info_t;

typedef struct {
	unsigned int width;
    unsigned int height;
    unsigned char fps;
    unsigned int row_width;
    unsigned int interval;
} internal_source_info_t;
//时钟150M=(width+interval)*height*fps

/*
    根据分辨率和帧率配置不同的寄存器值的初始化
*/
static internal_source_info_t internal_source_info[] = {
    SET_INTERNAL_IN_RESOLUTION(1024,256,50,1024,10694),
    SET_INTERNAL_IN_RESOLUTION(1024,768,60,1024,2231),
    SET_INTERNAL_IN_RESOLUTION(1280,720,60,1280,2192),
    SET_INTERNAL_IN_RESOLUTION(1280,960,60,1280,1324),
    SET_INTERNAL_IN_RESOLUTION(1280,1024,60,1280,1161),
    SET_INTERNAL_IN_RESOLUTION(1920,1080,25,1920,3635),
    SET_INTERNAL_IN_RESOLUTION(1920,1080,30,1920,2709),
    SET_INTERNAL_IN_RESOLUTION(1920,1080,60,1920,394),
    SET_INTERNAL_IN_RESOLUTION(1600,1200,60,1600,483),
    SET_INTERNAL_IN_RESOLUTION(4096,4096,1,4096,32578),
};



static pcie_fpga_info_t *g_fpga_pcie;
static void* pcie_reg;

/*
	函数: void pcie_fpga_reg_reset(void)
	作用: pcie各个通道的初始化
	参数:
      无
	返回值:
      无
*/

void pcie_fpga_reg_reset(void)
{
    unsigned int reg_offset;
    /**
        通道总共有24个通道，其中0-11表示缩放小图的通道 12-23表示原图的通道
    */
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, FPGA_PCIE_ENABLE_OFFSET, 0);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, FPGA_PCIE_ENABLE_OFFSET, 0);
    usleep(100*1000);
    for(int chn = 0; chn < 24; chn++) {
        reg_offset = ((chn + 1) * FPGA_PCIE_CHN_OFFSET);
        pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_ENABLE_OFFSET), 0);
        pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_ENABLE_OFFSET), 0);
        usleep(1000);
    }

    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, FPGA_PCIE_RESET_OFFSET, 0);
    usleep(200*1000);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, FPGA_PCIE_RESET_OFFSET, 1);
    usleep(200*1000);

    SLOGI("[%s] pcie reset end \n", __func__);
}

/*
	函数: void pcie_close_chn_enabne(unsigned int chn)
	作用: 关闭分通道使能
	参数:
      chn：需要关闭的通道号
	返回值:
      无
*/
void pcie_close_chn_enabne(unsigned int chn)
{
    unsigned int reg_offset;
    reg_offset = ((chn + 1) * FPGA_PCIE_CHN_OFFSET);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_ENABLE_OFFSET), 0);
    usleep(1000);
}

/*
	函数: internal_source_info_t* get_internal_source_info(fpga_reg_val_info_t* reg_val_info)
	作用: 根据输入的分辨率和帧率获得需要配置的寄存器的值
	参数:
      reg_val_info：输入的通道信息
	返回值:
      internal_source_info_t：需要配置的寄存器值
*/
internal_source_info_t* get_internal_source_info(fpga_reg_val_info_t* reg_val_info)
{
    unsigned char num = (sizeof(internal_source_info) / sizeof(internal_source_info_t));
    for(int i = 0; i < num; i++){
        if((internal_source_info[i].width == reg_val_info->src_width) && (internal_source_info[i].height == reg_val_info->src_height) \
            && (internal_source_info[i].fps == reg_val_info->fps)){
                return &internal_source_info[i];
        }
    }
    return NULL;
}

/*
	函数: int pcie_chn_image_reg_init(fpga_reg_val_info_t* reg_val_info)
	作用: 通道寄存器的配置
	参数:
      reg_val_info：输入的通道信息
	返回值:
      正确返回0，失败返回其他
*/
int pcie_chn_image_reg_init(fpga_reg_val_info_t* reg_val_info)
{
    unsigned int reg_offset;
    unsigned char chn;
    internal_source_info_t* internal_source_info;

    chn = reg_val_info->chn;
    reg_offset = ((chn + 1) * FPGA_PCIE_CHN_OFFSET);

    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_ENABLE_OFFSET), 0);
    usleep(10);

    /*判断是内源还是外源*/
    if(reg_val_info->chn_type == INTERNAL_SOURCE_MODE){/*配置的是内源*/
        
        /*获取内源配置寄存器的值*/
        internal_source_info = get_internal_source_info(reg_val_info);
        if(internal_source_info == NULL){
            SLOGI("[ERROR] get_internal_source_info error \n");
            return 1;
        }

        SLOGI("[Debug] width:%d height:%d fps:%d row_width:%d interval:%d \n", internal_source_info->width, internal_source_info->height, internal_source_info->fps,\
            internal_source_info->row_width, internal_source_info->interval);
        pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_LENGTH_OFFSET), internal_source_info->row_width);
        usleep(10);
        pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_INTERVAL_OFFSET), internal_source_info->interval);
        usleep(10); 
        pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_TYPE_OFFSET), reg_val_info->chn_type);
        usleep(10);
        pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_MODE_OFFSET), 2);
        usleep(10);
    } else if(EXTERNAL_SOURCE_MODE == reg_val_info->chn_type){ /*是外源*/
        pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_TYPE_OFFSET), reg_val_info->chn_type);
        usleep(10);
    }
    
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_RATIO_OFFSET), reg_val_info->ratio);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_EXTRACTION_OFFSET), reg_val_info->extraction);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_WIDTH_OFFSET), reg_val_info->src_width);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_HIGHT_OFFSET), reg_val_info->src_height);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_RATIO_WIDTH_OFFSET), reg_val_info->zoom_width);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_RATIO_HIGHT_OFFSET), reg_val_info->zoom_height);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_FILL_ROW_OFFSET), reg_val_info->fill_row);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_IMAGE_FILL_COLUMN_OFFSET), reg_val_info->fill_column);
    usleep(10);

    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_SET_CHN_ENABLE_OFFSET), 1);
    usleep(10);

    return 0;
}

/*
	函数: void pcie_pc_chn_image_reg_init(fpga_reg_val_info_t* reg_val_info)
	作用: 给pc原图的寄存器配置
	参数:
      reg_val_info：输入的通道信息
	返回值:
      无
*/
void pcie_pc_chn_image_reg_init(fpga_reg_val_info_t* reg_val_info)
{
    unsigned int reg_offset;
    unsigned char chn;

    chn = reg_val_info->chn;
    chn -= PCIE_SRC_START_CHANNEL;
    reg_offset = ((chn + 1) * FPGA_PCIE_CHN_OFFSET);

    SLOGI("[Debug] %s reg offset:0x%x src_width:%d src_height:%d \n", __func__, reg_offset, reg_val_info->src_width, reg_val_info->src_height);

    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_ENABLE_OFFSET), 0);
    usleep(10);

    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_TYPE_OFFSET), reg_val_info->chn_type);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_RATIO_OFFSET), reg_val_info->ratio);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_EXTRACTION_OFFSET), reg_val_info->extraction);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_WIDTH_OFFSET), reg_val_info->src_width);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_HIGHT_OFFSET), reg_val_info->src_height);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_RATIO_WIDTH_OFFSET), reg_val_info->zoom_width);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_RATIO_HIGHT_OFFSET), reg_val_info->zoom_height);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_FILL_ROW_OFFSET), reg_val_info->fill_row);
    usleep(10);
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_IMAGE_FILL_COLUMN_OFFSET), reg_val_info->fill_column);
    usleep(10);
 
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, (reg_offset + FPGA_SET_CHN_ENABLE_OFFSET), 1);
    usleep(10);
}


/*
	函数: unsigned int pcie_get_image_width(unsigned char chn)
	作用: 获取本通道的分辨率的宽
	参数:
      chn:需要获取的通道
	返回值:
      获取到的宽
*/
unsigned int pcie_get_image_width(unsigned char chn)
{
    unsigned int reg_offset;
    unsigned int reg_chn;

    reg_chn = chn - PCIE_SRC_START_CHANNEL;
    reg_offset = ((reg_chn + 1) * FPGA_PCIE_CHN_OFFSET);

    return pcie_bar_read_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_GET_IMAGE_WIDTH_OFFSET));
}

/*
	函数: unsigned int pcie_get_image_height(unsigned char chn)
	作用: 获取本通道的分辨率的高
	参数:
      chn:需要获取的通道
	返回值:
      获取到的高
*/
unsigned int pcie_get_image_height(unsigned char chn)
{
    unsigned int reg_offset;
    unsigned int reg_chn;

    reg_chn = chn - PCIE_SRC_START_CHANNEL;
    reg_offset = ((reg_chn + 1) * FPGA_PCIE_CHN_OFFSET);

    return pcie_bar_read_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_GET_IMAGE_HIGHT_OFFSET));
}

/*
	函数: unsigned int pcie_get_image_fps(unsigned char chn)
	作用: 获取本通道的分辨率的帧率
	参数:
      chn:需要获取的通道
	返回值:
      获取到的帧率
*/
unsigned int pcie_get_image_fps(unsigned char chn)
{
    unsigned int reg_offset;
    unsigned int reg_chn;

    reg_chn = chn - PCIE_SRC_START_CHANNEL;
    reg_offset = ((reg_chn + 1) * FPGA_PCIE_CHN_OFFSET);

    return pcie_bar_read_reg(g_fpga_pcie->pcie_reg, (reg_offset + FPGA_GET_IMAGE_FPS_OFFSET));
}

/*
	函数: void pcie_get_selftest_val(selftest_val_info_t* info)
	作用: 获取fpga给的自检数据
	参数:
      info:自检数据保存指针
	返回值:
      无
*/
void pcie_get_selftest_val(selftest_val_info_t* info)
{
   info->board_type = htonl(pcie_bar_read_reg(g_fpga_pcie->pcie_reg, 0x80));
   info->series_number = htonl(pcie_bar_read_reg(g_fpga_pcie->pcie_reg, 0x84));
   info->version1 = htonl(pcie_bar_read_reg(g_fpga_pcie->pcie_reg, 0x88));
   info->version2 = htonl(pcie_bar_read_reg(g_fpga_pcie->pcie_reg, 0x8c));
   info->link = htonl(pcie_bar_read_reg(g_fpga_pcie->pcie_reg, 0x90));
   info->pc_link = pcie_bar_read_reg(g_fpga_pcie->pcie_reg, 0x94);
   info->rk_link = pcie_bar_read_reg(g_fpga_pcie->pcie_reg, 0x98);

}

/*
	函数: void pcie_set_rk_enable(unsigned char val)
	作用: 3588全通道使能/关使能配置
	参数:
      val:配置的值
	返回值:
      无
*/
void pcie_set_rk_enable(unsigned char val)
{
    pcie_bar_write_reg(g_fpga_pcie->pcie_reg, FPGA_PCIE_ENABLE_OFFSET, val);
}

/*
	函数: void pcie_set_pc_enable(unsigned char val)
	作用: pc原图全通道使能/关使能配置
	参数:
      val:配置的值
	返回值:
      无
*/
void pcie_set_pc_enable(unsigned char val)
{
    pcie_bar_write_reg(g_fpga_pcie->pcie_pc_reg, FPGA_PCIE_ENABLE_OFFSET, val);
}


/*
	函数: pcie_get_fpga2rk_vfifo_szie(void)
	作用: 获取fpga到3588的pcie vfifo数据量
	参数:
      无
	返回值:
      获取到的数据量
*/
unsigned int pcie_get_fpga2rk_vfifo_szie(void)
{
    return pcie_bar_read_reg(g_fpga_pcie->pcie_reg, FPGA2RK_VFIFO_SIZE_OFFSET);
}


static void pcie_data_to_queue(image_frame_info_t *frame_info, ring_buffer_t *rb)
{
	image_frame_info_t frame_param, tmp_param;
	if (ringbuffer_is_full(rb)) {					
		ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));
        SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}

    memcpy(&frame_param,frame_info,sizeof(image_frame_info_t));		
	ringbuffer_put(rb, (uint8_t *)&frame_param, sizeof(frame_param));

    return ;
}

void pcie_fpga2rga_image_to_queue(unsigned char chn, image_frame_info_t *frame_info)
{
    image_frame_info_t event_param, tmp_param;
    data_stream_t *data_stream = data_stream_get();
    ring_buffer_t *rb = data_stream->chn_image[chn]->chn_data_rb;
	
	if (ringbuffer_is_full(rb)) {					
		ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));
        SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}

	memcpy(&event_param,frame_info,sizeof(image_frame_info_t));
    		
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}


/*
	函数: void pcie_fpga_device_stop(void)
	作用: pcie全通道停止函数
	参数:
      无
	返回值:
      无
*/
void pcie_fpga_device_stop(void)
{
    /* Caller has closed admission and confirmed all workers/DMA are idle. */
    pcie_set_rk_enable(0);
    pcie_set_pc_enable(0);
    pcie_fpga_reg_reset();
    ringbuffer_reset(g_fpga_pcie->pcie_data_rb);
}

/*
	函数: static void *pcie_read_data_thread(void* parameter)
	作用: pcie读取数据线程
	参数:
      parameter：线程参数
	返回值:
      线程返回值
*/
static void *pcie_read_data_thread(void* parameter)
{
    int ret;
    struct timeval time;
    unsigned int pcie_buf_offset = 0;
    unsigned int pcie_read_frame = 0;
    unsigned int fpga_vfifo_size = 0;
    unsigned int frame_size = FPGA_READ_ONE_DATA_MAX;
    unsigned int pcie_offset = (((frame_size / DATA_PAGE) + 1) * DATA_PAGE);
    unsigned int pcie_offset_max = (pcie_offset * PCIE_DATA_STREAM_FRAME_MAX);

    data_stream_t *data_stream = data_stream_get();
    ring_buffer_t *rb = g_fpga_pcie->pcie_data_rb;
    uint8_t *pcie_buf = g_fpga_pcie->pcie_data;
    image_frame_info_t frame_info;

    cpu_set_t mask;
    CPU_ZERO(&mask);
    // 限大核调度
    CPU_SET(4, &mask), CPU_SET(5, &mask);
    CPU_SET(6, &mask), CPU_SET(7, &mask);
    pthread_setaffinity_np(pthread_self( ), sizeof(mask), &mask);

    while(1) {
        STREAM_WORK_GUARD(work, STREAM_DMA_READ);
        if (!work.active) { usleep(1000); continue; }
        /*获取vfifo中的数据量*/
        fpga_vfifo_size = pcie_get_fpga2rk_vfifo_szie();
        /*如果数据量大于想要读取的数据，则进行数据的读取*/
        if(fpga_vfifo_size >= frame_size) {
            if(pcie_buf_offset >= pcie_offset_max){
                pcie_buf_offset = 0;
            }
            
            ret = dma_memory_mapped_read(pcie_buf + pcie_buf_offset,PCIE_READ_IMAGE_DATA_ADDRESS, frame_size,PCIE_C2H_0);
            if(ret == frame_size){
                frame_info.frame_offset = pcie_buf_offset;
                if(ringbuffer_is_full(rb)){
                    SLOGI("[%s] ringbuffer full \n", __func__);
                }
                pcie_data_to_queue(&frame_info, rb);
                pcie_buf_offset += pcie_offset;
                pcie_read_frame += 1;

            } else {
                SLOGI("[ERROR] PCIE READ DATA ERROR \n");
                stream_request_stop();
            }
        } else {
            xt_nanosleep(0, 100);
        }

    }
    return NULL;
}

/*
	函数: int pcie_low_chn_data_analysis(unsigned char chn, unsigned char* chn_buf_l)
	作用: pcie小图通道的解析
	参数:
      chn:通道
      chn_buf_l：一行数据(5184)
	返回值:
      正确返回0，失败返回其他
*/
int pcie_low_chn_data_analysis(unsigned char chn, unsigned char* chn_buf_l)
{
    ring_buffer_t *chn_rb;
    uint32_t memcpy_offset; 
    uint32_t memcpy_len;
    uint8_t *chn_image_buf;
    image_frame_info_t chn_frame_info;
    fpga_chn_image_info_t* chn_image;
    data_stream_t *data_stream = data_stream_get();
    
    chn_image = data_stream->chn_image[chn];
    if(chn < 12){
        if(!chn_image->init_flag[DATA_INIT]){
            SLOGI("-1-CHN:%d malloc buff error \n", chn);
            return 1;
        }
    } else {
        if(!chn_image->init_flag[MPP_INIT]){
            SLOGI("-1-CHN:%d malloc buff error \n", chn);
            return 1;
        }
    }

    if(chn_image->chn_data.rb_offset >= (chn_image->data_frame_max + 1)){
        chn_image->chn_data.rb_offset = 0;
    }

    chn_image_buf = chn_image->chn_data.yuv422_data[chn_image->chn_data.rb_offset].vir_addr;
    if(!chn_image_buf){
        SLOGI("-2-CHN:%d malloc buff error \n", chn);
        return 2;
    }

    /* 判断本行是不是图像的结束行，如果是结束行，则对数据进行拷贝 */
    if((chn_image->chn_data.data_offset + (FPGA_ROW_DATA_SIZE - FPGA_PCIE_DATA_HEAD_SIZE)) >= chn_image->chn_data.frame_size){
        
        memcpy_len = chn_image->chn_data.frame_size - chn_image->chn_data.data_offset; /* 剩余的进行拷贝 */
        memcpy(chn_image_buf + chn_image->chn_data.data_offset, (chn_buf_l + FPGA_PCIE_DATA_HEAD_SIZE),memcpy_len);
        
        /*判断是不是拷贝到了一整副图*/
        chn_rb = chn_image->chn_data_rb;
        if(chn_rb){
            if(!ringbuffer_is_full(chn_rb)){
                chn_frame_info.frame_offset = chn_image->chn_data.rb_offset;
                chn_frame_info.frame_size = chn_image->chn_data.frame_size;
                chn_frame_info.t0 = what_time_is_it_now();
                pcie_fpga2rga_image_to_queue(chn, &chn_frame_info);
                chn_image->chn_data.rb_offset += 1;
                if(chn_image->chn_data.rb_offset >= (chn_image->data_frame_max + 1)){
                    chn_image->chn_data.rb_offset = 0;
                }
            }
        } else {
            SLOGI("[Debug] chn:%d ringbuffer is NULL \n", chn);
        }

        chn_image->chn_data.data_offset = 0;
        /* 指向下一个缓冲区，将剩余的进行拷贝 */
        chn_image_buf = chn_image->chn_data.yuv422_data[chn_image->chn_data.rb_offset].vir_addr;

        /* 判断整行判断这一行是不是正好结束 */
        memcpy_offset = (memcpy_len + FPGA_PCIE_DATA_HEAD_SIZE);
        /* 这一行是正从的结束 */
        if(memcpy_offset != FPGA_ROW_DATA_SIZE){
            memcpy(chn_image_buf + chn_image->chn_data.data_offset, (chn_buf_l + memcpy_offset), (FPGA_ROW_DATA_SIZE - memcpy_offset));
            chn_image->chn_data.data_offset += (FPGA_ROW_DATA_SIZE - memcpy_offset); /* 更新拷贝的数据的大小 */
        }

        /* 本行处理完，进行下一行的传输 */
        return 0;
    }

    memcpy(chn_image_buf + chn_image->chn_data.data_offset, (chn_buf_l + FPGA_PCIE_DATA_HEAD_SIZE), (FPGA_ROW_DATA_SIZE - FPGA_PCIE_DATA_HEAD_SIZE));
    chn_image->chn_data.data_offset += (FPGA_ROW_DATA_SIZE - FPGA_PCIE_DATA_HEAD_SIZE); /* 更新拷贝的数据的大小 */
    
    return 0;
}

/*
	函数: int pcie_high_chn_data_analysis(unsigned char chn, unsigned char* chn_buf_l)
	作用: 高通道数据解析(基本上与低通道的解析是一致的，只是两者保存的一帧图的buffer是不一样的)
	参数:
      chn:通道
      chn_buf_l：一行数据(5184)
	返回值:
      正确返回0，失败返回其他
*/
int pcie_high_chn_data_analysis(unsigned char chn, unsigned char* chn_buf_l)
{
    ring_buffer_t *chn_rb;
    uint32_t memcpy_offset; 
    uint32_t memcpy_len;
    uint8_t *chn_image_buf;
    image_frame_info_t chn_frame_info;
    gpu_image_info_t *gpu_image;
    fpga_chn_image_info_t* chn_image;
    GPU_Frame_st* gpu_frame_stream;
    data_stream_t *data_stream = data_stream_get();
    
    gpu_frame_stream = gpu_data_stream_get();
    if((!gpu_frame_stream)){
        printf("[Debug] %s chn:%d get gpu_frame_stream error \n", __func__, chn);
        return 1;
    }

    gpu_image = &gpu_frame_stream->gpu_image[chn];
    if((!gpu_image)){
        printf("[Debug] %s chn:%d get gpu_image error \n", __func__, chn);
        return 2;
    }

    chn_image_buf = (uint8_t*)gpu_image->in_yuv_buffer[gpu_image->rb_offset];
    if((!chn_image_buf)){
        printf("[Debug] %s chn:%d get chn_image_buf error \n", __func__, chn);
        return 3;
    }

    chn_image = data_stream->chn_image[chn];
    if(!chn_image){
        SLOGI("-1-CHN:%d malloc buff error \n", chn);
        return 4;
    }

    /* 判断本行是不是图像的结束行，如果是结束行，则对数据进行拷贝 */
    if((gpu_image->data_offset + (FPGA_ROW_DATA_SIZE - FPGA_PCIE_DATA_HEAD_SIZE)) >= gpu_image->frame_size){

        memcpy_len = gpu_image->frame_size - gpu_image->data_offset; /* 剩余的进行拷贝 */
        memcpy(chn_image_buf + gpu_image->data_offset, (chn_buf_l + FPGA_PCIE_DATA_HEAD_SIZE),memcpy_len);

        chn_rb = chn_image->chn_data_rb;
        if(chn_rb){
            if(!ringbuffer_is_full(chn_rb)){
                chn_frame_info.frame_offset = gpu_image->rb_offset;
                //chn_frame_info.frame_index = gpu_image->rb_offset;
                chn_frame_info.frame_size = gpu_image->frame_size;
                chn_frame_info.t0 = what_time_is_it_now();
                pcie_fpga2rga_image_to_queue(chn, &chn_frame_info);
                gpu_image->rb_offset += 1;
                if(gpu_image->rb_offset >= (chn_image->data_frame_max + 1)){
                    gpu_image->rb_offset = 0;
                }
            }
        } else {
            SLOGI("[Debug] chn:%d ringbuffer is NULL \n", chn);
        }

        gpu_image->data_offset = 0;

        chn_image_buf = (uint8_t*)gpu_image->in_yuv_buffer[gpu_image->rb_offset];
        if((!chn_image_buf)){
            printf("[Debug] %s chn:%d get chn_image_buf error \n", __func__, chn);
            return 4;
        }

        /* 判断整行判断这一行是不是正好结束 */
        memcpy_offset = (memcpy_len + FPGA_PCIE_DATA_HEAD_SIZE);

        if(memcpy_offset != FPGA_ROW_DATA_SIZE){
            memcpy(chn_image_buf + gpu_image->data_offset, (chn_buf_l + memcpy_offset), (FPGA_ROW_DATA_SIZE - memcpy_offset));
            gpu_image->data_offset += (FPGA_ROW_DATA_SIZE - memcpy_offset); /* 更新拷贝的数据的大小 */
        }

    } else {
        memcpy(chn_image_buf + gpu_image->data_offset, (chn_buf_l + FPGA_PCIE_DATA_HEAD_SIZE), (FPGA_ROW_DATA_SIZE - FPGA_PCIE_DATA_HEAD_SIZE));
        gpu_image->data_offset += (FPGA_ROW_DATA_SIZE - FPGA_PCIE_DATA_HEAD_SIZE); /* 更新拷贝的数据的大小 */
    }

    return 0;
}

/*
	函数: static void *pcie_data_analysis_thread(void* parameter)
	作用: 读取到的数据解析线程
	参数:
        线程参数
	返回值:
        线程返回值
*/
/*
    说明:
        fpga传递的数据是有协议的，协议为64 + 5120 = 5184，每个5184会打一次帧头，
        因此按照5184的整数倍来读取数据，这样方便确认是不是丢数据。
*/
static void *pcie_data_analysis_thread(void* parameter)
{
    int ret;
    struct timeval time;
    uint8_t *pcie_buf;
    unsigned int chn, frame_index;
    image_frame_info_t frame_info;
    uint8_t *chn_buf_l;
    ring_buffer_t *rb = g_fpga_pcie->pcie_data_rb;
    unsigned int pcie_data_head = 0x499602d2;

    cpu_set_t mask;
    CPU_ZERO(&mask);
    // 限大核调度
    CPU_SET(4, &mask), CPU_SET(5, &mask);
    CPU_SET(6, &mask), CPU_SET(7, &mask);
    pthread_setaffinity_np(pthread_self( ), sizeof(mask), &mask);

    while(1){
        STREAM_WORK_GUARD(work, STREAM_WORK);
        if (!work.active) { usleep(1000); continue; }
        if (ringbuffer_is_empty(rb)) { usleep(1); continue; }
        ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));

        if(ringbuffer_is_full(rb)){
            SLOGI("[%s] ringbuffer full \n", __func__);
        }

        pcie_buf = g_fpga_pcie->pcie_data + frame_info.frame_offset;

        /*对每行数据进行解析*/
        for(frame_index = 0; frame_index < FPGA_READ_DATA_ROW_MAX; frame_index++){
            chn_buf_l = (pcie_buf + ((frame_index) * FPGA_ROW_DATA_SIZE));

            if(PCIE_GET_DATA_HEAD(chn_buf_l) != pcie_data_head){ /*帧头错误，则跳过本帧解析*/
                SLOGI("pcie index (0x%x != 0x%x )error \n", PCIE_GET_DATA_HEAD(chn_buf_l), pcie_data_head);
                continue;
            }

            chn = chn_buf_l[PCIE_HEAD_CHN_OFFSET]; /* DATA_CHANNEL_OFFSET:通道偏移 */
            if(chn >= GPU_STITCHBASE_CHN){/* 帧序号超过最大的帧序号，则跳出本帧解析 */
                SLOGI("get pcie data chn(%d - %d) error \n", chn, chn_buf_l[16]);
                continue;
            }

            if(chn >= PCIE_SRC_START_CHANNEL && chn <= PCIE_SRC_END_CHANNEL){
                pcie_high_chn_data_analysis(chn, chn_buf_l);
            } else {
                pcie_low_chn_data_analysis(chn, chn_buf_l);
            }
        }
    }
    return NULL;
}


/* 0x104 counts occupied 32-byte units; 0x100 is retained as a raw value.
 * Cache before every write, but print only on faults. */
static pthread_mutex_t vfifo_diag_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t sequence, started_ms;
    unsigned int before_100, before_104, bytes;
    int active;
} vfifo_diag;

static uint64_t vfifo_diag_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void pcie_log_vfifo_diagnostics(const char *reason)
{
    if (!g_fpga_pcie || !g_fpga_pcie->pcie_reg) return;
    pthread_mutex_lock(&vfifo_diag_lock);
    uint64_t now = vfifo_diag_ms();
    unsigned int current_100 = pcie_bar_read_reg(g_fpga_pcie->pcie_reg, FPGA2RK_VFIFO_SIZE_OFFSET);
    unsigned int current_104 = pcie_bar_read_reg(g_fpga_pcie->pcie_reg, RK2FPGA_VFIFO_SIZE_OFFSET);
    SLOGI("[VFIFO] reason=%s mono_ms=%" PRIu64 " seq=%" PRIu64
          " write_active=%d write_age_ms=%" PRIu64 " bytes=%u target=0x80000000"
          " before_100=0x%08x before_104=0x%08x now_100=0x%08x now_104=0x%08x used_104_bytes=%" PRIu64 " free_104_bytes=%" PRIu64 "\n",
          reason, now, vfifo_diag.sequence, vfifo_diag.active,
          vfifo_diag.active ? now - vfifo_diag.started_ms : 0,
          vfifo_diag.bytes, vfifo_diag.before_100, vfifo_diag.before_104,
          current_100, current_104, (uint64_t)current_104 * 32,
          rk2fpga_vfifo_free_bytes(current_104));
    pthread_mutex_unlock(&vfifo_diag_lock);
}

int pcie_mpp_data_send(unsigned char* mpp_data, unsigned int size)
{
    int ret = 0;
    pthread_mutex_lock(&vfifo_diag_lock);
    ++vfifo_diag.sequence;
    vfifo_diag.started_ms = vfifo_diag_ms();
    vfifo_diag.bytes = size;
    vfifo_diag.before_100 = pcie_bar_read_reg(g_fpga_pcie->pcie_reg, FPGA2RK_VFIFO_SIZE_OFFSET);
    vfifo_diag.before_104 = pcie_bar_read_reg(g_fpga_pcie->pcie_reg, RK2FPGA_VFIFO_SIZE_OFFSET);
    vfifo_diag.active = 1;
    pthread_mutex_unlock(&vfifo_diag_lock);
    ret = dma_memory_mapped_write(mpp_data, PCIE_WRITE_IMAGE_DATA_ADDRESS,size, PCIE_H2C_0);
    if(ret != size){
        pcie_log_vfifo_diagnostics("write_failed");
        SLOGI("rk2fpga size:(%d-%d) data error \n", ret, size);
    }
    pthread_mutex_lock(&vfifo_diag_lock);
    vfifo_diag.active = 0;
    pthread_mutex_unlock(&vfifo_diag_lock);

    return ret;
}


/*帧头填充*/
void mpp_data_assemble_frame(uint8_t* mpp_data, unsigned char src_chn, unsigned char chn, unsigned int frame_index)
{
    unsigned int framer_head = 0xfa3ce6d5;
    mpi_encode_info_t* mpi_encode = mpi_encode_info_get();

    mpp_data[0] = (framer_head >> 24) & 0xff;
    mpp_data[1] = (framer_head >> 16) & 0xff;
    mpp_data[2] = (framer_head >> 8) & 0xff;
    mpp_data[3] = framer_head & 0xff;

    mpp_data[4] = (MPP_OUTPUT_FRAME_SIZE >> 24) & 0xff;
    mpp_data[5] = (MPP_OUTPUT_FRAME_SIZE >> 16) & 0xff;
    mpp_data[6] = (MPP_OUTPUT_FRAME_SIZE >> 8) & 0xff;
    mpp_data[7] = MPP_OUTPUT_FRAME_SIZE & 0xff;

    mpp_data[8] = (frame_index >> 24) & 0xff;
    mpp_data[9] = (frame_index >> 16) & 0xff;
    mpp_data[10] = (frame_index >> 8) & 0xff;
    mpp_data[11] = frame_index & 0xff;

    mpp_data[16] = src_chn;
    mpp_data[17] = chn;
    
}
/*有效数据填充*/
void mpp_data_frame_effective_length(uint8_t* mpp_data, unsigned int length)
{
    mpp_data[60] = (length >> 24) & 0xff;
    mpp_data[61] = (length >> 16) & 0xff;
    mpp_data[62] = (length >> 8) & 0xff;
    mpp_data[63] = length & 0xff;
}

int pcie_send_mpp_data(int chn, image_frame_info_t *frame_info)
{
    int ret;
    mpi_image_encode_info_t *image_encode;
    mpi_encode_info_t* mpi_encode = mpi_encode_info_get();
    uint8_t *buffer = align_write_mpp_buf_get(chn);
    image_encode = &mpi_encode->image_encode[chn];

    memset(buffer, 0x0, (MPP_OUTPUT_FRAME_SIZE + MPP_OUTPUT_HEAD_SIZE));
    
    mpp_data_assemble_frame(buffer, frame_info->chn, chn, frame_info->frame_index);
    memcpy(buffer + MPP_OUTPUT_HEAD_SIZE, image_encode->encode_data + frame_info->frame_offset, frame_info->frame_size);
    mpp_data_frame_effective_length(buffer, frame_info->frame_size);

    ret = pcie_mpp_data_send(buffer, (MPP_OUTPUT_FRAME_SIZE + MPP_OUTPUT_HEAD_SIZE));
    if(ret != (MPP_OUTPUT_FRAME_SIZE + MPP_OUTPUT_HEAD_SIZE)){
        return 1;
    }

    SLOGI("rk2fpga chn:%d f_chn:%d index:%d offset:0x%x size:%d data ok \n", chn, frame_info->chn, frame_info->frame_index,frame_info->frame_offset, frame_info->frame_size);

    return 0;
}

/*
	函数: static void *pcie_mpp_send_thread(void* parameter)
	作用: 编码数据发送线程
	参数:
        线程参数
	返回值:
        线程返回值
*/
static void *pcie_mpp_send_thread(void* parameter)
{
    int ret;
    int capacity_paused = 0;
    uint64_t capacity_log_ms = 0;
    ring_buffer_t *rb;
    image_frame_info_t frame_info;

    mpi_image_encode_info_t *image_encode;
    mpi_encode_info_t* mpi_encode = mpi_encode_info_get();

    while(1){
        int wait_for_space = 0;
        /*遍历编码通道，有数据则通过PCIE发送出去*/
        for(int chn = 0; chn < MPP_ENCODE_NTHREAD_MAX; chn++){
            STREAM_WORK_GUARD(work, STREAM_DMA_WRITE);
            if (!work.active) { usleep(1000); break; }
            rb =mpi_encode->mpp_encode_rb[chn];
            image_encode = &mpi_encode->image_encode[chn];
            if(!ringbuffer_is_empty(rb)) {
                /* One H2C sender owns this path. Check before dequeue so a
                 * capacity pause neither drops this entry nor faults the stream. */
                uint32_t count = pcie_bar_read_reg(g_fpga_pcie->pcie_reg, RK2FPGA_VFIFO_SIZE_OFFSET);
                int can_write = rk2fpga_vfifo_can_write(count,
                    MPP_OUTPUT_FRAME_SIZE + MPP_OUTPUT_HEAD_SIZE);
                uint64_t now = vfifo_diag_ms();
                if ((!can_write || capacity_paused) &&
                    (!capacity_log_ms || now - capacity_log_ms >= 1000)) {
                    SLOGI("[VFIFO] %s DMA write: count_104=%u used_bytes=%" PRIu64
                          " free_bytes=%" PRIu64 " reserve_bytes=%" PRIu64 "\n",
                          can_write ? "resume" : "pause", count, (uint64_t)count * 32,
                          rk2fpga_vfifo_free_bytes(count), RK2FPGA_VFIFO_RESERVE_BYTES);
                    capacity_log_ms = now;
                }
                capacity_paused = !can_write;
                if (!can_write) {
                    wait_for_space = 1;
                    break;
                }
                if(ringbuffer_is_full(rb)){
                    SLOGI("[%s] chn:%d ringbuffer full \n", __func__, chn);
                }
                ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));
                
                ret = pcie_send_mpp_data(chn, &frame_info);
                if(ret != 0){
                    SLOGI("rk2fpga chn:%d offset:0x%x size:(%d) data error; stopping stream \n", chn, frame_info.frame_offset, frame_info.frame_size);
                    stream_request_stop();
                    break;
                }
            }
        }
        /* Outside the work guard: TYPE7 does not wait on the capacity sleep. */
        if (wait_for_space) usleep(1000);
        else xt_nanosleep(0, 100);
    }

    return NULL;
}




/*
	函数: int fpga_pcie_device_open(void)
	作用: pcie设备初始化
	参数:
        无
	返回值:
        正确返回0，失败返回其他
*/
int fpga_pcie_device_open(void)
{
    int ret;

    ret = pcie_bar_open();
    if(ret != 0){
        printf("[ERROR] pcie bar mmap error \n");
        return A818_PCIE_BAR_OPEN;
    }

    ret = pcie_impl_init();
    if(ret != 0){
        printf("[ERROR] pcie impl init error \n");
        ret = A818_PCIE_IMPL_INIT;
        goto err_1;
    }

    pcie_reg = pcie_bar_mmap(FPGA_PCIE_REG_PHY, FPGA_PCIE_REG_LEN);
    if(pcie_reg == NULL){
        printf("[ERROR] pcie reg bar mmap error \n");
        ret = A818_PCIE_REG_MMAP;
        goto err_2;
    }

    set_pcie_timestamp_addr(pcie_reg);

    return A818_SUCCESS;

err_2:
    pcie_impl_destory();
err_1:
    pcie_bar_close();
    return ret;
}

/*
	函数: void fpga_pcie_device_close(void)
	作用: pcie设备初销毁
	参数:
        无
	返回值:
        无
*/
void fpga_pcie_device_close(void)
{
    pcie_bar_munmap(pcie_reg, FPGA_PCIE_REG_LEN);
    pcie_impl_destory();
    pcie_bar_close();
}

/*
	函数: int pcie_fpga_data_init(void)
	作用: pcie资源初始化
	参数:
        无
	返回值:
        正确返回0，失败返回其他
*/
int pcie_fpga_data_init(void)
{
	int ret;
	pthread_t read_tid, analysis_tid, send_tid;

	g_fpga_pcie = (pcie_fpga_info_t*)malloc(sizeof(pcie_fpga_info_t));
	if(NULL == g_fpga_pcie){
		printf("[ERROR] fpga pcie malloc error \n");
		return A818_PCIE_DATA_MALLOC;
	}


    g_fpga_pcie->pcie_data = align_read_buf_get();
    g_fpga_pcie->pcie_data_rb = ringbuffer_create(PCIE_DATA_STREAM_FRAME_MAX*sizeof(image_frame_info_t), sizeof(image_frame_info_t));
    if (!g_fpga_pcie->pcie_data_rb) {
        printf("[ERROR] create pcie data rb buffer failed\n");
        ret = A818_PCIE_DATA_RB;
        goto err_1;
    }

    g_fpga_pcie->pcie_reg = pcie_reg;

    g_fpga_pcie->pcie_pc_reg = pcie_bar_mmap(FPGA_PCIE_PC_REG_PHY, FPGA_PCIE_REG_LEN);
    if(g_fpga_pcie->pcie_pc_reg == NULL){
        printf("[ERROR] pcie reg bar mmap error \n");
        ret = A818_PCIE_PC_REG_MMAP;
        goto err_2;
    }

    /* 初始化相关线程 */
    ret = pthread_create(&read_tid,NULL,&pcie_read_data_thread,NULL);;
    if (ret != 0) {
        printf("[ERROR] Create pcie read data thread failed %d\n",ret);
        ret = A818_PCIE_READ_THREAD;
        goto err_3;
    }

    ret = pthread_create(&analysis_tid,NULL,&pcie_data_analysis_thread,NULL);;
    if (ret != 0) {
        printf("[ERROR] Create pcie data analysis thread failed %d\n",ret);
        ret = A818_PCIE_ANALYSIS_THREAD;
        goto err_4;
    }

    ret = pthread_create(&send_tid,NULL,&pcie_mpp_send_thread,NULL);;
    if (ret != 0) {
        printf("[ERROR] Create pcie data send thread failed %d\n",ret);
        ret = A818_PCIE_SEND_THREAD;
        goto err_5;
    }

    pcie_fpga_reg_reset();
    SLOGI("[%s] pcie init ok \n", __func__);
    usleep(200*1000);

    return A818_SUCCESS;

err_5:
    pthread_cancel(analysis_tid);
err_4:
    pthread_cancel(read_tid);
err_3:
    pcie_bar_munmap(g_fpga_pcie->pcie_pc_reg, FPGA_PCIE_REG_LEN);  
err_2:
    ringbuffer_destory(g_fpga_pcie->pcie_data_rb);
err_1:
    free(g_fpga_pcie);
    return ret;
}

/*
	函数: void pcie_fpga_data_destory(void)
	作用: pcie资源释放
	参数:
        无
	返回值:
        无
*/
void pcie_fpga_data_destory(void)
{
    if(g_fpga_pcie){
        pcie_set_rk_enable(0);
        pcie_set_pc_enable(0);
        ringbuffer_destory(g_fpga_pcie->pcie_data_rb);
        free(g_fpga_pcie);
        g_fpga_pcie = NULL;
    }
}
