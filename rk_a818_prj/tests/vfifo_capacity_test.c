#include "vfifo_capacity.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint32_t full = 268435456U / 32;
    const uint32_t reserve_limit = (268435456U - 33554432U) / 32;
    const uint32_t packet_units = 32832U / 32;
    assert(rk2fpga_vfifo_free_bytes(0) == 268435456U);
    assert(rk2fpga_vfifo_can_write(0, 32832));
    assert(!rk2fpga_vfifo_can_write(reserve_limit, 32832));
    assert(!rk2fpga_vfifo_can_write(reserve_limit + 1, 32832));
    assert(!rk2fpga_vfifo_can_write(reserve_limit - 1, 32832));
    assert(rk2fpga_vfifo_can_write(reserve_limit - packet_units, 32832));
    assert(!rk2fpga_vfifo_can_write(reserve_limit - packet_units + 1, 32832));
    assert(!rk2fpga_vfifo_can_write(full, 32832));
    assert(rk2fpga_vfifo_free_bytes(full + 1) == 0);
    assert(rk2fpga_vfifo_free_bytes(UINT32_MAX) == 0);
    assert(!rk2fpga_vfifo_can_write(UINT32_MAX, 32832));
    assert(!rk2fpga_vfifo_can_write(0, UINT64_MAX));
    puts("VFIFO capacity boundary tests passed");
    return 0;
}
