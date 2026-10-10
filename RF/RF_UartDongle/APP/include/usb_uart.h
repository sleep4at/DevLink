/********************************** (C) COPYRIGHT *******************************
* File Name          : usb_uart.h
* Author             : WCH
* Version            : V1.0
* Date               : 2025/04/22
* Description        : 
*******************************************************************************/

#ifndef __USB_UART_H
#define __USB_UART_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "CH57x_common.h"
#include "buf.h"


/**
 * @brief  LED ¹¦ÄÜÓë¶¨Òå Ä¬ÈÏ¿ªÆô
 * @note   LED status indicator on PA3
 */
#ifndef LED_FUNC
/* PA3 is driven by the single-role Host connection/status indicator. */
#define LED_FUNC     FALSE
#endif 
#define LED_PIN      (1<<3)



//Line Code½á¹¹
 typedef struct __PACKED _LINE_CODE
{
  uint32_t  BaudRate;   /* ²¨ÌØÂÊ */
  uint8_t StopBits;   /* Í£Ö¹Î»¼ÆÊý£¬0£º1Í£Ö¹Î»£¬1£º1.5Í£Ö¹Î»£¬2£º2Í£Ö¹Î» */
  uint8_t ParityType;   /* Ð£ÑéÎ»£¬0£ºNone£¬1£ºOdd£¬2£ºEven£¬3£ºMark£¬4£ºSpace */
  uint8_t DataBits;   /* Êý¾ÝÎ»¼ÆÊý£º5£¬6£¬7£¬8£¬16 */
  uint8_t ioStaus;
}LINE_CODE, *PLINE_CODE;
extern LINE_CODE Uart0Para;

void USB_Init( void );
void USB_StatusQuery( void );
uint8_t USB_RxQuery( void *buf, typeBufSize *len );

#ifdef __cplusplus
}
#endif

#endif
