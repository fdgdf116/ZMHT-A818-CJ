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

#include "pcie_engine.h"
#include "uart_comm.h"
#include "a818_error.h"
#include "xutils.h"
#include "vivado_common.h"
#include "log.h"

static char *uart_device_name = "/dev/ttyS4";
static uart_param_t uart_param;
static selftest_val_info_t selftest_info;

/*
	函数: static void *rs232_send_data_thread(void* parameter)
	作用: rs232发送线程
	参数:
        线程参数
	返回值:
        线程返回值
*/
static void *rs232_send_data_thread(void* parameter)
{
    unsigned char send_data[64];

    while(1){
        /*获取pcie自检信息*/
        pcie_get_selftest_val(&selftest_info);

        memset(send_data, 0x0, sizeof(send_data));
        sprintf(send_data, "board_type:%d series_number:%d \r\n", htonl(selftest_info.board_type), htonl(selftest_info.series_number));
        uart_send(&uart_param, send_data, sizeof(send_data));

        memset(send_data, 0x0, sizeof(send_data));
        sprintf(send_data, "fpga_version:%d   rk_version:%s \r\n", htonl(selftest_info.version1), A818_RK3588_VERSION);
        uart_send(&uart_param, send_data, sizeof(send_data));

        memset(send_data, 0x0, sizeof(send_data));
        sprintf(send_data, "link:%d  pc_link:%d  rk_link:%d \r\n", htonl(selftest_info.link), selftest_info.pc_link, selftest_info.rk_link);
        uart_send(&uart_param, send_data, sizeof(send_data));

        memset(send_data, 0x0, sizeof(send_data));
        sprintf(send_data, "-------------------------------------------\r\n");
        uart_send(&uart_param, send_data, sizeof(send_data));

        xt_nanosleep(2,0);
    }
}

/*
	函数: int rs232_send_data_init(void)
	作用: rs232资源初始化
	参数:
        无
	返回值:
        正确返回0，失败返回其他
*/
int rs232_send_data_init(void)
{
    int ret;
    pthread_t rs232_send;
    ret = uart_init(&uart_param,uart_device_name, 115200, "none", 8, 1);
    if(ret != 0){
        SLOGI("RS232_init error \n");
        ret = A818_RS232_INIT;
        return ret;
    }

    ret = pthread_create(&rs232_send,NULL,&rs232_send_data_thread,NULL);;
    if (ret != 0) {
        printf("[ERROR] Create pcie data analysis thread failed %d\n",ret);
        ret = A818_RS232_THREAD;
        goto err;
    }

    return 0;
err:
    uart_close(&uart_param);
    return ret;
}

/*
	函数: void rs232_send_data_destory(void)
	作用: rs232资源初释放
	参数:
        无
	返回值:
        无
*/
void rs232_send_data_destory(void)
{
    uart_close(&uart_param);
}


