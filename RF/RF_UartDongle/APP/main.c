/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH
 * Version            : V1.1
 * Date               : 2025/06/27
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
#include "rf.h"
#include "rf_uart_rx.h"
#include "usb_uart.h"

/* Free-running core-clock counter for non-blocking LED timing. */
static void AppTimebaseInit( void )
{
    SysTick->CTLR = 0;
    SysTick->CNTL = 0;
    SysTick->CMP = 0xFFFFFFFFu;
    SysTick->SR = SysTick_SR_CNTIF;
    SysTick->CTLR = SysTick_CTLR_STCLK | SysTick_CTLR_STE;
}

extern volatile uint8_t RF_bound_Flag;

/* Activity changes the selected LED for a short window. The GPIO is updated
 * in the main loop, never directly from USB or RF interrupt context. */
static volatile uint8_t s_host_led_phase;
static volatile uint32_t s_host_led_until;

void HostLedActivity( void )
{
    /* Alternate the selected LED for every accepted transaction. */
    s_host_led_phase ^= 1u;
    s_host_led_until = SYS_GetSysTickCnt() + GetSysClock() / 20u;
}

/* PA7 is the remote-download request key; PA2 enters this CH570's ROM
 * Bootloader. Both keys are active-low and handled from the main loop. */
static volatile uint8_t s_boot_request;
static volatile uint8_t s_self_boot_request;
static volatile uint32_t s_self_boot_edge_tick;

#define HOST_SELFBOOT_DEBOUNCE_MS   25u
#define HOST_SELFBOOT_HOLD_MS       3000u

__INTERRUPT __HIGH_CODE
void GPIOA_IRQHandler( void )
{
    if( GPIOA_ReadITFlagBit( GPIO_Pin_7 ) )
    {
        GPIOA_ClearITFlagBit( GPIO_Pin_7 );
        s_boot_request = 1;
    }
    if( GPIOA_ReadITFlagBit( GPIO_Pin_2 ) )
    {
        GPIOA_ClearITFlagBit( GPIO_Pin_2 );
        /* Ignore bounce/retrigger edges while a press is already being
         * debounced or is waiting for the release confirmation. */
        if( s_self_boot_request == 0 )
        {
            s_self_boot_request = 1;
            s_self_boot_edge_tick = SYS_GetSysTickCnt();
        }
    }
}

__HIGH_CODE
uint8_t Host_BootRequestTake( void )
{
    uint8_t pending = s_boot_request;
    s_boot_request = 0;
    return pending;
}

static void HostIo_Init( void )
{
    s_boot_request = 0;
    s_self_boot_request = 0;
    s_self_boot_edge_tick = 0;
    GPIOA_ModeCfg( GPIO_Pin_7, GPIO_ModeIN_PU );
    GPIOA_ITModeCfg( GPIO_Pin_7, GPIO_ITMode_FallEdge );
    GPIOA_ModeCfg( GPIO_Pin_2, GPIO_ModeIN_PU );
    GPIOA_ITModeCfg( GPIO_Pin_2, GPIO_ITMode_FallEdge );
    GPIOA_ClearITFlagBit( GPIO_Pin_2 | GPIO_Pin_7 );
    GPIOA_ModeCfg( LED_PIN, GPIO_ModeIN_Floating );
    PFIC_EnableIRQ( GPIO_A_IRQn );
}

/* Erase the small reset stub at Flash address 0, then reset.  With the reset
 * stub gone, CH570 ROM ISP takes ownership for its normal timeout window.
 * The routine is copied to RAM so it can continue executing after erasing
 * the Flash block that contains the application's reset entry. */
__HIGH_CODE __attribute__((noinline))
static void Host_EnterRomBootloader( void )
{
    uint32_t irq_status;

    if( !(R8_GLOB_CFG_INFO & RB_CFG_BOOT_EN) )
    {
        return;
    }

    SYS_DisableAllIrq( &irq_status );
    __risc_v_disable_irq();

    /* Release the key before erasing and leave it passive for ROM startup. */
    GPIOA_ModeCfg( GPIO_Pin_2, GPIO_ModeIN_PU );

    /* Erase exactly one 4-KiB Flash block.  This is the block containing
     * boot_stub.S; the application is linked at 0x2000 and is not erased. */
    while( FLASH_ROM_ERASE( 0, FLASH_BLOCK_SIZE ) )
    {
        /* Retry transient FlashROM failures, as in the reference sequence. */
    }

    FLASH_ROM_SW_RESET();

    /* WCH reference ROM-ISP hand-off sequence. */
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG1;
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG2;
    SAFEOPERATE;
    R16_INT_LSI_TUNE = 0xFFFF;
    R8_RST_WDOG_CTRL |= RB_SOFTWARE_RESET;
    R8_SAFE_ACCESS_SIG = 0;

    while( 1 )
    {
    }
}

/* PA2 must be held for 3 seconds, then released to confirm ISP entry. The
 * GPIO IRQ only timestamps the press; timing and hand-off run in main. */
static void Host_SelfBootTask( void )
{
    uint32_t now;
    uint32_t debounce_ticks;
    uint32_t hold_ticks;

    if( !s_self_boot_request )
    {
        return;
    }

    if( s_self_boot_request == 1 )
    {
        now = SYS_GetSysTickCnt();
        debounce_ticks = (GetSysClock() / 1000u) * HOST_SELFBOOT_DEBOUNCE_MS;
        if( (uint32_t)(now - s_self_boot_edge_tick) < debounce_ticks )
        {
            return;
        }
        if( GPIOA_ReadPortPin( GPIO_Pin_2 ) )
        {
            s_self_boot_request = 0;
            return;
        }
        s_self_boot_request = 2;
        return;
    }

    now = SYS_GetSysTickCnt();
    hold_ticks = (GetSysClock() / 1000u) * HOST_SELFBOOT_HOLD_MS;

    if( s_self_boot_request == 2 )
    {
        if( !GPIOA_ReadPortPin( GPIO_Pin_2 ) )
        {
            if( (uint32_t)(now - s_self_boot_edge_tick) < hold_ticks )
            {
                return;
            }
            /* State 3: fast LED blink asks the user to release to confirm. */
            s_self_boot_request = 3;
            return;
        }

        /* Short press cancels. */
        if( (uint32_t)(now - s_self_boot_edge_tick) < hold_ticks )
        {
            s_self_boot_request = 0;
            return;
        }
    }
    else if( s_self_boot_request == 3 && !GPIOA_ReadPortPin( GPIO_Pin_2 ) )
    {
        return;
    }

    s_self_boot_request = 0;

    /* Detach the application USB device before handing control to ROM ISP. */
    USB_DeviceDisable();
    mDelaymS( 20 );
    Host_EnterRomBootloader();
}

static void HostLedTask( void )
{
    static uint8_t last_bound = 0xFF;
    static uint8_t pulse_on = 1;
    static uint32_t last_toggle;
    uint8_t bound = (RF_bound_Flag != 0);
    uint32_t now = SYS_GetSysTickCnt();
    uint8_t active = ((int32_t)(now - s_host_led_until) < 0);

    if( s_self_boot_request == 3 )
    {
        if( ((((uint32_t)(now - s_self_boot_edge_tick)) /
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
        /* Host role is active-high; traffic briefly selects the other LED. */
        if( bound && active && s_host_led_phase ) GPIOA_ResetBits( LED_PIN );
        else GPIOA_SetBits( LED_PIN );
        GPIOA_ModeCfg( LED_PIN, GPIO_ModeOut_PP_5mA );
    }
    if( !active ) s_host_led_phase = 0;
}



/*********************************************************************
 * GLOBAL TYPEDEFS
 */
__attribute__((used))
void process_main( void )
{
    while(1)
    {
        Host_SelfBootTask();
        HostLedTask();
        RF_RxService();
        USB_StatusQuery();
    }
}

/*********************************************************************
 * @fn      main
 *
 * @brief   主函数
 *
 * @return  none
 */
int main(void)
{
    HSECFG_Capacitance( HSECap_18p );
    SetSysClock( CLK_SOURCE_HSE_PLL_100MHz );
    AppTimebaseInit();
    HostIo_Init();
#ifdef DEBUG
    GPIOA_SetBits( bTXD_4 );
    GPIOA_ModeCfg( bTXD_4, GPIO_ModeOut_PP_5mA ); // TXD-配置推挽输出，注意先让IO口输出高电平
    UART_Remap( ENABLE, UART_TX_REMAP_PA7, UART_RX_REMAP_PA2 );
    UART_DefInit( );
#endif

    //设置LED_PIN
#if(defined(LED_FUNC)) && (LED_FUNC == TRUE)
    GPIOA_ResetBits(LED_PIN);
    GPIOA_ModeCfg(LED_PIN, GPIO_ModeOut_PP_5mA);
#endif

    PRINT("start.\n");
    PRINT("%s\n", VER_RF_LIB);
    USB_Init( );
    RFRole_Init( );
    RF_UartRxInit( );
    process_main( );
}

/******************************** endfile @ main ******************************/
