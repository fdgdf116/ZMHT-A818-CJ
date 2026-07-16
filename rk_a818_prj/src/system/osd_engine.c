#include "osd_engine.h"
#include "log/log.h"
#include "GUI.h"
#include "GUI_Init.h"
#include "lcd_init.h"
#include "data_engine.h"
#include "dp_engine.h"
#include "gpu_interface.h"
#include "pcie_reg_rw.h"
#include "pcie_engine.h"
#include "xutils.h"
#include "a818_error.h"

#define OSD_GUI_FONT_FORMAT_MAX (3)
#define OSD_GUI_FONT_SIZE_MAX (9)

/*
  GUI的画布分布：
    一帧图的最大分辨率是4096x4096，最多可以8路编码输出，因此申请了一个4096x4096x8的大画布。
    其中：
       第一路编码的画布为，第一个4096x4096
       第二路编码的画布为，第二个4096x4096
       ...
       ...
       拼接的画布为：第八个4096x4096
*/

struct osd_chn_info_t{
  unsigned char chn;
  pthread_t tid;
  pthread_t osd_tid;
  unsigned char osd_flag;
  unsigned char gui_flag;
  ring_buffer_t *osd_rb;
};

struct osd_data_stream_t {
  pthread_t gui_tid;
  pthread_t dp_send_tid;
  ring_buffer_t *osd_dp_rb;
  pthread_mutex_t lock;
  struct osd_chn_info_t chn_info[DATA_CHANNEL_NUM_MAX];
};

struct osd_gui_font_info {
  unsigned char fony_size;
  const GUI_FONT GUI_UNI_PTR * pNewFont;
};

#define SET_OSD_GUI_FONT(size, Font) { 	             \
	.fony_size = size,   				                       \
	.pNewFont = &Font,					                      \
}

/* gui字体的初始化 */
struct osd_gui_font_info gui_font[OSD_GUI_FONT_FORMAT_MAX][OSD_GUI_FONT_SIZE_MAX] = {
  {
    SET_OSD_GUI_FONT(0, GUI_FontHZ8x8_songti),
    SET_OSD_GUI_FONT(1, GUI_FontHZ16x16_songti),
    SET_OSD_GUI_FONT(2, GUI_FontHZ24x24_songti),
    SET_OSD_GUI_FONT(3, GUI_FontHZ32x32_songti),
    SET_OSD_GUI_FONT(4, GUI_FontHZ40x40_songti),
    SET_OSD_GUI_FONT(5, GUI_FontHZ48x48_songti),
    SET_OSD_GUI_FONT(6, GUI_FontHZ56x56_songti),
    SET_OSD_GUI_FONT(7, GUI_FontHZ64x64_songti),
    SET_OSD_GUI_FONT(8, GUI_FontHZ96x96_songti),
  },

  {
    SET_OSD_GUI_FONT(0, GUI_FontHZ8x8_heiti),
    SET_OSD_GUI_FONT(1, GUI_FontHZ16x16_heiti),
    SET_OSD_GUI_FONT(2, GUI_FontHZ24x24_heiti),
    SET_OSD_GUI_FONT(3, GUI_FontHZ32x32_heiti),
    SET_OSD_GUI_FONT(4, GUI_FontHZ40x40_heiti),
    SET_OSD_GUI_FONT(4, GUI_FontHZ48x48_heiti),
    SET_OSD_GUI_FONT(4, GUI_FontHZ56x56_heiti),
    SET_OSD_GUI_FONT(4, GUI_FontHZ64x64_heiti),
    SET_OSD_GUI_FONT(4, GUI_FontHZ96x96_heiti),
  },

  {
    SET_OSD_GUI_FONT(0, GUI_FontHZ8x8_fangsong),
    SET_OSD_GUI_FONT(1, GUI_FontHZ16x16_fangsong),
    SET_OSD_GUI_FONT(2, GUI_FontHZ24x24_fangsong),
    SET_OSD_GUI_FONT(3, GUI_FontHZ32x32_fangsong),
    SET_OSD_GUI_FONT(4, GUI_FontHZ40x40_fangsong),
    SET_OSD_GUI_FONT(4, GUI_FontHZ48x48_fangsong),
    SET_OSD_GUI_FONT(4, GUI_FontHZ56x56_fangsong),
    SET_OSD_GUI_FONT(4, GUI_FontHZ64x64_fangsong),
    SET_OSD_GUI_FONT(4, GUI_FontHZ96x96_fangsong),
  },

};

static struct osd_data_stream_t *gs_osd_stream = NULL;
static unsigned char* gs_osd_data=NULL;

static void osd_dp_image_to_queue(image_frame_info_t *frame_info)
{
  image_frame_info_t event_param, tmp_param;
  ring_buffer_t *rb = gs_osd_stream->osd_dp_rb;
	
	if (ringbuffer_is_full(rb)) {
    ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));				
    SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}
	memcpy(&event_param,frame_info,sizeof(image_frame_info_t));
    		
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}


static void osd_image_to_queue(unsigned char chn, image_frame_info_t *frame_info)
{
  image_frame_info_t event_param, tmp_param;
  ring_buffer_t *rb = gs_osd_stream->chn_info[chn].osd_rb;
	
	if (ringbuffer_is_full(rb)) {
    ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));	
    SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}
  
	memcpy(&event_param,frame_info,sizeof(image_frame_info_t));
    		
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}

/*
	函数: void gui_osd_src_image(unsigned char chn)
	作用: 编码通道数据进行时帧标的叠加
	参数:
      chn:编码通道
	返回值:
      无
*/
void gui_osd_src_image(unsigned char chn)
{
    char buf[128];
    time_t timep;
    struct tm tm_p;
    int position_x;
    int position_y;
    unsigned int offset;
    unsigned int y_start;
    unsigned int chn_width;
    unsigned int chn_height;
    unsigned int chn_fps;

    data_stream_t *data_stream = data_stream_get();
    fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];

    /*获取通道的宽、高、以及帧率*/
    chn_width = pcie_get_image_width(chn);
    chn_height = pcie_get_image_height(chn);
    chn_fps = pcie_get_image_fps(chn);

    /*根据不同的分辨率设置时帧标显示的大小*/
    if(chn_height <= 256){
      GUI_SetFont(&GUI_FontHZ16x16_songti);
      offset = 16;
    } else if(chn_height <= 1024){
      GUI_SetFont(&GUI_FontHZ24x24_songti);
      offset = 24;
    } else if(chn_height <= 2048){
      GUI_SetFont(&GUI_FontHZ32x32_songti);
      offset = 32;
    } else if(chn_height <= 3184){
      GUI_SetFont(&GUI_FontHZ40x40_songti);
      offset = 40;
    } else {
      GUI_SetFont(&GUI_FontHZ64x64_songti);
      offset = 64;
    }

    GUI_SetColor(GUI_GREEN);

    /*获取到对应通道的画布的位置*/
    y_start = chn_image->mpp_info.mpp_chn * 4096;

    /*根据叠加的位置获取到叠加的起始点坐标*/
    switch(chn_image->mpp_info.position){
      case 0:
        position_x = 0;
        position_y = y_start;
        break;
      case 1:
        position_x = chn_image->mpp_info.width - (offset*13);
        position_y = y_start;
        break;
      case 2:
        position_x = 0;
        position_y = y_start + chn_image->mpp_info.height - (offset*4);
        break;
      case 3:
        position_x = chn_image->mpp_info.width - (offset*13);
        position_y = y_start + chn_image->mpp_info.height - (offset*4);
        break;
      default:
        position_x = 0;
        position_y = y_start;
        break;

    }

    /*进行叠加时针标*/
    GUI_SetPenSize(2);
    memset(buf, 0x0, sizeof(buf));
    sprintf(buf,"chn%d_%dx%d@%d", (chn_image->mpp_info.chn - 11), \
      chn_width, chn_height, chn_fps);
    GUI_DispStringAt(buf, position_x, position_y);

    memset(buf, 0x0, sizeof(buf));
    sprintf(buf, "frame:%d", chn_image->frame_num);
    GUI_DispStringAt(buf, position_x, position_y+offset);
    chn_image->frame_num +=1;

    memset(buf, 0x0, sizeof(buf));
	  time(&timep);
	  localtime_r(&timep, &tm_p);
    sprintf(buf, "time:%d-%d-%d %d:%d:%d", (1900 + tm_p.tm_year), ( 1 + tm_p.tm_mon), tm_p.tm_mday, \
		  tm_p.tm_hour, tm_p.tm_min, tm_p.tm_sec);
    GUI_DispStringAt(buf, position_x, position_y + (offset*2));
    GUI_SetPenSize(1);
}

/*
	函数: void draw_wujiaoxing(int x, int y, int r)
	作用: 画五角星
	参数:
      x:起始坐标x
      y:起始坐标y
      r:大小
	返回值:
      无
*/
void draw_wujiaoxing(int x, int y, int r)
{
   GUI_POINT aPoints[10];
   float pi = 3.1415926;

    //左上点
    aPoints[0].x = x - (short)(r * sin(2*pi/5));
    aPoints[0].y = y - (short)(r * cos(2*pi/5));

    aPoints[1].x = x - (short)(r * cos(2*pi/5) * tan(pi/5));
    aPoints[1].y = y - (short)(r * cos(2*pi/5));

    //顶点
    aPoints[2].x  = x;
    aPoints[2].y = y-r;

    aPoints[3].x = x + (short)(r * cos(2*pi/5) * tan(pi/5));
    aPoints[3].y = y - (short)(r * cos(2*pi/5));

    //右上点
    aPoints[4].x  = x + (short)(r * sin(2 * pi/5));
    aPoints[4].y =  y - (short)(r * cos(2 * pi/5));

    aPoints[5].x = x + (short)(r * cos(2*pi/5) * acos(pi/5) * sin(2 * pi/5));
    aPoints[5].y = y + (short)(r * cos(2*pi/5) * acos(pi/5) * cos(2 * pi/5));

    //右下点
    aPoints[6].x  = x + (short)(r * sin(pi/5));
    aPoints[6].y = y + (short)(r * cos(pi/5));

    aPoints[7].x  = x ;
    aPoints[7].y = y + (short)(r * cos(2*pi/5)* acos(pi/5) );

  //左下点
    aPoints[8].x  = x - (short)(r * sin(pi/5));
    aPoints[8].y = y + (short)(r * cos(pi/5));
  
    aPoints[9].x = x - (short)(r * cos(2*pi/5)* acos(pi/5) * sin(2 * pi/5));
    aPoints[9].y = y + (short)(r * cos(2*pi/5) * acos(pi/5) * cos(2 * pi/5));

  GUI_FillPolygon(aPoints, 10, 0, 0);
  
}
/*
	函数: void draw_sanjiao(int x, int y, int r)
	作用: 画三角
	参数:
      x:起始坐标x
      y:起始坐标y
      r:大小
	返回值:
      无
*/
void draw_sanjiao(int x, int y, int r)
{
    GUI_POINT aPoints[3];
    aPoints[0].x = x;
    aPoints[0].y = y-r;

    aPoints[1].x = x-r;
    aPoints[1].y = y+r;

    aPoints[2].x = x+r;
    aPoints[2].y = y+r;

    GUI_FillPolygon(aPoints, 3, 0, 0);
  
}

/*
	函数: gui_osd_draw_character(unsigned int x, unsigned int y, unsigned int r, unsigned char type)
	作用: 画三角或者五角星
	参数:
      x:起始坐标x
      y:起始坐标y
      r:大小
      type：作画的类型
	返回值:
      无
*/
void gui_osd_draw_character(unsigned int x, unsigned int y, unsigned int r, unsigned char type)
{
    switch(type)
    {
      case 0:
        draw_wujiaoxing(x, y, r);
        break;
      case 1:
        draw_sanjiao(x,y,r);
        break;
    }
}

/*
	函数: void gui_osd_stitchbase_chn_image(void)
	作用: 拼接图像显示小图通道和拼接图的时针标
	参数:
      无
	返回值:
      无
*/
void gui_osd_stitchbase_chn_image(void)
{
    int position_x;
    int position_y;
    unsigned int offset;
    unsigned int y_start;
    time_t timep;
    struct tm tm_p;
    unsigned char buf[64];
    data_stream_t *data_stream = data_stream_get();

    y_start = 7 * 4096;
    position_x = 520;
    position_y = 0;

    GUI_SetColor(GUI_GREEN);
    GUI_SetFont(&GUI_FontHZ32x32_songti);
    offset = 32;

    /*遍历拼接通道*/
    for(int stitchbase_chn = 0; stitchbase_chn < DATA_RATIO_CHANNEL_MAX; stitchbase_chn++){
      /*是否是拼图小图*/
      if(data_stream->stitchbase_chn_info[stitchbase_chn].flag){
        memset(buf, 0x0, sizeof(buf));
        sprintf(buf,"chn%d", (stitchbase_chn + 1));
        GUI_DispStringAt(buf, data_stream->stitchbase_chn_info[stitchbase_chn].x, y_start + data_stream->stitchbase_chn_info[stitchbase_chn].y);
      }
    }

    GUI_SetPenSize(2);
    memset(buf, 0x0, sizeof(buf));
    sprintf(buf,"%dx%d@%d",1920, 1080, 60);
    GUI_DispStringAt(buf, position_x, y_start + position_y + offset);

    memset(buf, 0x0, sizeof(buf));
    sprintf(buf, "ID:%d", data_stream->stitchbase_index);
    GUI_DispStringAt(buf, position_x, y_start + position_y + (offset*2));
    data_stream->stitchbase_index +=1;

    memset(buf, 0x0, sizeof(buf));
    time(&timep);
    localtime_r(&timep, &tm_p);
    sprintf(buf, "time:%d-%d-%d %d:%d:%d", (1900 + tm_p.tm_year), ( 1 + tm_p.tm_mon), tm_p.tm_mday, \
      tm_p.tm_hour, tm_p.tm_min, tm_p.tm_sec);
    GUI_DispStringAt(buf, position_x, y_start + position_y + (offset*3));
    GUI_SetPenSize(1);

    GUI_GotoXY(0,y_start);
}
/*
	函数: void gui_osd_stitchbase_image(void)
	作用: 拼接图叠加相关字符
	参数:
      无
	返回值:
      无
*/
void gui_osd_stitchbase_image(void)
{

  unsigned int x, y, w, h;
  unsigned char font_size;
  //unsigned char num;
  string_info_t * info;
  unsigned char c_data;
  unsigned char s_data[3];
  unsigned int y_start;

  data_stream_t *data_stream = data_stream_get();
  osd_image_info_t * osd_info = &data_stream->osd_info;

  y_start = 7 * 4096;

  x = osd_info->x;
  y = osd_info->y + y_start;
  w = osd_info->x_off;
  h = osd_info->y_off;

  GUI_SetFont(gui_font[osd_info->font_format][osd_info->font_size].pNewFont);
  GUI_SetPenSize(osd_info->line_width);
  GUI_SetColor(osd_info->shape_color);
  /* 画图形 */
  switch(osd_info->shape){
    case STITCHBASE_OSD_CROSS_SHAPE:
      GUI_DrawLine(x-w-1, y-1, x+w-1, y-1);
      GUI_DrawLine(x-1, y-h-1, x-1, y+h-1);
    break;
    case STITCHBASE_OSD_BLOCK_SHAPE:
      GUI_DrawLine(x-w-1, y-h-1, x+w-1, y-h-1);
      GUI_DrawLine(x-w-1, y+h-1, x+w-1, y+h-1);
      GUI_DrawLine(x-w-1, y-h-1, x-w-1, y+h-1);
      GUI_DrawLine(x+w-1, y-h-1, x+w-1, y+h-1);
    break;
    case STITCHBASE_OSD_ROUND_SHAPE:
      GUI_DrawArc(x, y, w, h, 0, 360);
    break;
  }

  font_size = (osd_info->font_size + 1) * 8;
  GUI_GotoXY(osd_info->string_x, osd_info->string_y + y_start);
  GUI_SetColor(osd_info->font_color);
  
  /*画字符*/
  if(osd_info->data_size){
    for(int i = 0; i < (osd_info->data_size / sizeof(string_info_t)); i++){
        info = &osd_info->data[i];
        switch(info->type){
          case 0:
            s_data[0] = info->data[0];
            s_data[1] = info->data[1];
            s_data[2] = 0x0;
            GUI_DispString(s_data);
            break;
          case 1:
          case 2:
            c_data = info->data[0];
            GUI_DispChar(c_data);
            break;
          case 3:
            x = GUI_GetDispPosX();
            y = GUI_GetDispPosY();
            //gui_osd_draw_character(x + (font_size/8)*6, y + (font_size/8)*num, (font_size/8)*6, info->data[0] - '0');
            gui_osd_draw_character(x + (font_size / 2), y + (font_size / 2), (font_size / 2), info->data[0] - '0');
            GUI_GotoXY(x + font_size, y);
            break;
        }
    }
  }

  GUI_SetPenSize(1);
}

/*
	函数: void chage_src_to_new(unsigned char *src, unsigned char *dec, int new_w, int new_h)
	作用: 从画布中将图像裁剪出来
	参数:
      src：画布的buffer
      dec：裁剪后的buffer
      new_w：裁剪的宽
      new_h：裁剪的高
	返回值:
      无
*/
void chage_src_to_new(unsigned char *src, unsigned char *dec, int new_w, int new_h)
{
	int i = 0;

	for(i = 0; i < new_h; i++) {
    memcpy(dec + (i*new_w*2), src + (i *VIDEO_OSD_WIDTH*2), new_w*2);
    memset(src + (i *VIDEO_OSD_WIDTH*2), 0x0, new_w*2);
	}

	return ;
}

/*
	函数: static void* gui_paint_on_canvas_thread(void* parameter)
	作用: 编码图进行画布做画线程
	参数:
      parameter:线程参数
	返回值:
      线程返回值
*/
static void* gui_paint_on_canvas_thread(void* parameter)
{
    ring_buffer_t *rb;
    unsigned char osd_chn;
    ring_buffer_t *mpp_rb;
    ring_buffer_t *osd_rb;
    ring_buffer_t *dp_rb;
    ring_buffer_t *osd_dp_rb;
    unsigned char* gui_osd_data;
    struct timeval time;

    fpga_chn_image_info_t* chn_image;
    gpu_image_info_t *gpu_image;
    GPU_Frame_st* gpu_frame_stream = gpu_data_stream_get();
    data_stream_t *data_stream = data_stream_get();
    struct dp_send_info *dp_stream = dp_stream_get();

    image_frame_info_t frame_info = {0};
    image_frame_info_t osd_frame_info = {0};
    image_frame_info_t mpp_frame_info = {0};
    image_frame_info_t dp_frame_info = {0};

    void *out_gui_osd_addr;

    unsigned int width;
    unsigned int height;
    
    unsigned char mpp_flag;
    unsigned char dp_flag;

    double t0, t1;

    /**********/
    unsigned int y_offset;
    unsigned int uv_offset;
    unsigned char* src_buffer;
    unsigned char* y_buffer;
    unsigned char* uv_buffer;


    osd_chn = *((unsigned char*)parameter);
    SLOGI("[debug] %s %d chn:%d \n", __func__, __LINE__, osd_chn);

    rb = data_stream->chn_image[osd_chn]->chn_data_rb;
    chn_image = data_stream->chn_image[osd_chn];
    gpu_image = &gpu_frame_stream->gpu_image[osd_chn];

    gui_osd_data = gs_osd_data + (chn_image->mpp_info.mpp_chn * (4096*4096*2));
    struct timespec start_tm,end_tm;
    while(1){
      if(!gs_osd_stream->chn_info[osd_chn].gui_flag){
        break;
      }

      while(ringbuffer_is_empty(rb)){
          time.tv_sec=0;
          time.tv_usec=(1);
          select(0,NULL,NULL,NULL,&time);
          if(!gs_osd_stream->chn_info[osd_chn].gui_flag){
            goto stop;
          }
      }

      clock_gettime(CLOCK_REALTIME, &start_tm);

      ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));

      width = chn_image->mpp_info.width;
      height = chn_image->mpp_info.height;

      if(gpu_image->osd_rb_offset >= gpu_image->data_frame_max){
          gpu_image->osd_rb_offset = 0;
      }

      /*
        判断编码图是不是需要叠加时针标，如果不叠加的话，直接编码输出，这样会减少耗时
      */
      if((data_stream->gui_osd_flag & (1 << GUI_OSD_CHN_MASK))){
        gpu_yuv_to_rgb(gpu_image->in_rgb_buffer[gpu_image->osd_rb_offset], gpu_image->in_yuv_buffer[frame_info.frame_offset],\
                    width, height);

        pthread_mutex_lock(&gs_osd_stream->lock);
        pthread_mutex_lock(&data_stream->osd_lock);

        /*在画布上画时针标*/
        gui_osd_src_image(osd_chn);

        pthread_mutex_unlock(&data_stream->osd_lock);
        pthread_mutex_unlock(&gs_osd_stream->lock);

        out_gui_osd_addr = gpu_image->gui_osd_buffer[gpu_image->osd_rb_offset];
        /*裁剪出需要的画布*/
        chage_src_to_new(gui_osd_data, out_gui_osd_addr, width, height);
        clock_gettime(CLOCK_REALTIME, &end_tm);

        //SLOGI("[Debug]:%s chn:%d osd_rb diff_tm:%u \n", __func__, osd_chn, diff_timespec(&end_tm,&start_tm));

        /*拷贝到叠加的ring buffer中*/
        osd_rb = gs_osd_stream->chn_info[osd_chn].osd_rb;
        if(!ringbuffer_is_full(osd_rb)){
          osd_frame_info.chn = osd_chn;
          osd_frame_info.frame_offset = gpu_image->osd_rb_offset;
          osd_image_to_queue(osd_chn, &osd_frame_info);
          gpu_image->osd_rb_offset += 1;
        } else {
          SLOGI("[Debug]:%s chn:%d osd_rb ringbuffer_is_full \n", __func__, osd_chn);
        }

      } else { /*不需要时针标叠加*/
        y_offset = 0;
        uv_offset = 0;
        src_buffer = gpu_image->in_yuv_buffer[frame_info.frame_offset];
        y_buffer = gpu_image->out_yuv_buffer[gpu_image->osd_rb_offset];
        uv_buffer = gpu_image->out_yuv_buffer[gpu_image->osd_rb_offset] + (width*height);

        /*将pcie接收到的数据转成编码需要的格式*/
        for(int i = 0; i < (width*height*2); i++){
          if((i%2) == 0){
            y_buffer[y_offset] = src_buffer[i];
            y_offset += 1;
          } else {
            uv_buffer[uv_offset] = src_buffer[i];
            uv_offset += 1;
          }
        }

        /*放到编码的ring buffer中，后续进行编码输出*/
        mpp_rb = gpu_frame_stream->mpp_data_rb[chn_image->mpp_info.mpp_chn];
        if(chn_image->init_flag[MPP_INIT]){
          if(!ringbuffer_is_full(mpp_rb)){
            mpp_frame_info.frame_offset = gpu_image->osd_rb_offset;
            mpp_frame_info.chn = osd_chn;
            gpu_mpp_to_queue(chn_image->mpp_info.mpp_chn, &mpp_frame_info);
            //gpu_image->osd_rb_offset += 1;
            mpp_flag = 1;
          } else {
            mpp_flag = 0;
          }

        } else {
          mpp_flag = 1;
        }

        /*如果需要dp显示，则通知dp显示的线程*/
        pthread_mutex_lock(&data_stream->dp_lock);
        if(dp_stream->open_flag){
          if(data_stream->dp_chn == osd_chn){
            osd_dp_rb = gs_osd_stream->osd_dp_rb;
            if(!ringbuffer_is_full(osd_dp_rb)){
              dp_frame_info.chn = osd_chn;
              dp_frame_info.frame_offset = gpu_image->osd_rb_offset;
              //dp_frame_info.frame_offset = frame_info.frame_offset;
              dp_frame_info.t0 = frame_info.t0;
              osd_dp_image_to_queue(&dp_frame_info);
              dp_flag = 1;
            } else {
              SLOGI("[Debug] %s dp ringbuffer_is_full", __func__);
              dp_flag = 0;
            }
          } else {
            dp_flag = 1;
          }
        } else {
          dp_flag = 1;
        }
        pthread_mutex_unlock(&data_stream->dp_lock);

        if((dp_flag) && (mpp_flag)){
          gpu_image->osd_rb_offset += 1;
        }

      }
    }
stop:
    return NULL;
}

/*
	函数: void* gui_stitchbase_paint_on_canvas_thread(void* parameter)
	作用: 拼接图进行画布做画线程
	参数:
      parameter:线程参数
	返回值:
      线程返回值
*/

static void* gui_stitchbase_paint_on_canvas_thread(void* parameter)
{
    ring_buffer_t *rb;
    unsigned char osd_chn;
    ring_buffer_t *osd_rb;
    ring_buffer_t *dp_rb;
    struct timeval time;

    unsigned char* gui_osd_data;

    
    fpga_chn_image_info_t* chn_image;
    gpu_image_info_t *gpu_image;
    GPU_Frame_st* gpu_frame_stream = gpu_data_stream_get();
    data_stream_t *data_stream = data_stream_get();
    struct dp_send_info *dp_stream = dp_stream_get();

    image_frame_info_t frame_info = {0};
    image_frame_info_t osd_frame_info = {0};

    void *out_gui_osd_addr;

    unsigned int width;
    unsigned int height;
    double t0, t1;

    osd_chn = GPU_STITCHBASE_CHN;
    rb = gpu_frame_stream->osd_data_rb[osd_chn];

    chn_image = data_stream->chn_image[osd_chn];
    gpu_image = &gpu_frame_stream->gpu_image[osd_chn];

    gui_osd_data = gs_osd_data + ((chn_image->mpp_info.mpp_chn - 1) * (4096*4096*2));

    while(1){

      if(!gs_osd_stream->chn_info[osd_chn].gui_flag){
        break;
      }

      while(ringbuffer_is_empty(rb)){
          time.tv_sec=0;
          time.tv_usec=(1);
          select(0,NULL,NULL,NULL,&time);
          if(!gs_osd_stream->chn_info[osd_chn].gui_flag){
            goto stop;
          }
      }

      ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));

      width = chn_image->mpp_info.width;
      height = chn_image->mpp_info.height;

      pthread_mutex_lock(&gs_osd_stream->lock);
      pthread_mutex_lock(&data_stream->osd_lock);

      /*是否需要叠加时针标和小图通道*/
      if((data_stream->gui_osd_flag & (1 << GUI_STITCHBASE_CHN_MASK))){
        gui_osd_stitchbase_chn_image();
      }

      /*是否需要叠加图像和字符串*/
      if(!data_stream->stitchbase_osd_flag){
        gui_osd_stitchbase_image();
      } 

      pthread_mutex_unlock(&data_stream->osd_lock);
      pthread_mutex_unlock(&gs_osd_stream->lock);

      out_gui_osd_addr = gpu_image->gui_osd_buffer[frame_info.frame_offset];

      t0 = what_time_is_it_now();
      chage_src_to_new(gui_osd_data, out_gui_osd_addr, width, height);
      t1 = what_time_is_it_now();

      /*通知叠加线程*/
      osd_rb = gs_osd_stream->chn_info[osd_chn].osd_rb;
      if(!ringbuffer_is_full(osd_rb)){
        osd_frame_info.chn = osd_chn;
        osd_frame_info.frame_offset = frame_info.frame_offset;
        osd_frame_info.t0 = frame_info.t0;
        osd_image_to_queue(osd_chn, &osd_frame_info);
      } else {
        SLOGI("[Debug]:%s chn:%d osd_rb ringbuffer_is_full \n", __func__, osd_chn);
      }

    }
stop:
    return NULL;
}

/*
	函数: void *gpu_osd_overlapped_thread(void* parameter)
	作用: 进行图像的叠加
	参数:
      parameter:线程参数
	返回值:
      线程返回值
*/
static void *gpu_osd_overlapped_thread(void* parameter)
{
  unsigned char osd_chn;
  struct timeval time;
  ring_buffer_t *rb;
  ring_buffer_t *mpp_rb;
  ring_buffer_t *dp_rb;

  fpga_chn_image_info_t* chn_image;
  gpu_image_info_t *gpu_image;
  GPU_Frame_st* gpu_frame_stream = gpu_data_stream_get();
  data_stream_t *data_stream = data_stream_get();
  image_frame_info_t frame_info = {0};
  image_frame_info_t mpp_frame_info = {0};
  image_frame_info_t dp_frame_info = {0};

  uint8_t* in_rgb_addr;
  uint8_t* osd_addr;
  uint8_t* out_yuv_addr;

  unsigned int width;
  unsigned int height;

  osd_chn = *((unsigned char*)parameter);
  SLOGI("[debug] %s %d chn:%d \n", __func__, __LINE__, osd_chn);

  rb = gs_osd_stream->chn_info[osd_chn].osd_rb;

  while(1){
    if(!gs_osd_stream->chn_info[osd_chn].osd_flag){
      break;
    }

    while(ringbuffer_is_empty(rb)){
        time.tv_sec=0;
        time.tv_usec=(1);
        select(0,NULL,NULL,NULL,&time);
        if(!gs_osd_stream->chn_info[osd_chn].osd_flag){
          goto stop;
        }      
    }
    ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));
 
    chn_image = data_stream->chn_image[frame_info.chn];
    gpu_image = &gpu_frame_stream->gpu_image[frame_info.chn];

    in_rgb_addr = gpu_image->in_rgb_buffer[frame_info.frame_offset];
    osd_addr = gpu_image->gui_osd_buffer[frame_info.frame_offset];
    out_yuv_addr = gpu_image->out_yuv_buffer[frame_info.frame_offset];

    width = chn_image->mpp_info.width;
    height = chn_image->mpp_info.height;
    /*进行图像的叠加*/
    gpu_cooperate_encode_yuv(osd_addr, in_rgb_addr, out_yuv_addr, width, height);

    /*放到编码的ring buffer中，进行编码*/
    mpp_rb = gpu_frame_stream->mpp_data_rb[chn_image->mpp_info.mpp_chn];
    if((chn_image->init_flag[MPP_INIT]) && (!ringbuffer_is_full(mpp_rb))){
      mpp_frame_info.frame_offset = frame_info.frame_offset;
      mpp_frame_info.chn = frame_info.chn;
      gpu_mpp_to_queue(chn_image->mpp_info.mpp_chn, &mpp_frame_info);
    } else if(ringbuffer_is_full(mpp_rb)){
      SLOGI("[Debug] %s mpp ringbuffer_is_full", __func__);
    }
    
    /*如果需要dp显示，则放到dp数据拷贝的ring buffer中*/
    if((frame_info.chn == GPU_STITCHBASE_CHN) || (data_stream->gui_osd_flag & (1 << GUI_OSD_CHN_MASK))){
      pthread_mutex_lock(&data_stream->dp_lock);
      if(data_stream->dp_chn == frame_info.chn){
        dp_rb = gs_osd_stream->osd_dp_rb;
        if(!ringbuffer_is_full(dp_rb)){
          dp_frame_info.chn = frame_info.chn;
          dp_frame_info.frame_offset = frame_info.frame_offset;
          dp_frame_info.t0 = frame_info.t0;
          osd_dp_image_to_queue(&dp_frame_info);
        } else {
          SLOGI("[Debug] %s dp ringbuffer_is_full", __func__);
        }
      }
      pthread_mutex_unlock(&data_stream->dp_lock);
    }
  }
stop:
  SLOGI("[DEBUG]chn:%d %s stop \n", osd_chn, __func__);
  return NULL;
}

/*
	函数: void *osd_dp_send_thread(void* parameter)
	作用: dp显示数据的拷贝线程
	参数:
      parameter:线程参数
	返回值:
      线程返回值
*/
static void *osd_dp_send_thread(void* parameter)
{
    struct timeval time;
    unsigned int width;
    unsigned int height;

    ring_buffer_t *dp_rb;

    void *gpu_rgb_addr;
    void *dp_buf;

    gpu_image_info_t *gpu_image;
    fpga_chn_image_info_t* chn_image;
    
    image_frame_info_t frame_info = {0};
    image_frame_info_t dp_frame_info = {0};

    ring_buffer_t *rb = gs_osd_stream->osd_dp_rb;

    GPU_Frame_st* gpu_frame_stream = gpu_data_stream_get();
    data_stream_t *data_stream = data_stream_get();
    struct dp_send_info *dp_stream = dp_stream_get();

    unsigned int y_offset;
    unsigned int uv_offset;
    unsigned char* src_buffer;
    unsigned char* y_buffer;
    unsigned char* uv_buffer;

    while(1){

      while(ringbuffer_is_empty(rb)){
          time.tv_sec=0;
          time.tv_usec=(1);
          select(0,NULL,NULL,NULL,&time);
      }

      ringbuffer_get(rb, (uint8_t *)&frame_info, sizeof(image_frame_info_t));

      chn_image = data_stream->chn_image[frame_info.chn];
      gpu_image = &gpu_frame_stream->gpu_image[frame_info.chn];

      width = chn_image->mpp_info.width;
      height = chn_image->mpp_info.height;

      if(dp_stream->dp_rb_offset >= dp_stream->data_frame_max){
        dp_stream->dp_rb_offset = 0;
      }

      if(!dp_stream->open_flag){
        SLOGI("[DENIG] %s continue\n", __func__);
        continue;
      }

      dp_buf = (void*)dp_stream->yuv_data[dp_stream->dp_rb_offset].vir_addr;

#if 0
      /*根据是不是进行了osd叠加，从不同的buffer中将数据拷贝到dp的ring buffer中*/
      if((frame_info.chn == GPU_STITCHBASE_CHN) || (data_stream->gui_osd_flag & (1 << GUI_OSD_CHN_MASK))){
        gpu_rgb_addr = gpu_image->out_yuv_buffer[frame_info.frame_offset];
        memcpy(dp_buf, gpu_rgb_addr, (width * height * 2));
      } else { /*如果是编码图没有叠加时针标*/
        y_offset = 0;
        uv_offset = 0;
        src_buffer = gpu_image->in_yuv_buffer[frame_info.frame_offset];
        y_buffer = dp_buf;
        uv_buffer = dp_buf + (width*height);

        for(int i = 0; i < (width*height*2); i++){
          if((i%2) == 0){
            y_buffer[y_offset] = src_buffer[i];
            y_offset += 1;
          } else {
            uv_buffer[uv_offset] = src_buffer[i];
            uv_offset += 1;
          }
        }

      }
#else
      gpu_rgb_addr = gpu_image->out_yuv_buffer[frame_info.frame_offset];
      memcpy(dp_buf, gpu_rgb_addr, (width * height * 2));
#endif
      /*放到DP显示的线程中*/
      dp_rb = dp_stream->data_rb;
      if(!ringbuffer_is_full(dp_rb)){
        dp_frame_info.frame_offset = dp_stream->dp_rb_offset;;
        fill_image_to_queue(&dp_frame_info);
        dp_stream->dp_rb_offset += 1;
      } else {
        SLOGI("[Debug] %s ringbuffer_is_full offset:%d \n", __func__, dp_stream->dp_rb_offset);
      }
    }
}

/*
	函数: int osd_ringbuffer_init(void)
	作用: osd 相关的ring buffer初始化
	参数:
      无
	返回值:
      正常返回0，失败返回其他
*/
int osd_ringbuffer_init(void)
{
  int ret;
  int chn;
  gs_osd_stream->osd_dp_rb = ringbuffer_create(RINGBUFFER_FRAME_MAX*sizeof(image_frame_info_t), sizeof(image_frame_info_t));
  if (!gs_osd_stream->osd_dp_rb) {
      printf("[ERROR] create osd dp rb buffer failed\n");
      return A818_OSD_DP_RB;
  }


  for(chn = 0; chn < DATA_CHANNEL_NUM_MAX;chn++){
    gs_osd_stream->chn_info[chn].osd_rb = ringbuffer_create(RINGBUFFER_FRAME_MAX*sizeof(image_frame_info_t), sizeof(image_frame_info_t));
    if (!gs_osd_stream->chn_info[chn].osd_rb) {
        printf("[ERROR] create osd rb buffer failed\n");
        ret = A818_OSD_RB;
        goto err_1;
    }
  }

  return A818_SUCCESS;

err_1:
  for(int i = 0; i < chn; i++){
    if(gs_osd_stream->chn_info[chn].osd_rb){
      ringbuffer_destory(gs_osd_stream->chn_info[chn].osd_rb);
    }
  }
  ringbuffer_destory(gs_osd_stream->osd_dp_rb);
  return ret;
}

/*
	函数: int gui_osd_overlapped_chn_init(unsigned char chn)
	作用: 根据不同的通道，初始化不同的作画线程
	参数:
      无
	返回值:
      正常返回0，失败返回其他
*/
int gui_osd_overlapped_chn_init(unsigned char chn)
{
  int ret;
  data_stream_t *data_stream = data_stream_get();
  fpga_chn_image_info_t* chn_image = data_stream->chn_image[chn];
  gs_osd_stream->chn_info[chn].chn = chn;
  gs_osd_stream->chn_info[chn].osd_flag = 1;
  chn_image->frame_num = 0;

  gs_osd_stream->chn_info[chn].gui_flag = 1;


  if(chn != GPU_STITCHBASE_CHN){
    ret = pthread_create(&gs_osd_stream->chn_info[chn].tid, NULL, &gui_paint_on_canvas_thread, &gs_osd_stream->chn_info[chn].chn);
    if (ret != 0) {
        printf("[ERROR] Create gui_paint_on_canvas_thread failed %d\n",ret);
        return A818_OSD_GPU_THREAD;
    }
  } else {
    ret = pthread_create(&gs_osd_stream->chn_info[chn].tid, NULL, &gui_stitchbase_paint_on_canvas_thread, &gs_osd_stream->chn_info[chn].chn);
    if (ret != 0) {
        printf("[ERROR] Create gui_paint_on_canvas_thread failed %d\n",ret);
        return A818_OSD_GPU_THREAD;
    }
  }

  /*初始化叠加线程*/
  ret = pthread_create(&gs_osd_stream->chn_info[chn].osd_tid, NULL, &gpu_osd_overlapped_thread, &gs_osd_stream->chn_info[chn].chn);
  if (ret != 0) {
      printf("[ERROR] Create gpu_osd_overlapped_thread failed %d\n",ret);
      return A818_OSD_GPU_THREAD;
  }
  SLOGI("[Debug] %s chn:%d \n", __func__, chn);
  return 0;
}

/*
	函数: void gui_osd_overlapped_chn_destory(unsigned char chn)
	作用: 相同的通道osd资源的释放
	参数:
      chn：释放资源的通道号
	返回值:
      无
*/
void gui_osd_overlapped_chn_destory(unsigned char chn)
{
  if(gs_osd_stream->chn_info[chn].osd_flag){
    gs_osd_stream->chn_info[chn].osd_flag = 0;
    SLOGI("[Debug] %s chn:%d \n", __func__, chn);
    xt_nanosleep(0, 100*1000);
  }

  if(gs_osd_stream->chn_info[chn].gui_flag){
    gs_osd_stream->chn_info[chn].gui_flag = 0;
    SLOGI("[Debug] %s chn:%d \n", __func__, chn);
    xt_nanosleep(0, 100*1000);
  }
  usleep(1000);
}

/*
	函数: void osd_ringbuffer_destory(void)
	作用: osd ring buffer资源的释放
	参数:
      无
	返回值:
      无
*/
void osd_ringbuffer_destory(void)
{
  int chn;
  for(chn = 0; chn < DATA_CHANNEL_NUM_MAX;chn++){
    if(gs_osd_stream->chn_info[chn].osd_rb){
      ringbuffer_destory(gs_osd_stream->chn_info[chn].osd_rb);
    }
  }
  ringbuffer_destory(gs_osd_stream->osd_dp_rb);
}

/*
	函数: void gui_osd_ringbuffer_is_empty(void)
	作用: 判断osd相关的ring buffer是否使用完
	参数:
      无
	返回值:
      无
*/
void gui_osd_ringbuffer_is_empty(void)
{
  int chn;

  image_frame_info_t frame_info = {0};
  for(chn = 0; chn < DATA_CHANNEL_NUM_MAX;chn++){
    while(!ringbuffer_is_empty(gs_osd_stream->chn_info[chn].osd_rb)){
      xt_nanosleep(0, 100); 
    }
  }

  while(!ringbuffer_is_empty(gs_osd_stream->osd_dp_rb)){
    xt_nanosleep(0, 100);  
  }
}

/*
	函数: void ucgui_lib_init(unsigned char **pic_buf)
	作用: ucgui库的初始化
	参数:
      无
	返回值:
      无
*/
void ucgui_lib_init(unsigned char **pic_buf)
{

	set_gui_addr(pic_buf,1);
	UcguiInitPara();
	GUI_Init();
 
	GUI_Clear();
  GUI_SetFont(&GUI_FontHZ32x32_heiti);
  GUI_SetColor(GUI_GREEN);
	GUI_SetTextMode(GUI_TEXTMODE_TRANS);
}

/*
	函数: int gui_osd_init(void)
	作用: osd相关资源初始化
	参数:
      无
	返回值:
      正常返回0，失败返回其他
*/
int gui_osd_init(void)
{
  int ret;

  gs_osd_data = (unsigned char *)malloc(4096 * 4096 * 8*2);
  if(gs_osd_data == NULL){
    printf("[err] ucgui lib osd buffer malloc error \n");
    return A818_OSD_GUI_MALLOC;
  }

  memset(gs_osd_data, 0x0, (4096 * 4096 * 8*2));
  ucgui_lib_init(&gs_osd_data);

  gs_osd_stream = (struct osd_data_stream_t*)malloc(sizeof(struct osd_data_stream_t));
  if(!gs_osd_stream){
    printf("malloc osd stream error \n");
    return A818_OSD_GUI_STREAM_MALLOC;
  }


  memset(gs_osd_stream, 0x0, sizeof(struct osd_data_stream_t));

  ret = osd_ringbuffer_init();
  if(ret != A818_SUCCESS){
    printf("[ERROR] osd_ringbuffer_init error \n");
    ret = A818_OSD_GUI_RB;
    goto err_1;
  }


  ret = pthread_create(&gs_osd_stream->dp_send_tid, NULL, &osd_dp_send_thread, NULL);
  if (ret != 0) {
      printf("[ERROR] Create osd_dp_send_thread failed %d\n",ret);
      ret = A818_OSD_DP_THREAD;
      goto err_2;
  }

  pthread_mutex_init(&gs_osd_stream->lock,NULL);

	return A818_SUCCESS;
err_2:
  osd_ringbuffer_destory();
err_1:
  free(gs_osd_stream);
  return ret;
}

/*
	函数: void gui_osd_destory(void)
	作用: osd相关资源的释放
	参数:
      无
	返回值:
      无
*/
void gui_osd_destory(void)
{
  pthread_cancel(gs_osd_stream->dp_send_tid);
  osd_ringbuffer_destory();
  free(gs_osd_stream);
  return ;
}