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
#include <rf.h>
#include <rf_uart_tx.h>
#include <rf_uart_rx.h>
#include <uart.h>
#include <usb_uart.h>
#include <role_manager.h>

extern volatile uint8_t RF_bound_Flag;

static void RoleLedTask(void)
{
    static uint8_t last_mode = 0xFF;
    static uint8_t last_bound = 0xFF;
    static uint8_t last_output = 0xFF;
    static uint8_t pulse_on = 1;
    static uint32_t last_toggle;
    uint8_t mode = RoleManager_GetMode();
    uint8_t bound = (RF_bound_Flag != 0);
    uint8_t output;
    uint32_t now = SYS_GetSysTickCnt();

    if ((mode != last_mode) || (bound != last_bound)) {
        last_mode = mode;
        last_bound = bound;
        pulse_on = 1;
        last_toggle = now;
    } else if (!bound && (uint32_t)(now - last_toggle) >= GetSysClock() / 2u) {
        pulse_on ^= 1u;
        last_toggle = now;
    }

    output = (!bound && !pulse_on) ? 0 : (mode == ROLE_MODE_HOST ? 2 : 1);
    if (output != last_output) {
        last_output = output;
        if (output == 0) {
            RoleManager_LedOff();
        } else if (output == 2) {
            RoleManager_LedHigh();
        } else {
            RoleManager_LedLow();
        }
    }
}


/*********************************************************************
 * GLOBAL TYPEDEFS
 */
__attribute__((used))
void process_main( void )
{
    while(1)
    {
        RoleLedTask();
        RoleManager_Task();
        if (RoleManager_GetMode() == ROLE_MODE_HOST) {
            USB_StatusQuery();
        } else {
            RF_StatusQuery();
            if (RoleManager_TakeRemoteBootCommand()) {
                RoleManager_TargetBootPulse();
            }
        }
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
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(CLK_SOURCE_HSE_PLL_100MHz);
    RoleManager_Init();
    PRINT("start.\n");
    PRINT("%s\n", VER_RF_LIB);
    if (RoleManager_GetMode() == ROLE_MODE_HOST) {
        USB_Init();
    } else {
        UART_Init();
    }
    RFRole_Init();
    if (RoleManager_GetMode() == ROLE_MODE_HOST) {
        RF_UartRxInit();
    } else {
        RF_UartTxInit();
    }
    process_main();
}

/******************************** endfile @ main ******************************/
