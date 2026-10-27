#ifndef VFIFO_CAPACITY_H
#define VFIFO_CAPACITY_H

#include <stdint.h>

/* FPGA register 0x104 reports occupied 32-byte units. */
#define RK2FPGA_VFIFO_CAPACITY_BYTES UINT64_C(268435456)
#define RK2FPGA_VFIFO_RESERVE_BYTES UINT64_C(33554432)

static inline uint64_t rk2fpga_vfifo_free_bytes(uint32_t count)
{
    uint64_t used = (uint64_t)count * 32;
    return used >= RK2FPGA_VFIFO_CAPACITY_BYTES ? 0 :
           RK2FPGA_VFIFO_CAPACITY_BYTES - used;
}

static inline int rk2fpga_vfifo_can_write(uint32_t count, uint64_t bytes)
{
    uint64_t available = rk2fpga_vfifo_free_bytes(count);
    return available > RK2FPGA_VFIFO_RESERVE_BYTES &&
           bytes <= available - RK2FPGA_VFIFO_RESERVE_BYTES;
}
#endif
