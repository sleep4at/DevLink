#ifndef ROLE_BUFFERS_H
#define ROLE_BUFFERS_H

#include <stdint.h>
#include "uart.h"

#define ROLE_RF_BUFFER_SIZE       512u
#define ROLE_UART_BUFFER_SIZE     UART_BUF_LEN
#define ROLE_USB_RING_SIZE        512u
#define ROLE_USB_EP_SIZE          64u

/* Host and Device never run at the same time, so their large byte arrays
 * intentionally occupy the same RAM. Keep all protocol state outside here. */
typedef union __attribute__((aligned(4))) {
    struct {
        uint8_t rf_data[ROLE_RF_BUFFER_SIZE];
        uint8_t uart_rx[ROLE_UART_BUFFER_SIZE];
    } host;
    struct {
        uint8_t rf_data[ROLE_RF_BUFFER_SIZE];
        uint8_t usb_ring[ROLE_USB_RING_SIZE];
        uint8_t ep0_send[256];
        uint8_t ep1_out[ROLE_USB_EP_SIZE];
        uint8_t ep2[2 * ROLE_USB_EP_SIZE];
        uint8_t ep1[ROLE_USB_EP_SIZE];
        uint8_t ep0[ROLE_USB_EP_SIZE];
        uint8_t ch341_reg_add[10];
        uint8_t ch341_reg_val[10];
    } device;
} role_buffer_storage_t;

extern role_buffer_storage_t gRoleBufferStorage;

#endif
