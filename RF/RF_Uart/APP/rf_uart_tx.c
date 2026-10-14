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

volatile uint8_t gTxDataSeq;
volatile uint8_t gRfStatus;
volatile uint8_t gBoundStatus;
volatile uint8_t getDataProbe;
uint32_t  gRfRxFlag;
rfPackage_t *pPkt_t;

/* Runtime diagnostics; these do not alter the on-air packet format. */
volatile uint32_t gRfResendCount;
volatile uint32_t gRfResendFailCount;
volatile uint32_t gRfUartDirectCount;

static void rfProcessRx( rfPackage_t *pPkt );
static void rfProcessTx( void );
static void rfProcessTimeout( void );

static uint8_t rf_packet_valid( const rfPackage_t *pPkt )
{
    if( !pPkt || pPkt->length < PKT_DATA_OFFSET ||
        pPkt->length > DATA_LEN_MAX_RX )
        return 0;
    switch( pPkt->type )
    {
        case PKT_CMD_BOUND_RSP:
            return pPkt->length == PKT_DATA_OFFSET + sizeof(bound_rsp_t);
        case PKT_CMD_RSP_STATUS:
            if( pPkt->length == PKT_DATA_OFFSET ) return 1;
            if( pPkt->length < PKT_DATA_OFFSET + 1 ) return 0;
            {
                const rfRsp_t *rsp = (const rfRsp_t *)(pPkt + 1);
                if( rsp->opcode == OPCODE_BSP )
                    return pPkt->length == PKT_DATA_OFFSET + 1 + sizeof(rsp->buad_t);
                if( rsp->opcode == OPCODE_DATA )
                    return pPkt->length > PKT_DATA_OFFSET + 1;
                if( rsp->opcode == OPCODE_ACK )
                    return pPkt->length == PKT_DATA_OFFSET + 1;
                return 0;
            }
        case PKT_DATA_RSP_ACK:
            if( pPkt->length == PKT_DATA_OFFSET ) return 1;
            if( pPkt->length < PKT_DATA_OFFSET + 2 ) return 0;
            return ((const rfRsp_t *)(pPkt + 1))->opcode == OPCODE_DATA;
        default:
            return 0;
    }
}

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
/* Receive capacity advertised by the Host in ACK/status packets (16-byte
 * units). Zero means retain UART bytes and request status instead of sending. */
static volatile uint8_t gHostCredit;
static volatile uint8_t gSelfBootPending;
static volatile uint32_t gSelfBootEdgeTick;
static volatile uint8_t gDeviceLedPhase;
static volatile uint32_t gDeviceLedUntil;

#define DEVICE_SELFBOOT_DEBOUNCE_MS   25u
#define DEVICE_SELFBOOT_HOLD_MS       3000u

/* PA7 is both the target BOOT connection and a local active-low key.  The
 * interrupt only records the edge; ROM entry is performed from the main loop
 * after debounce so no flash/reset work runs in GPIO interrupt context. */
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
__INTERRUPT __HIGH_CODE
void GPIOA_IRQHandler( void )
{
    if( GPIOA_ReadITFlagBit( BOOT_PIN ) )
    {
        GPIOA_ClearITFlagBit( BOOT_PIN );
        gSelfBootPending = 1;
        gSelfBootEdgeTick = SYS_GetSysTickCnt();
    }
}
#endif

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
        uint32_t irq_state = __risc_v_disable_irq();
        free_len = pRfBuf->buf_len - pRfBuf->data_len;
        __risc_v_enable_irq( irq_state );
    }
    free_len /= RF_CREDIT_UNIT;
    return (free_len > 0xFFu) ? 0xFFu : (uint8_t)free_len;
}

static typeBufSize Device_HostCreditLength( uint8_t credit )
{
    uint32_t len = (uint32_t)credit * RF_CREDIT_UNIT;
    return (len > DATA_LEN_MAX_TX) ? DATA_LEN_MAX_TX : (typeBufSize)len;
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

    /* PA7 is also the local key.  Keep its edge interrupt out of the target
     * BOOT/RESET output sequence, then restore it before returning idle. */
    PFIC_DisableIRQ( GPIO_A_IRQn );
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
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
    GPIOA_ITModeCfg( BOOT_PIN, GPIO_ITMode_FallEdge );
    GPIOA_ClearITFlagBit( BOOT_PIN );
    PFIC_EnableIRQ( GPIO_A_IRQn );
#endif

    /* Preserve the ISP sync byte ordering used by the original implementation. */
    if( send_sync )
    {
        uint8_t sync = 0x7F;
        UART_Send( (char *)&sync, 1 );
    }
}

/* Erase the application reset/startup block, then reset so the chip ROM ISP
 * takes over.  This intentionally destroys the current image's 0x0000 entry
 * and is therefore only called after the PA7 long-hold/release confirmation.
 * Keep this routine in RAM: it must continue executing after erasing address 0. */
__HIGH_CODE __attribute__((noinline))
static void Device_EnterRomBootloader( void )
{
    uint32_t irq_status;

    /* A disabled boot-loader option cannot be entered by software. */
    if( !(R8_GLOB_CFG_INFO & RB_CFG_BOOT_EN) )
    {
        return;
    }

    SYS_DisableAllIrq( &irq_status );
    /* No interrupt may return into Flash after the startup block is erased. */
    __risc_v_disable_irq();

    /* Leave the target-MCU control lines passive while erasing and resetting. */
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_Floating );
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_PU );

    /* This project links _start at 0x0000 (unlike the reference project's
     * reserved jump-stub layout).  Erasing the first 4 KiB invalidates that
     * entry; the full project HEX restores it during subsequent programming. */
    while( FLASH_ROM_ERASE( 0, FLASH_BLOCK_SIZE ) )
    {
        /* Retry transient FlashROM failures, as in the reference firmware. */
    }

    FLASH_ROM_SW_RESET();

    /* Follow the WCH/reference sequence after erasing the reset entry. */
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG1;
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG2;
    SAFEOPERATE;
    R16_INT_LSI_TUNE = 0xFFFF;
    R8_RST_WDOG_CTRL |= RB_SOFTWARE_RESET;
    R8_SAFE_ACCESS_SIG = 0;

    /* The reset above should not return. */
    while( 1 )
    {
    }
}

/* PA7 must be held for 3 seconds, then released to confirm destructive ISP
 * entry.  The IRQ only records the falling edge; debounce, hold timing, and
 * Flash operations all run from the main loop. */
void Device_SelfBootTask( void )
{
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
    uint32_t now;
    uint32_t debounce_ticks;
    uint32_t hold_ticks;

    if( !gSelfBootPending )
    {
        return;
    }

    if( gSelfBootPending == 1 )
    {
        now = SYS_GetSysTickCnt();
        debounce_ticks = (GetSysClock() / 1000u) * DEVICE_SELFBOOT_DEBOUNCE_MS;
        if( (uint32_t)(now - gSelfBootEdgeTick) < debounce_ticks )
        {
            return;
        }

        /* Reject a short/noisy press before starting the long-hold timer. */
        if( GPIOA_ReadPortPin( BOOT_PIN ) )
        {
            gSelfBootPending = 0;
            return;
        }
        gSelfBootPending = 2;
        return;
    }

    now = SYS_GetSysTickCnt();
    hold_ticks = (GetSysClock() / 1000u) * DEVICE_SELFBOOT_HOLD_MS;

    if( gSelfBootPending == 2 )
    {
        if( !GPIOA_ReadPortPin( BOOT_PIN ) )
        {
            if( (uint32_t)(now - gSelfBootEdgeTick) < hold_ticks )
            {
                return;
            }
            /* State 3 is shown as a fast PA3 LED blink; release confirms. */
            gSelfBootPending = 3;
            return;
        }

        /* Release before the hold threshold cancels.  If the main loop did
         * not sample the armed state before release, still require the full
         * interval before accepting the release as confirmation. */
        if( (uint32_t)(now - gSelfBootEdgeTick) < hold_ticks )
        {
            gSelfBootPending = 0;
            return;
        }
    }
    else if( gSelfBootPending == 3 && !GPIOA_ReadPortPin( BOOT_PIN ) )
    {
        return;
    }

    gSelfBootPending = 0;
    Device_EnterRomBootloader();
#endif
}

static void Device_LedTask( void )
{
    static uint8_t last_bound = 0xFF;
    static uint8_t pulse_on = 1;
    static uint32_t last_toggle;
    uint8_t bound = (RF_bound_Flag != 0);
    uint32_t now = SYS_GetSysTickCnt();
    uint8_t active = ((int32_t)(now - gDeviceLedUntil) < 0);

    if( gSelfBootPending == 3 )
    {
        /* Blink quickly to indicate that releasing PA7 will erase Flash. */
        if( ((((uint32_t)(now - gSelfBootEdgeTick)) /
               (GetSysClock() / 5u)) & 1u) != 0u )
        {
            GPIOA_SetBits( LED_PIN );
        }
        else
        {
            GPIOA_ResetBits( LED_PIN );
        }
        GPIOA_ModeCfg( LED_PIN, GPIO_ModeOut_PP_5mA );
        return;
    }

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
    /* A disconnected link must not carry an incomplete transaction or old
     * received bytes into the next pairing. */
    simple_buf_reset( pRfBuf );
    gTxBuf.status = STA_IDLE;
    gTxBuf.resendCount = 0;
    gTxDataSeq = 0;
    getDataProbe = 0;
    gTimeout = 0;
    gRfRxFlag = 0;
    gRfStatus = RF_STATUS_IDLE;
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
    if( !rf_packet_valid( pPkt ) )
    {
        /* RX completion is terminal, even for a rejected packet. Retry the
         * unchanged transaction rather than leave STA_BUSY awaiting an IRQ
         * that may never arrive. Preserve WAITRSP/REWAIT for retry accounting. */
        rfProcessTimeout();
        return;
    }
    if( gTxDataSeq == pPkt->seq )
    {
        if( pPkt->type == PKT_DATA_RSP_ACK )
        {
            /* A zero reserved byte from the original Host firmware means
             * credit was not implemented there; preserve legacy operation. */
            gHostCredit = (pPkt->resv & RF_CREDIT_VALID) ?
                          (pPkt->resv & 0x7Fu) : 0xFFu;
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
                        /* The ACK also carries reverse-direction UART data.
                         * If the advertised credit was stale, do not consume
                         * this ACK: retry the original DATA packet so the
                         * Host resends the same response after space frees. */
                        gRfUartDirectCount++;
                        gRfStatus = RF_STATUS_WAIT;
                        /* This is a local backpressure event, not a
                         * completed transaction.  Give the unchanged DATA
                         * packet the same bounded retry budget as a radio
                         * timeout; otherwise STA_RESEND is observed with a
                         * zero counter and the packet is discarded. */
                        gTxBuf.resendCount = RESEND_COUNT;
                        gTxBuf.status = STA_RESEND;
                        return;
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
            /* Status resv bit 0 remains remote-boot; bit 7 marks credit
             * support and bits 1..6 carry 32-byte Host queue units. */
            gHostCredit = (pPkt->resv & RF_CREDIT_VALID) ?
                          (uint8_t)(((pPkt->resv & RF_STATUS_CREDIT_MASK) >> 1) * 2u) : 0xFFu;
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
                        /* Preserve the packet for retry when reverse data did
                         * not fit; never block or silently discard it here. */
                        gRfUartDirectCount++;
                        gRfStatus = RF_STATUS_WAIT;
                        gTxBuf.resendCount = RESEND_COUNT;
                        gTxBuf.status = STA_RESEND;
                        return;
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
    else
    {
        rfProcessTimeout();
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
    /* First and repeated ACK waits belong to one transaction. Initialize
     * the retry budget only once; REWAIT must not reset it. */
    if( gRfStatus == RF_STATUS_WAITRSP || gRfStatus == RF_STATUS_REWAIT )
    {
        gTxBuf.status = STA_RESEND;
        if( gRfStatus == RF_STATUS_WAITRSP )
        {
            gTxBuf.resendCount = RESEND_COUNT;
        }
        return;
    }

    gTxBuf.status = STA_RESEND;
    if( gBoundStatus )
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
/* Foreground scheduler may execute from Flash. IRQ callbacks and the
 * destructive ISP entry remain RAM-resident. Prevent inlining into main. */
__attribute__((noinline))
void RF_StatusQuery( void )
{
    uint8_t s;

    Device_SelfBootTask();
    Device_BootPulseTask();
    Device_LedTask();
    if( gTxBuf.status == STA_IDLE )
    {
        rfPackage_t *pPkt_t = (rfPackage_t *)gTxBuf.TxBuf;

        gTxBuf.len  = Device_HostCreditLength( gHostCredit );
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
                pPkt_t->resv = Device_RfCredit();
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
            /* ACK loss makes peer acceptance ambiguous. Do not build a new
             * payload at the same sequence and let the Host mistake it for a
             * duplicate. End this failed session and pair again. The failed
             * in-flight transaction is not guaranteed delivered. */
            rf_disconnect();
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
void RF_UartTxInit( void )
{
    rfBoundInfo_t *pInfo;
    PRINT("----------------- rf uart tx mode -----------------\n");
    gBootPulsePending = 0;
    gBootPulseSendSync = 0;
    gSelfBootPending = 0;
    gSelfBootEdgeTick = 0;
    /* BOOT/RESET are high impedance until a validated 0x7F command arrives. */
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_Floating );
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_PU );
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
    GPIOA_ITModeCfg( BOOT_PIN, GPIO_ITMode_FallEdge );
    GPIOA_ClearITFlagBit( BOOT_PIN );
    PFIC_EnableIRQ( GPIO_A_IRQn );
#endif
    GPIOA_ModeCfg( LED_PIN, GPIO_ModeIN_Floating );
    gTxDataSeq = 0;
    gRfRxFlag = 0;
    gHostCredit = 0;
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
