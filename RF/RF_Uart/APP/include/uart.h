/********************************** (C) COPYRIGHT *******************************
 * File Name          : uart.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2022/06/30
 * Description        : 
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/


#ifndef BLE_DIRECTTEST_APP_INCLUDE_UART_H
#define BLE_DIRECTTEST_APP_INCLUDE_UART_H

#include "buf.h"


#define  UART_BUF_LEN   (1024*3)

#define    TXD_PIN   (1<<0)  // PA0
#define    RXD_PIN   (1<<1)  // PA1                                                 


/**
 * @brief   DTR/RTS¹¦ÄÜÓëSTÒ»¼üÏÂÔØ¹¦ÄÜ
 * @note    Ä¬ÈÏ¿ªÆôSTÒ»¼üÏÂÔØ¹¦ÄÜÇÒ¹Ø±ÕDTR/RTS¹¦ÄÜ£»
 *          ÈçÐè¿ªÆôDTR/RTS¹¦ÄÜ£¬½«DTR_RTS_FUNCÖÃ1¼´¿É£»
 *          Device mode: PA2 is target NRST/local self-boot key;
 *          PA7 is target BOOT and remains high-Z while idle.
 */

 
#ifndef DTR_RTS_FUNC
// #define DTR_RTS_FUNC     TRUE
#define DTR_RTS_FUNC     FALSE
#endif 
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == TRUE))
#define    DTR_PIN        (1<<2)
#define    RTS_PIN        (1<<3)
#else
#define    RESET_PIN      (1<<2)
#define    BOOT_PIN       (1<<7)
#endif

/* Target download timing, in milliseconds. Tune these for the target MCU and
 * the board-level BOOT polarity circuit. */
#ifndef DEVICE_TARGET_BOOT_SETUP_MS
#define DEVICE_TARGET_BOOT_SETUP_MS       50u
#endif
#ifndef DEVICE_TARGET_RESET_LOW_MS
#define DEVICE_TARGET_RESET_LOW_MS        50u
#endif
#ifndef DEVICE_TARGET_BOOT_HOLD_MS
#define DEVICE_TARGET_BOOT_HOLD_MS       100u
#endif


/**
 * @brief   LED ¹¦ÄÜÓë¶¨Òå
 * @note    LED status indicator on PA3
 */
#ifndef LED_FUNC
/* PA3 is driven by the single-role connection/status indicator. */
#define LED_FUNC     FALSE
#endif
#define LED_PIN      (1<<3)


enum uart_status
{
    UART_STATUS_IDLE,
    UART_STATUS_START,
    UART_STATUS_RCVING,
    UART_STATUS_RCV_END,
    UART_STATUS_SENDING,
    UART_STATUS_SEND,
    UART_STATUS_NUM,
};

#define  DATA_LEN_UART       (32)

extern volatile uint8_t gBoundStatus;

void UART_Init(void);
uint8_t UART_RxQuery( void *buf, typeBufSize *len );
void UART_SetBuad( uint32_t buad );
void UART_SetTimer( uint16_t ms );
void UART_Send( char* data, uint16_t size);

#endif /* BLE_DIRECTTEST_APP_INCLUDE_UART_H */
