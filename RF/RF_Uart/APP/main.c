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
#include <rf.h>
#include <rf_uart_tx.h>
#include <uart.h>

/* Free-running core-clock counter for non-blocking LED timing. */
static void AppTimebaseInit( void )
{
    SysTick->CTLR = 0;
    SysTick->CNTL = 0;
    SysTick->CMP = 0xFFFFFFFFu;
    SysTick->SR = SysTick_SR_CNTIF;
    SysTick->CTLR = SysTick_CTLR_STCLK | SysTick_CTLR_STE;

    /* Calibrate the independent LSI/RTC counter. Target BOOT/RESET timing
     * uses this clock so UART-driven SYSCLK changes cannot stretch pulses. */
    LClk_Cfg( ENABLE );
    RTC_InitClock( Count_2047 );
}


/*********************************************************************
 * GLOBAL TYPEDEFS
 */
__attribute__((used))
void process_main( void )
{
    while(1)
    {
        RF_StatusQuery( );
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
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(CLK_SOURCE_HSE_PLL_24MHz);
    AppTimebaseInit();
    UART_Init( );
    PRINT("start.\n");
    PRINT("%s\n", VER_RF_LIB);
    RFRole_Init( );
    RF_UartTxInit( );
    process_main();
}

/******************************** endfile @ main ******************************/
