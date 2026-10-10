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
/* Í·ÎÄ¼þ°üº¬ */
#include "rf.h"
#include "rf_uart_rx.h"
#include "usb_uart.h"

extern volatile uint8_t RF_bound_Flag;

/* PA2 is a local active-low request key in the fixed Host firmware. */
static volatile uint8_t s_boot_request;

__INTERRUPT __HIGH_CODE
void GPIOA_IRQHandler( void )
{
    if( GPIOA_ReadITFlagBit( GPIO_Pin_2 ) )
    {
        GPIOA_ClearITFlagBit( GPIO_Pin_2 );
        s_boot_request = 1;
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
    GPIOA_ModeCfg( GPIO_Pin_2, GPIO_ModeIN_PU );
    GPIOA_ITModeCfg( GPIO_Pin_2, GPIO_ITMode_FallEdge );
    GPIOA_ClearITFlagBit( GPIO_Pin_2 );
    /* PA7 is not used as a role key in a fixed Host image. */
    GPIOA_ModeCfg( GPIO_Pin_7, GPIO_ModeIN_Floating );
    GPIOA_ModeCfg( LED_PIN, GPIO_ModeIN_Floating );
    PFIC_EnableIRQ( GPIO_A_IRQn );
}

static void HostLedTask( void )
{
    static uint8_t last_bound = 0xFF;
    static uint8_t pulse_on = 1;
    static uint32_t last_toggle;
    uint8_t bound = (RF_bound_Flag != 0);
    uint32_t now = SYS_GetSysTickCnt();

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
        /* Host role is the active-high LED in the user's dual-LED wiring. */
        GPIOA_SetBits( LED_PIN );
        GPIOA_ModeCfg( LED_PIN, GPIO_ModeOut_PP_5mA );
    }
}



/*********************************************************************
 * GLOBAL TYPEDEFS
 */
__attribute__((used))
__HIGH_CODE
void process_main( void )
{
    while(1)
    {
        HostLedTask();
        USB_StatusQuery();
    }
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Ö÷º¯Êý
 *
 * @return  none
 */
int main(void)
{
    HSECFG_Capacitance( HSECap_18p );
    SetSysClock( CLK_SOURCE_HSE_PLL_100MHz );
    HostIo_Init();
#ifdef DEBUG
    GPIOA_SetBits( bTXD_4 );
    GPIOA_ModeCfg( bTXD_4, GPIO_ModeOut_PP_5mA ); // TXD-ÅäÖÃÍÆÍìÊä³ö£¬×¢ÒâÏÈÈÃIO¿ÚÊä³ö¸ßµçÆ½
    UART_Remap( ENABLE, UART_TX_REMAP_PA7, UART_RX_REMAP_PA2 );
    UART_DefInit( );
#endif

    //ÉèÖÃLED_PIN
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
