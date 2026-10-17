/********************************** (C) COPYRIGHT *******************************
 * File Name          : uart.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2022/06/30
 * Description        : 
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#include "CH57x_common.h"
#include "uart.h"
#include "rf_uart_tx.h"
#include "rf.h"



static uint8_t uart_buf[UART_BUF_LEN];
static struct simple_buf *pUartbuf = NULL;
static struct simple_buf uart_buffer;

volatile uint8_t  uart_flag;
uint32_t gBaudRate;
uint32_t gSysClock;
uint32_t gIntervalTimer;
uint32_t gUartRxCount;


#define  BOUND_GET_PERI   10 // 发送绑定请求


/*********************************************************************
 * @fn      uart_buffer_create
 *
 * @brief   Create a file called uart_buffer simple buffer of the buffer
 *          and assign its address to the variable pointed to by the buffer pointer.
 *
 * @param   buf    -   a parameter buf pointing to a pointer.
 *
 * @return  none
 */
static void uart_buffer_create(struct simple_buf **buf)
{
    *buf = simple_buf_create(&uart_buffer, uart_buf, sizeof(uart_buf));
}

/*********************************************************************
 * @fn      uart_start_timeout
 *
 * @brief
 *
 * @param   none
 *
 * @return  none
 */
__HIGH_CODE
static void uart_rx_timeout( void )
{
    R32_TMR_CNT_END = (R16_UART_DL*8)*100; // 超时时间设置为100bit
    R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R8_TMR_CTRL_MOD = RB_TMR_COUNT_EN;
}

/*********************************************************************
 * @fn      TMR0_IRQHandler
 *
 * @brief   TMR0中断函数
 *
 * @return  none
 */
__INTERRUPT
__HIGH_CODE
void TMR_IRQHandler(void) // 定时中断
{
    if(TMR_GetITFlag(TMR_IT_CYC_END))
    {
        TMR_ClearITFlag( TMR_IT_CYC_END );
        R32_TMR_CNT_END = gIntervalTimer;
        R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
        R8_TMR_CTRL_MOD = RB_TMR_COUNT_EN;
        if( uart_flag == UART_STATUS_START)
        {
            uart_flag = UART_STATUS_SENDING;
        }
        else if( uart_flag == UART_STATUS_RCVING )
        {
            uart_flag = UART_STATUS_RCV_END;
        }
        else if( uart_flag == UART_STATUS_SENDING)
        {
            uart_flag = UART_STATUS_SEND;
        }
    }
}

__HIGH_CODE
void UART_SetTimer( uint16_t ms )
{
    gSysClock = GetSysClock();
    gIntervalTimer =  gSysClock/2000*ms;
    R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R8_TMR_CTRL_MOD = RB_TMR_COUNT_EN;
}

__HIGH_CODE
void UART_SetBuad( uint32_t buad )
{
    if( gBaudRate != buad )
    {
        uint32_t x;
        gSysClock = GetSysClock();
        PRINT("bsp = %d\n",buad);
        mDelaymS(1);
        gBaudRate = buad;
        x = 10 * gSysClock/ 8 / buad;
        x = (x + 5) / 10;
        R16_UART_DL = (uint16_t)x;
    }
}

/************************************************************************
 * @fn      UART_RxQuery
 *
 * @brief   查询串口状态
 *
 * @return  0：发送数据   0x80 发送状态或者命令  0xFF 无事件
 */
uint8_t UART_RxQuery( void *buf, typeBufSize *len )
{
    uint32_t irq_state = __risc_v_disable_irq();
    uint8_t result = 0xFF;

    if( uart_flag == UART_STATUS_RCV_END )
    {
        if( gBoundStatus == BOUND_STATUS_EST )
        {
            if( !*len )
            {
                /* The Host has no receive credit. Keep the UART bytes and
                 * turn this timer event into a GET_STATUS credit probe. */
                uart_flag = UART_STATUS_START;
                result = 0x80;
            }
            else if( read_buf( pUartbuf, buf, len ) == 0 )
                uart_flag = UART_STATUS_START;
            if( *len ) result = 0;
        }
        else
        {
            /* Retain UART bytes until pairing is established; send a bind
             * request rather than DATA on the advertising channel. */
            *len = 0;
            uart_flag = UART_STATUS_START;
            result = 0x80;
        }
    }
    else if( uart_flag == UART_STATUS_SEND )
    {
        /* Reconnecting can leave queued bytes with the timer flag SEND. */
        if( gBoundStatus == BOUND_STATUS_EST && pUartbuf->data_len )
        {
            if( !*len )
            {
                /* Do not consume bytes until the Host advertises space. */
                result = 0x80;
            }
            else
            {
                read_buf( pUartbuf, buf, len );
                result = 0;
            }
        }
        else
        {
            *len = 0;
            result = 0x80;
        }
        uart_flag = UART_STATUS_START;
    }
    else
    {
        *len = 0;
    }
    __risc_v_enable_irq( irq_state );
    return result;
}
/************************************************************************
 * @fn      UART_Send
 *
 * @brief   UART发送数据
 *
 * @return  none
 */
void UART_Send( char* data, uint16_t size)
{
    for( int i=0; i<size; i++)
    {
        while( R8_UART_TFC == UART_FIFO_SIZE );                        /* 等待数据发送 */
        R8_UART_THR = *data++;                                         /* 发送数据 */
    }
//    while (( R8_UART_LSR & RB_LSR_TX_ALL_EMP) == 0); // 等待发送完成
}

/************************************************************************
 * @fn      UART_IRQHandler
 *
 * @brief   UART中断函数
 *
 * @return  none
 */
__INTERRUPT
__HIGH_CODE
void UART_IRQHandler(void)
{
    uint8_t i;
    uint8_t tmp_buf[UART_FIFO_SIZE];
    typeBufSize len;

    switch(UART_GetITFlag())
    {
        case UART_II_LINE_STAT: // 线路状态错误
        {
            (void)UART_GetLinSTA();
            break;
        }

        case UART_II_RECV_RDY: // 数据达到设置触发点
            len = R8_UART_RFC;
            if( !len ) break;
            if( len > UART_FIFO_SIZE ) len = UART_FIFO_SIZE;
            i = len;
            do{
                tmp_buf[len-i] = UART_RecvByte();
            }while(--i);
            if( write_buf( pUartbuf,tmp_buf,&len ) >= DATA_LEN_UART )
            {
                uart_flag = UART_STATUS_RCV_END;
            }
            else
            {
                uart_flag = UART_STATUS_RCVING;
                uart_rx_timeout( );
            }
            gUartRxCount += len;
            break;

        case UART_II_RECV_TOUT: // 接收超时，暂时一帧数据接收完成
            len = R8_UART_RFC;
            if( !len ) break;
            if( len > UART_FIFO_SIZE ) len = UART_FIFO_SIZE;
            i = len;
            do{
                tmp_buf[len-i] = UART_RecvByte();
            }while(--i);
            write_buf( pUartbuf,tmp_buf, &len );
            gUartRxCount += len;
            uart_flag = UART_STATUS_RCV_END;
            break;

        default:
            if( pRfBuf && pRfBuf->data_len )
            {
                len = UART_FIFO_SIZE-R8_UART_TFC;
                gRfRxFlag = read_buf( pRfBuf,tmp_buf,&len );
                for( int i=0; i<len; i++)
                {
                    R8_UART_THR = tmp_buf[i];
                }
            }
            break;
    }
}

/*********************************************************************
 * @fn      uart_start_receiving
 *
 * @brief   Serial port starts receiving.
 *
 * @param   none
 *
 * @return  none
 */
int uart_start_receiving(void)
{
    uart_flag = UART_STATUS_START;
    uart_buffer_create(&pUartbuf);
    PFIC_EnableIRQ(UART_IRQn);

    UART_SetTimer( ADV_INTERVAL );
    R8_TMR_CTRL_MOD = RB_TMR_COUNT_EN;
    TMR_ITCfg(ENABLE, TMR_IT_CYC_END); // 开启中断
    PFIC_EnableIRQ(TMR_IRQn);
    return 0;
}

/*********************************************************************
 * @fn      uart_task_init
 *
 * @brief   Serial port initialization and task initialization.
 *
 * @param   none
 *
 * @return  none
 */
void UART_Init(void)
{
    gSysClock = GetSysClock();
    //设置LED_PIN
#if(defined(LED_FUNC)) && (LED_FUNC == TRUE)
    GPIOA_ResetBits(LED_PIN);
    GPIOA_ModeCfg(LED_PIN, GPIO_ModeOut_PP_5mA);
#endif


// 设置DTR、RTS脚与ST——RESET、BOOT脚
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == TRUE))
    GPIOA_SetBits( (DTR_PIN|RTS_PIN) );
    GPIOA_ModeCfg( (DTR_PIN|RTS_PIN), GPIO_ModeOut_PP_5mA);
#else
    /* Device idle state: PA2 is the pulled-up local key/target NRST input;
     * PA7 stays high-Z so it cannot enable the external BOOT NMOS branch.
     * The remote-boot task temporarily drives both pins when requested. */
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_PU );
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_Floating );

#endif




    // 关闭两线调试
    R16_PIN_ALTERNATE &= ~RB_PIN_DEBUG_EN;
    // 默认波特率 115200 PA0-TXD PA1-RXD
    GPIOA_SetBits(TXD_PIN);
    GPIOA_ModeCfg(TXD_PIN, GPIO_ModeOut_PP_5mA);
    GPIOA_ModeCfg(RXD_PIN, GPIO_ModeIN_PU);
    UART_Remap(ENABLE, UART_TX_REMAP_PA0, UART_RX_REMAP_PA1);

    UART_DefInit( );
    UART_SetBuad( 115200 );

    UART_ByteTrigCfg(UART_4BYTE_TRIG);
    UART_INTCfg(ENABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT|RB_IER_THR_EMPTY);
    uart_flag = UART_STATUS_IDLE;
    uart_start_receiving();
}

/* Quiesce the target UART before the boot-stub flash block is erased. */
void UART_Close(void)
{
    TMR_ITCfg(DISABLE, TMR_IT_CYC_END);
    TMR_Disable();
    PFIC_DisableIRQ(TMR_IRQn);

    UART_INTCfg(DISABLE, 0xFF);
    PFIC_DisableIRQ(UART_IRQn);
    R8_UART_MCR &= ~RB_MCR_INT_OE;
    R8_UART_FCR = RB_FCR_FIFO_EN;

    UART_Remap(DISABLE, UART_TX_REMAP_PA0, UART_RX_REMAP_PA1);
    GPIOA_ModeCfg(TXD_PIN | RXD_PIN, GPIO_ModeIN_Floating);
    uart_flag = UART_STATUS_IDLE;
}

