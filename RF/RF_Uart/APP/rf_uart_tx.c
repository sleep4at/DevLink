/********************************** (C) COPYRIGHT *******************************
 * File Name          : rf_basic.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2024/08/15
 * Description        : ÎÞÏß´®¿Ú-·¢ËÍ¶Ë£¬×¢£ºRF·¢ËÍÊ§°Ü RESEND_COUNT ´ÎÊý»á¶ªÆúÊý¾Ý°ü
 *
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

/******************************************************************************/
/* Í·ÎÄ¼þ°üº¬ */
#include "rf.h"
#include "rf_uart_tx.h"
#include "uart.h"

/*********************************************************************
 * GLOBAL TYPEDEFS
 */

rfTxBuf_t gTxBuf;
uint32_t gRfRxCount;
uint16_t gInterval;
uint16_t gTimeoutMax;
uint16_t gTimeout;
uint16_t gServerData;

uint8_t gTxDataSeq;
uint8_t gRfStatus;
uint8_t gBoundStatus;
uint8_t getDataProbe;
uint32_t  gRfRxFlag;
rfPackage_t *pPkt_t;

/* Runtime diagnostics; these do not alter the on-air packet format. */
volatile uint32_t gRfResendCount;
volatile uint32_t gRfResendFailCount;
volatile uint32_t gRfUartDirectCount;

static void rfProcessRx( rfPackage_t *pPkt );
static void rfProcessTx( void );
static void rfProcessTimeout( void );

// tf status callbacks
rfStatusCBs_t rfCBs =
{
    rfProcessRx,
    rfProcessTx,
    rfProcessTimeout,
    rfProcessTimeout,
};

#define  RF_BUF_LEN    512
static uint8_t rf_buf[RF_BUF_LEN];
static struct simple_buf rf_buffer;
struct simple_buf *pRfBuf = NULL;

uint8_t volatile RF_bound_Flag;
static volatile uint8_t gBootPulsePending;
static volatile uint8_t gBootPulseSendSync;
static volatile uint8_t gDeviceLedPhase;
static volatile uint32_t gDeviceLedUntil;

void DeviceLedActivity( void )
{
    gDeviceLedPhase ^= 1u;
    gDeviceLedUntil = SYS_GetSysTickCnt() + GetSysClock() / 20u;
}

/* Advertise UART-output queue space to the Host in 16-byte units. */
static uint8_t Device_RfCredit( void )
{
    uint32_t free_len = 0;

    if( pRfBuf )
    {
        PFIC_DisableAllIRQ();
        free_len = pRfBuf->buf_len - pRfBuf->data_len;
        PFIC_EnableAllIRQ();
    }
    free_len /= RF_CREDIT_UNIT;
    return (free_len > 0xFFu) ? 0xFFu : (uint8_t)free_len;
}

/* The target BOOT/RESET sequence is deliberately deferred out of the RF
 * callback.  The callback runs from radio interrupt context and must not
 * block for tens of milliseconds. */
void Device_BootPulseTask( void )
{
    if( !gBootPulsePending )
    {
        return;
    }
    uint8_t send_sync = gBootPulseSendSync;
    gBootPulsePending = 0;
    gBootPulseSendSync = 0;

    GPIOA_SetBits( BOOT_PIN | RESET_PIN );
    GPIOA_ModeCfg( BOOT_PIN | RESET_PIN, GPIO_ModeOut_PP_5mA );
    mDelaymS(1);
    GPIOA_ResetBits( RESET_PIN );
    mDelaymS(1);
    GPIOA_SetBits( RESET_PIN );
    mDelaymS(1);
    GPIOA_ResetBits( BOOT_PIN );
    mDelaymS(50);

    /* Idle Device mode must not drive the target reset/boot lines. */
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_Floating );
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_PU );

    /* Preserve the ISP sync byte ordering used by the original implementation. */
    if( send_sync )
    {
        uint8_t sync = 0x7F;
        UART_Send( (char *)&sync, 1 );
    }
}

static void Device_LedTask( void )
{
    static uint8_t last_bound = 0xFF;
    static uint8_t pulse_on = 1;
    static uint32_t last_toggle;
    uint8_t bound = (RF_bound_Flag != 0);
    uint32_t now = SYS_GetSysTickCnt();
    uint8_t active = ((int32_t)(now - gDeviceLedUntil) < 0);

    if( bound != last_bound )
    {
        last_bound = bound;
        pulse_on = 1;
        last_toggle = now;
    }
    else if( !bound && (uint32_t)(now - last_toggle) >= GetSysClock() / 2u )
    {
        pulse_on ^= 1u;
        last_toggle = now;
    }

    if( !bound && !pulse_on )
    {
        GPIOA_ModeCfg( LED_PIN, GPIO_ModeIN_Floating );
    }
    else
    {
        /* Device role is active-low; wireless activity briefly selects the
         * other LED, then the role indicator returns after 50 ms. */
        if( bound && active && gDeviceLedPhase ) GPIOA_SetBits( LED_PIN );
        else GPIOA_ResetBits( LED_PIN );
        GPIOA_ModeCfg( LED_PIN, GPIO_ModeOut_PP_5mA );
    }
}

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
static void rf_buffer_create(struct simple_buf **buf)
{
    *buf = simple_buf_create(&rf_buffer, rf_buf, sizeof(rf_buf) );
}

/*******************************************************************************
 * @fn      rf_disconnect
 *
 * @brief   ¶Ï¿ªÁ¬½Ó
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rf_disconnect( void )
{
    gBoundStatus = BOUND_STATUS_IDLE;
    UART_SetTimer( ADV_INTERVAL );
    rf_tx_set_sync_word( AA );
    rf_tx_set_frequency( DEF_FREQUENCY );

    rf_rx_set_sync_word( AA );
    rf_rx_set_frequency( DEF_FREQUENCY );

    RF_bound_Flag = 0;
    PRINT("disconnect.\n" );
}

/*******************************************************************************
 * @fn      rf_bound
 *
 * @brief   ¶Ï¿ªÁ¬½Ó
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rf_bound( bound_rsp_t *rsp )
{
    rfBoundInfo_t info;

    gTimeout = 0;
    gBoundStatus = BOUND_STATUS_WAIT;
    gInterval = rsp->interval;
    gTimeoutMax = (rsp->timeout*10/rsp->interval);
    gServerData = rsp->severData;
    rf_tx_set_sync_word( rsp->accessaddr );
    rf_tx_set_frequency( rsp->channel );
    rf_tx_set_phy_type( rsp->phy );
    rf_rx_set_sync_word( rsp->accessaddr );
    rf_rx_set_frequency( rsp->channel );
    rf_rx_set_phy_type( rsp->phy );
    info.head = 0x55aa;
    info.serverData = gServerData;
    FLASH_ROM_ERASE( BOUND_INFO_FLASH_ADDR, 4096 );
    FLASH_ROM_WRITE( BOUND_INFO_FLASH_ADDR,&info,4 );

    RF_bound_Flag = 1;
    PRINT("bound success.%x %x\n",rsp->accessaddr,rsp->channel );
}

/*******************************************************************************
 * @fn      RF_ProcessRx
 *
 * @brief
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rfProcessRx( rfPackage_t *pPkt )
{
    if( gTxDataSeq == pPkt->seq )
    {
        if( pPkt->type == PKT_DATA_RSP_ACK )
        {
            // Êý¾Ý·¢ËÍ³É¹¦
            if( pPkt->length > PKT_DATA_OFFSET+1 )
            {
                typeBufSize len;

                pPkt_t = pPkt;
                {
                    rfRsp_t *pRsp_t = (rfRsp_t *)(pPkt_t+1);
                    len = pPkt_t->length-PKT_DATA_OFFSET-1;
                    gRfRxFlag = write_buf( pRfBuf, pRsp_t->other.rspData, &len );
                    if( !len )
                    {
                        /* Safety fallback for a stale credit value.  It
                         * preserves bytes, but can briefly block here while
                         * UART FIFO space becomes available. */
                        gRfUartDirectCount++;
                        UART_Send(pRsp_t->other.rspData,pPkt_t->length-PKT_DATA_OFFSET-1);
                    }
                    if( !R8_UART_TFC )
                    {
                        PFIC_SetPendingIRQ( UART_IRQn );
                    }
                }
            }
            getDataProbe = 6;
        }
        else if( pPkt->type == PKT_CMD_BOUND_RSP )
        {
            rf_bound( (bound_rsp_t *)(pPkt+1) );
        }
        else if( pPkt->type == PKT_CMD_RSP_STATUS )
        {
            rfRsp_t *pRsp_t = (rfRsp_t *)(pPkt+1);
            if( pPkt->resv & RF_CTRL_ENTER_BOOT )
            {
                gBootPulsePending = 1;
            }

            if( gBoundStatus == BOUND_STATUS_WAIT )
            {
                gBoundStatus = BOUND_STATUS_EST;
                UART_SetTimer( gInterval );
            }

            if(  pPkt->length == PKT_DATA_OFFSET )
            {

            }
            // ×´Ì¬Ó¦´ðÎª¶Ô¶ËÉè±¸²¨ÌØÂÊ
            else if( pRsp_t->opcode == OPCODE_BSP )
            {
                if((pRsp_t->buad_t.BaudRate > 400000) && (pRsp_t->buad_t.BaudRate < 1000000))
                {
                    SetSysClock(CLK_SOURCE_HSE_PLL_100MHz);
                }
                else
                {
                    SetSysClock(CLK_SOURCE_HSE_PLL_24MHz);
                }
                mDelaymS(10);

                UART_SetBuad( pRsp_t->buad_t.BaudRate );
                // Í£Ö¹Î»
                if( pRsp_t->buad_t.StopBits )
                {
                    R8_UART_LCR |= RB_LCR_STOP_BIT ; // 2¸öÍ£Ö¹Î»
                }
                else
                {
                    R8_UART_LCR &= ~RB_LCR_STOP_BIT;// 1¸öÍ£Ö¹Î»
                }

                // ÆæÅ¼Ð£Ñé
                if( pRsp_t->buad_t.ParityType )
                {
                    R8_UART_LCR &= ~RB_LCR_PAR_MOD;
                    R8_UART_LCR |= ((pRsp_t->buad_t.ParityType-1)&3)<<4;
                    R8_UART_LCR |= RB_LCR_PAR_EN;
                }
                else
                {
                    R8_UART_LCR &= ~RB_LCR_PAR_EN;
                }

                // Êý¾ÝÎ»
                R8_UART_LCR &= ~RB_LCR_WORD_SZ;
                R8_UART_LCR |= (pRsp_t->buad_t.DataBits-5);
                if( pRsp_t->buad_t.DataBits )
                {

                }

#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == TRUE))
                // DTR µçÆ½×´Ì¬
                if( pRsp_t->buad_t.ioStaus&0x20 )
                {
                    GPIOA_SetBits( DTR_PIN );
                }
                else
                {
                    GPIOA_ResetBits( DTR_PIN );
                }
                // RTS µçÆ½×´Ì¬
                if( pRsp_t->buad_t.ioStaus&0x40 )
                {
                    GPIOA_SetBits( RTS_PIN );
                }
                else
                {
                    GPIOA_ResetBits( RTS_PIN );
                }
#endif
            }
            else if( pRsp_t->opcode == OPCODE_DATA )
            {
                typeBufSize len;
                uint8_t is_boot_sync;

                pPkt_t = pPkt;
                getDataProbe = 6;
                is_boot_sync = (pPkt_t->length-PKT_DATA_OFFSET-1) == 1 &&
                               pRsp_t->other.rspData[0] == 0x7f;


#if(DTR_RTS_FUNC == FALSE)
                //RESETÓëBOOTÒý½Å¿ØÖÆÐ­Òé½âÎö ½öÔÚÁ¬½ÓÏÂÔØÊ±´¥·¢
                if(is_boot_sync)
                {
                    //BOOT½ÅÀ­¸ß¡¢RESETµÍµçÆ½¸´Î»½øBOOT
                    /* Execute the pulse from the main loop, not the RF IRQ. */
                    gBootPulsePending = 1;
                    gBootPulseSendSync = 1;
                }
#endif
                if( !is_boot_sync )
                {
                    rfRsp_t *pRsp_t = (rfRsp_t *)(pPkt_t+1);
                    len = pPkt_t->length-PKT_DATA_OFFSET-1;
                    gRfRxFlag = write_buf( pRfBuf, pRsp_t->other.rspData, &len );
                    if( !len )
                    {
                        gRfUartDirectCount++;
                        UART_Send(pRsp_t->other.rspData,pPkt_t->length-PKT_DATA_OFFSET-1);
                    }
                    if( !R8_UART_TFC )
                    {
                        PFIC_SetPendingIRQ( UART_IRQn );
                    }
                }
            if( !is_boot_sync && len ) DeviceLedActivity();

            }
        }
        else
        {

        }
        gRfStatus = RF_STATUS_IDLE;
        gTxBuf.status = STA_IDLE;
        gTxDataSeq ++;
        gTimeout = 0;
    }
}

/*******************************************************************************
 * @fn      RF_ProcessTx
 *
 * @brief
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rfProcessTx( void )
{
    if( gRfStatus == RF_STATUS_RETX )
    {
        gRfStatus = RF_STATUS_REWAIT;
    }
    else
    {
        gRfStatus = RF_STATUS_WAITRSP;
    }
    rf_rx_start( 150 );
}

/*******************************************************************************
 * @fn      RF_ProcessTimeout
 *
 * @brief
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static  void rfProcessTimeout( void )
{
    gTxBuf.status = STA_RESEND;
    if( gRfStatus == RF_STATUS_WAITRSP )
    {
        gTxBuf.resendCount = RESEND_COUNT;
    }
    else
    {
        if( gBoundStatus == BOUND_STATUS_WAIT )
        {
            if( ++ gTimeout > BOUND_EST_COUNT )
            {
                rf_disconnect( );
            }
            gTxBuf.status = STA_IDLE;
        }
        else if( gBoundStatus == BOUND_STATUS_EST )
        {
            if( ++ gTimeout > gTimeoutMax )
            {
                rf_disconnect( );
                gTxBuf.status = STA_IDLE;
            }
        }
    }
}


/*******************************************************************************
 * @fn      RF_StatusQuery
 *
 * @brief   ×´Ì¬´¦Àí
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
void RF_StatusQuery( void )
{
    uint8_t s;

    Device_BootPulseTask();
    Device_LedTask();
    if( gTxBuf.status == STA_IDLE )
    {
        rfPackage_t *pPkt_t = (rfPackage_t *)gTxBuf.TxBuf;

        gTxBuf.len  = DATA_LEN_MAX_TX;
        s = UART_RxQuery( (void *)(pPkt_t+1), &gTxBuf.len );
        // ·¢ËÍÊý¾Ý
        if( s == 0 )
        {
            DeviceLedActivity();

            gRfStatus = RF_STATUS_TX;
            pPkt_t->type = PKT_DATA_FLAG;
            pPkt_t->length = gTxBuf.len + PKT_DATA_OFFSET;
            gTxBuf.status = STA_BUSY;
            pPkt_t->seq = gTxDataSeq;
            /* Credit for the opposite direction is carried with every
             * Device data packet, so Host can safely put USB data in its ACK. */
            pPkt_t->resv = Device_RfCredit();
            rf_tx_start( gTxBuf.TxBuf, 60 );
        }
        else if( s == 0x80 )
        {
            if( gBoundStatus )
            {
                // »ñÈ¡×´Ì¬
                gRfStatus = RF_STATUS_GETS;
                pPkt_t->type = PKT_CMD_GET_STATUS;
                pPkt_t->length = PKT_DATA_OFFSET;
            }
            else
            {
                // ÇëÇó°ó¶¨
                bound_req_t *pReq_t = (bound_req_t *)&gTxBuf.TxBuf[PKT_HEAD_LEN];

                gRfStatus = RF_STATUS_REQ;
                pPkt_t->type = PKT_CMD_BOUND_REQ;
                pPkt_t->length = PKT_DATA_OFFSET + sizeof(bound_req_t);
                pReq_t->interval = ADV_INTERVAL;
                pReq_t->severData = gServerData;
                gTxDataSeq = 0;
            }
            gTxBuf.status = STA_BUSY;
            pPkt_t->seq = gTxDataSeq;
            pPkt_t->resv = gBoundStatus ? Device_RfCredit() : 0;
            rf_tx_start( gTxBuf.TxBuf, 60 );
        }
        else
        {
            if( getDataProbe )
            {
                getDataProbe--;
                gRfStatus = RF_STATUS_GETS;
                pPkt_t->type = PKT_CMD_GET_STATUS;
                pPkt_t->length = PKT_DATA_OFFSET;
                gTxBuf.status = STA_BUSY;
                pPkt_t->seq = gTxDataSeq;
                pPkt_t->resv = 0;
                rf_tx_start( gTxBuf.TxBuf, 60 );
            }
        }
    }
    else if( gTxBuf.status == STA_RESEND )
    {
        if( gTxBuf.resendCount )
        {
            gRfResendCount++;
            if( gTxBuf.resendCount < RESEND_COUNT/2 )
            {
                PRINT("*%d %d\n",gTxBuf.resendCount,gTxDataSeq);
            }
            gRfStatus = RF_STATUS_RETX;
            if( gTxBuf.resendCount != 0xFF ) gTxBuf.resendCount--;
            gTxBuf.status = STA_BUSY;
            rf_tx_start( gTxBuf.TxBuf, 60 );
        }
        else
        {
            // ·¢ËÍÊ§°Ü¶ªÆú
            gRfResendFailCount++;
            PRINT(" resend fail.%d\n",gTxBuf.TxBuf[1]);
            gTxBuf.status = STA_IDLE;
        }
    }
}

/*******************************************************************************
 * @fn      RF_UartTxInit
 *
 * @brief   RF uart·¢ËÍÓ¦ÓÃ³õÊ¼»¯
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
void RF_UartTxInit( void )
{
    rfBoundInfo_t *pInfo;
    PRINT("----------------- rf uart tx mode -----------------\n");
    gBootPulsePending = 0;
    gBootPulseSendSync = 0;
    /* BOOT/RESET are high impedance until a validated 0x7F command arrives. */
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_Floating );
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_PU );
    GPIOA_ModeCfg( LED_PIN, GPIO_ModeIN_Floating );
    gTxDataSeq = 0;
    gRfRxFlag = 0;
    gRfResendCount = 0;
    gRfResendFailCount = 0;
    gRfUartDirectCount = 0;
    gBoundStatus = BOUND_STATUS_IDLE;
    gTxBuf.status = 0;
    rf_buffer_create(&pRfBuf);

    pInfo = (rfBoundInfo_t *)(BOUND_INFO_FLASH_ADDR);
    if( pInfo->head == BOUND_INFO_HEAD )
    {
        gServerData = pInfo->serverData;
    }
    else {
        gServerData = 0;
    }
    PRINT("gServerData = %x \n",gServerData);
    RFRole_RegisterStatusCbs( &rfCBs );
}

/******************************** endfile @rf ******************************/
