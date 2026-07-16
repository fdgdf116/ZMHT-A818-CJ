#ifndef PCIE_ENGINE_H_
#define PCIE_ENGINE_H_

#include "types_def.h"
#include "ringbuffer.h"
#include "vivado_common.h"

#ifdef __cplusplus
extern "C" {
#endif
void pcie_fpga_reg_reset(void);

int pcie_chn_image_reg_init(fpga_reg_val_info_t* reg_val_info);
void pcie_pc_chn_image_reg_init(fpga_reg_val_info_t* reg_val_info);

void pcie_set_rk_enable(unsigned char val);
void pcie_set_pc_enable(unsigned char val);

int fpga_pcie_device_open(void);
void fpga_pcie_device_close(void);

int pcie_fpga_data_init(void);
void pcie_fpga_data_destory(void);

void pcie_close_chn_enabne(unsigned int chn);
void pcie_fpga_device_stop(void);

void pcie_get_selftest_val(selftest_val_info_t* info);


unsigned int pcie_get_image_width(unsigned char chn);
unsigned int pcie_get_image_height(unsigned char chn);
unsigned int pcie_get_image_fps(unsigned char chn);
int pcie_mpp_data_send(unsigned char* mpp_data, unsigned int size);

#ifdef __cplusplus
}
#endif
#endif /* GPU_GPU_INTERFACE_H_ */
