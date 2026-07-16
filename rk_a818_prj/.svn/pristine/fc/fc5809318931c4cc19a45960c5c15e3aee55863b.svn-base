#ifndef _UART_COMM_H
#define _UART_COMM_H

#include <stdint.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif 

/* This structure is byte-aligned */
typedef struct {
        /* Descriptor (tty) */
        int fd;
        char device[32];
        /* Bauds: 9600, 19200, 57600, 115200, etc */
        int baud;
        /* Data bit */
        uint8_t data_bit;
        /* Stop bit */
        uint8_t stop_bit;
        /* Parity: "even", "odd", "none" */
        char parity[6];
	/* Save old termios settings */
        struct termios old_tios;
}uart_param_t;

/* Initializes the gprs_param structure
   - device: "/dev/ttyS0"
   - baud:   9600, 19200, 57600, 115200, etc
   - parity: "even", "odd" or "none" 
   - data_bits: 5, 6, 7, 8 
   - stop_bits: 1, 2 
*/
int uart_init(uart_param_t *uart_param, const char *device,
                     int baud, const char *parity, int data_bit,
                     int stop_bit);

uint32_t uart_send(uart_param_t *uart_param, uint8_t *buf, int len);
int uart_recv(uart_param_t *uart_param,uint8_t *buf, int len);
void uart_close(uart_param_t *uart_param);
int SelectData(int fd_);
#ifdef __cplusplus
}
#endif

#endif