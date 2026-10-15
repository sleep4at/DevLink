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
static uint8_t gBootPulseState;
static uint32_t gBootPulseLastRtc;
static uint32_t gBootPulseElapsed;
/* Receive capacity advertised by the Host in ACK/status packets (16-byte
 * units). Zero means retain UART bytes and request status instead of sending. */
static volatile uint8_t gHostCredit;
static volatile uint8_t gSelfBootPending;
static volatile uint32_t gSelfBootEdgeTick;
static volatile uint32_t gSelfBootArmTick;
static volatile uint8_t gDeviceLedPhase;
static volatile uint32_t gDeviceLedStartRtc;
static volatile uint8_t gDeviceLedActive;

#define DEVICE_SELFBOOT_DEBOUNCE_MS   25u
#define DEVICE_SELFBOOT_HOLD_MS       3000u
#define DEVICE_SELFBOOT_CONFIRM_HOLD_MS 1000u
#define DEVICE_SELFBOOT_CONFIRM_WINDOW_MS 5000u

enum
{
    DEVICE_SELFBOOT_IDLE = 0,
    DEVICE_SELFBOOT_DEBOUNCE,
    DEVICE_SELFBOOT_FIRST_HOLD,
    DEVICE_SELFBOOT_FIRST_RELEASE,
    DEVICE_SELFBOOT_ARMED,
    DEVICE_SELFBOOT_CONFIRM_DEBOUNCE,
    DEVICE_SELFBOOT_CONFIRM_HOLD,
    DEVICE_SELFBOOT_FINAL_RELEASE
};

/* Use the calibrated low-speed RTC counter for all wall-clock intervals.
 * Unlike SysTick, this counter is independent of UART-related SYSCLK changes. */
static uint32_t Device_MsToRtcTicks( uint32_t ms )
{
    return (Freq_LSI / 1000u) * ms;
}

static uint32_t Device_RtcElapsed( uint32_t now, uint32_t then )
{
    if( now >= then )
    {
        return now - then;
    }
    return RTC_MAX_COUNT - then + now;
}

/* PA2 is shared by target NRST and the local active-low key. The interrupt
 * only records the edge; ROM entry is handled from the main loop after
 * debounce, never from GPIO interrupt context. */
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
__INTERRUPT __HIGH_CODE
void GPIOA_IRQHandler( void )
{
    if( GPIOA_ReadITFlagBit( RESET_PIN ) )
    {
        GPIOA_ClearITFlagBit( RESET_PIN );
        if( gSelfBootPending == DEVICE_SELFBOOT_ARMED )
        {
            gSelfBootPending = DEVICE_SELFBOOT_CONFIRM_DEBOUNCE;
            gSelfBootEdgeTick = RTC_GetCycleLSI();
        }
        else if( gSelfBootPending == DEVICE_SELFBOOT_IDLE )
        {
            gSelfBootPending = DEVICE_SELFBOOT_DEBOUNCE;
            gSelfBootEdgeTick = RTC_GetCycleLSI();
        }
    }
}
#endif

void DeviceLedActivity( void )
{
    gDeviceLedPhase ^= 1u;
    gDeviceLedStartRtc = RTC_GetCycleLSI();
    gDeviceLedActive = 1;
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

enum
{
    DEVICE_BOOT_PULSE_IDLE = 0,
    DEVICE_BOOT_PULSE_SETUP,
    DEVICE_BOOT_PULSE_RESET_LOW,
    DEVICE_BOOT_PULSE_BOOT_HOLD
};

/* Run the target BOOT/RESET sequence as a foreground state machine. This
 * keeps RF/UART servicing alive during the 200 ms target startup window. */
void Device_BootPulseTask( void )
{
    uint32_t now = RTC_GetCycleLSI();

    if( gBootPulseState == DEVICE_BOOT_PULSE_IDLE )
    {
        if( !gBootPulsePending || !GPIOA_ReadPortPin( RESET_PIN ) )
        {
            return;
        }

        /* The local key shares PA2 with target NRST. Defer while held so PA2
         * is never driven high against the grounded key. */
        gBootPulsePending = 0;
        PFIC_DisableIRQ( GPIO_A_IRQn );

        /* Assert target BOOT first and allow it to settle before reset. */
        GPIOA_SetBits( BOOT_PIN );
        GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeOut_PP_5mA );
        gBootPulseLastRtc = now;
        gBootPulseElapsed = 0;
        gBootPulseState = DEVICE_BOOT_PULSE_SETUP;
        return;
    }

    /* RTC counter wraps at RTC_MAX_COUNT, not at the SYSCLK-dependent
     * SysTick period. Durations therefore remain stable across clock changes. */
    if( now >= gBootPulseLastRtc )
    {
        gBootPulseElapsed += now - gBootPulseLastRtc;
    }
    else
    {
        gBootPulseElapsed += RTC_MAX_COUNT - gBootPulseLastRtc + now;
    }
    gBootPulseLastRtc = now;

    if( gBootPulseState == DEVICE_BOOT_PULSE_SETUP )
    {
        if( gBootPulseElapsed < Device_MsToRtcTicks(DEVICE_TARGET_BOOT_SETUP_MS) )
        {
            return;
        }
        /* Hold reset low for the target-specific pulse width. */
        GPIOA_ResetBits( RESET_PIN );
        GPIOA_ModeCfg( RESET_PIN, GPIO_ModeOut_PP_5mA );
        gBootPulseElapsed = 0;
        gBootPulseState = DEVICE_BOOT_PULSE_RESET_LOW;
        return;
    }

    if( gBootPulseState == DEVICE_BOOT_PULSE_RESET_LOW )
    {
        if( gBootPulseElapsed < Device_MsToRtcTicks(DEVICE_TARGET_RESET_LOW_MS) )
        {
            return;
        }
        /* Release reset, but keep BOOT asserted through the startup window. */
        GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_PU );
        gBootPulseElapsed = 0;
        gBootPulseState = DEVICE_BOOT_PULSE_BOOT_HOLD;
        return;
    }

    if( gBootPulseElapsed < Device_MsToRtcTicks(DEVICE_TARGET_BOOT_HOLD_MS) )
    {
        return;
    }

    /* Release BOOT without driving it low, so the board's external biasing
     * network selects the normal run state. */
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_Floating );
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_PU );
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
    GPIOA_ITModeCfg( RESET_PIN, GPIO_ITMode_FallEdge );
    GPIOA_ClearITFlagBit( RESET_PIN );
    PFIC_EnableIRQ( GPIO_A_IRQn );
#endif

    gBootPulseState = DEVICE_BOOT_PULSE_IDLE;
    gBootPulseElapsed = 0;

    /* Preserve the original optional STM32 USART ROM ISP sync behavior. */
    if( gBootPulseSendSync )
    {
        uint8_t sync = 0x7F;
        gBootPulseSendSync = 0;
        UART_Send( (char *)&sync, 1 );
    }
}

/* Erase the application reset/startup block, then reset so the chip ROM ISP
 * takes over.  This intentionally destroys the current image's 0x0000 entry
 * and is therefore only called after the PA2 long-hold/release confirmation.
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

    /* Keep the shared NRST/key input released; leave BOOT high-Z so the
     * board's gate pulldown keeps the NMOS branch off during ROM hand-off. */
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_PU );
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_Floating );

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

/* Two-step PA2 confirmation protects the destructive ISP erase from a single
 * probe glitch or accidental long press. The first 3 s hold opens a 5 s window
 * marked by double flashes; within it, hold for 1 s and release to erase. */
void Device_SelfBootTask( void )
{
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
    uint32_t now;
    uint32_t debounce_ticks;
    uint32_t hold_ticks;
    uint32_t confirm_ticks;
    uint32_t window_ticks;

    if( !gSelfBootPending )
    {
        return;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_DEBOUNCE )
    {
        now = RTC_GetCycleLSI();
        debounce_ticks = Device_MsToRtcTicks(DEVICE_SELFBOOT_DEBOUNCE_MS);
        if( Device_RtcElapsed(now, gSelfBootEdgeTick) < debounce_ticks )
        {
            return;
        }

        /* Reject a short/noisy press before starting the long-hold timer. */
        if( GPIOA_ReadPortPin( RESET_PIN ) )
        {
            gSelfBootPending = DEVICE_SELFBOOT_IDLE;
            return;
        }
        gSelfBootPending = DEVICE_SELFBOOT_FIRST_HOLD;
        return;
    }

    now = RTC_GetCycleLSI();
    hold_ticks = Device_MsToRtcTicks(DEVICE_SELFBOOT_HOLD_MS);
    confirm_ticks = Device_MsToRtcTicks(DEVICE_SELFBOOT_CONFIRM_HOLD_MS);
    window_ticks = Device_MsToRtcTicks(DEVICE_SELFBOOT_CONFIRM_WINDOW_MS);

    if( gSelfBootPending == DEVICE_SELFBOOT_FIRST_HOLD )
    {
        if( !GPIOA_ReadPortPin( RESET_PIN ) )
        {
            if( Device_RtcElapsed(now, gSelfBootEdgeTick) < hold_ticks )
            {
                return;
            }
            /* The first hold opens the confirmation window. Double flash is
             * shown immediately; releasing this press does not erase Flash. */
            gSelfBootPending = DEVICE_SELFBOOT_FIRST_RELEASE;
            gSelfBootArmTick = now;
            return;
        }

        if( Device_RtcElapsed(now, gSelfBootEdgeTick) < hold_ticks )
        {
            gSelfBootPending = DEVICE_SELFBOOT_IDLE;
            return;
        }
        gSelfBootPending = DEVICE_SELFBOOT_ARMED;
        gSelfBootArmTick = now;
        return;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_FIRST_RELEASE )
    {
        if( Device_RtcElapsed(now, gSelfBootArmTick) >= window_ticks )
        {
            gSelfBootPending = DEVICE_SELFBOOT_IDLE;
            return;
        }
        if( !GPIOA_ReadPortPin( RESET_PIN ) )
        {
            return;
        }
        gSelfBootPending = DEVICE_SELFBOOT_ARMED;
        return;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_ARMED )
    {
        if( Device_RtcElapsed(now, gSelfBootArmTick) >= window_ticks )
        {
            gSelfBootPending = DEVICE_SELFBOOT_IDLE;
        }
        return;
    }

    /* The entire second confirmation must complete inside the five-second
     * window; timeout always cancels and leaves the running image untouched. */
    if( (gSelfBootPending == DEVICE_SELFBOOT_CONFIRM_DEBOUNCE ||
         gSelfBootPending == DEVICE_SELFBOOT_CONFIRM_HOLD ||
         gSelfBootPending == DEVICE_SELFBOOT_FINAL_RELEASE) &&
        Device_RtcElapsed(now, gSelfBootArmTick) >= window_ticks )
    {
        gSelfBootPending = DEVICE_SELFBOOT_IDLE;
        return;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_CONFIRM_DEBOUNCE )
    {
        debounce_ticks = Device_MsToRtcTicks(DEVICE_SELFBOOT_DEBOUNCE_MS);
        if( Device_RtcElapsed(now, gSelfBootEdgeTick) < debounce_ticks )
        {
            return;
        }
        if( GPIOA_ReadPortPin( RESET_PIN ) )
        {
            gSelfBootPending = DEVICE_SELFBOOT_IDLE;
            return;
        }
        gSelfBootPending = DEVICE_SELFBOOT_CONFIRM_HOLD;
        return;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_CONFIRM_HOLD )
    {
        if( !GPIOA_ReadPortPin( RESET_PIN ) )
        {
            if( Device_RtcElapsed(now, gSelfBootEdgeTick) >= confirm_ticks )
            {
                /* Extra-fast blink means the second hold is complete; release
                 * is the final explicit erase confirmation. */
                gSelfBootPending = DEVICE_SELFBOOT_FINAL_RELEASE;
            }
            return;
        }
        if( Device_RtcElapsed(now, gSelfBootEdgeTick) >= confirm_ticks )
        {
            /* The main loop may observe release before it sampled the final
             * held state; accept only if the full second hold elapsed. */
            gSelfBootPending = DEVICE_SELFBOOT_FINAL_RELEASE;
        }
        else
        {
            gSelfBootPending = DEVICE_SELFBOOT_IDLE;
        }
        return;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_FINAL_RELEASE )
    {
        if( !GPIOA_ReadPortPin( RESET_PIN ) )
        {
            return;
        }
        gSelfBootPending = DEVICE_SELFBOOT_IDLE;
        Device_EnterRomBootloader();
    }
#endif
}

static void Device_LedTask( void )
{
    static uint8_t last_bound = 0xFF;
    static uint8_t pulse_on = 1;
    static uint32_t last_toggle;
    uint8_t bound = (RF_bound_Flag != 0);
    uint32_t now = RTC_GetCycleLSI();
    uint8_t active = gDeviceLedActive &&
                     (Device_RtcElapsed(now, gDeviceLedStartRtc) <
                      Device_MsToRtcTicks(50u));

    if( gDeviceLedActive && !active )
    {
        gDeviceLedActive = 0;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_FIRST_RELEASE ||
        gSelfBootPending == DEVICE_SELFBOOT_ARMED ||
        gSelfBootPending == DEVICE_SELFBOOT_CONFIRM_DEBOUNCE ||
        gSelfBootPending == DEVICE_SELFBOOT_CONFIRM_HOLD )
    {
        /* During the 5-second confirmation window, alternate the two role
         * LEDs: PA3 low selects Device and PA3 high selects Host. */
        uint32_t phase = Device_RtcElapsed(now, gSelfBootArmTick) %
                         Device_MsToRtcTicks(400u);
        if( phase < Device_MsToRtcTicks(200u) )
        {
            GPIOA_ResetBits( LED_PIN );
        }
        else
        {
            GPIOA_SetBits( LED_PIN );
        }
        GPIOA_ModeCfg( LED_PIN, GPIO_ModeOut_PP_5mA );
        return;
    }

    if( gSelfBootPending == DEVICE_SELFBOOT_FINAL_RELEASE )
    {
        /* Once the second 1-second hold is accepted, flash only the Device
         * LED until release. High-Z is the off state; never drive Host high. */
        if( ((Device_RtcElapsed(now, gSelfBootEdgeTick) /
               Device_MsToRtcTicks(100u)) & 1u) != 0u )
        {
            GPIOA_ModeCfg( LED_PIN, GPIO_ModeIN_Floating );
        }
        else
        {
            GPIOA_ResetBits( LED_PIN );
            GPIOA_ModeCfg( LED_PIN, GPIO_ModeOut_PP_5mA );
        }
        return;
    }

    if( bound != last_bound )
    {
        last_bound = bound;
        pulse_on = 1;
        last_toggle = now;
    }
    else if( !bound && Device_RtcElapsed(now, last_toggle) >=
                        Device_MsToRtcTicks(500u) )
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
    gBootPulseState = DEVICE_BOOT_PULSE_IDLE;
    gBootPulseLastRtc = 0;
    gBootPulseElapsed = 0;
    gSelfBootPending = 0;
    gSelfBootEdgeTick = 0;
    gSelfBootArmTick = 0;
    gDeviceLedStartRtc = 0;
    gDeviceLedActive = 0;
    /* PA2 is the local active-low key (and target NRST); PA7 must not bias
     * the target BOOT network while idle. */
    GPIOA_ModeCfg( RESET_PIN, GPIO_ModeIN_PU );
    GPIOA_ModeCfg( BOOT_PIN, GPIO_ModeIN_Floating );
#if((defined(DTR_RTS_FUNC)) && (DTR_RTS_FUNC == FALSE))
    GPIOA_ITModeCfg( RESET_PIN, GPIO_ITMode_FallEdge );
    GPIOA_ClearITFlagBit( RESET_PIN );
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
