#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h> 
#include <fcntl.h> 
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>
#include <linux/videodev2.h>

#include "types_def.h"
#include "ringbuffer.h"
#include "data_engine.h"
#include "gpu_interface.h"
#include "mpi_encode.h"
#include "osd_engine.h"
#include "log.h"
#include "pcie_engine.h"
#include "dp_engine.h"
#include "fifo_engine.h"
#include "pcie_impl.h"
#include "pcie_reg_rw.h"
#include "rs232_engine.h"
#include "a818_error.h"

static void sig_handler(int sig)
{
    axififo_data_destory();
    rs232_send_data_destory();
    pcie_fpga_data_destory();
    mpp_encode_destory();
    gui_osd_destory();
    drm_dp_data_destory();
    data_stream_destory();
    fpga_pcie_device_close();
    gpu_lib_destory();
    exit(0);
}


int main(int argc, char *argv[])
{
    int ret = 0; 
    
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGKILL, sig_handler);
    signal(SIGTERM, sig_handler);

    SLOGI("----------------------------------\n");
    SLOGI(" Firmware compile time:%s %s\n", __DATE__, __TIME__);
    SLOGI("----------------------------------\n");
    
    ret = gpu_lib_init();
    if(ret != A818_SUCCESS){
        SLOGI("LIB GPU init error ret = %d \n", ret);
        return 1;
    } else {
        SLOGI("LIB GPU init ok \n");
    }

    ret = fpga_pcie_device_open();
    if(ret != A818_SUCCESS){
        SLOGI("fpga pcide deivce error ret = %d \n", ret);
        goto err_1;
    } else {
        SLOGI("fpga pcide deivce init ok \n");
    }

    ret = data_stream_init();
    if(ret != A818_SUCCESS){
        SLOGI("data stream init error ret = %d \n", ret);
        goto err_2;
    } else {
        SLOGI("data stream init ok \n");
    }

    ret = drm_dp_data_init();
    if((ret != A818_SUCCESS) && (ret != A818_DP_READ_MODE)){
        SLOGI("dp send init error ret = %d \n", ret);
        goto err_3;
    } else {
        SLOGI("dp send init ok \n");
    }

    ret = gui_osd_init();
    if(ret != A818_SUCCESS){
        SLOGI("a818 osd init error ret = %d \n", ret);
        goto err_4;
    } else {
        SLOGI("a818 osd init ok \n");
    }

    ret = mpp_encode_init();
    if(ret != A818_SUCCESS){
        SLOGI("encode frame init error ret = %d \n", ret);
        goto err_5;
    } else {
        SLOGI("encode frame init ok \n");
    }

    ret = pcie_fpga_data_init();
    if(ret != A818_SUCCESS){
        SLOGI("pcie data init error ret = %d \n", ret);
        goto err_6;
    } else {
        SLOGI("fpga pcie data init ok \n");
    }

    sleep(2);

    ret = rs232_send_data_init();
    if(ret != A818_SUCCESS){
        SLOGI("rs232 init error \n");
        goto err_7;
    } else {
        SLOGI("rs232 init ok \n");
    }

    ret = axififo_data_init();
    if(ret != A818_SUCCESS){
        SLOGI("axififo init error \n");
        goto err_8;
    } else {
        SLOGI("axififo init ok \n");
    }


    
    while(1){
        sleep(1);
    }


    return 0;
err_8:
    rs232_send_data_destory();
err_7:
    pcie_fpga_data_destory();
err_6:
    mpp_encode_destory();
err_5:
    gui_osd_destory();
err_4:
    drm_dp_data_destory();
err_3:
    data_stream_destory();
err_2:
    fpga_pcie_device_close();
err_1:
    gpu_lib_destory();
    return ret;
}
