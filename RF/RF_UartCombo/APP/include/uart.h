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


/* Shared RAM with the Device-role USB buffers; leaves room for stack. */
#define  UART_BUF_LEN   1280

#define    TXD_PIN   (1<<0)  // PA0
#define    RXD_PIN   (1<<1)  // PA1                                                 


/**
 * @brief   DTR/RTS功能与ST一键下载功能
 * @note    默认开启ST一键下载功能且关闭DTR/RTS功能；
 *          如需开启DTR/RTS功能，将DTR_RTS_FUNC置1即可；
 *          DTR / RESET_PIN     PA2
 *          RTS / BOOT_PIN      PA3
 */

 
#ifdef DTR_RTS_FUNC
#undef DTR_RTS_FUNC
#endif
/* PA2/PA7 are role keys and target RESET/BOOT in the combined image. */
#define DTR_RTS_FUNC FALSE
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == TRUE))
#define    DTR_PIN        (1<<2)
#define    RTS_PIN        (1<<3)
#else
#define    RESET_PIN      (1<<2)
#define    BOOT_PIN       (1<<7)
#endif


/**
 * @brief   LED 功能与定义
 * @note    The combined image controls the PA3 dual-LED directly.
 */
#ifdef LED_FUNC
#undef LED_FUNC
#endif
#define LED_FUNC FALSE
#if((defined(LED_FUNC)) && (LED_FUNC == TRUE))
#define    LED_PIN        (1<<3) 
#endif


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

extern uint8_t gBoundStatus;

void UART_Init(void);
uint8_t UART_RxQuery( void *buf, typeBufSize *len );
void UART_SetBuad( uint32_t buad );
void UART_SetTimer( uint16_t ms );
void UART_Send( char* data, uint16_t size);

#endif /* BLE_DIRECTTEST_APP_INCLUDE_UART_H */
