#ifndef ROLE_MANAGER_H
#define ROLE_MANAGER_H

#include "CH57x_common.h"

/* The default only applies when the role record is blank or invalid. */
#define ROLE_MODE_HOST       0u
#define ROLE_MODE_DEVICE     1u
#ifndef ROLE_DEFAULT_MODE
#define ROLE_DEFAULT_MODE    ROLE_MODE_HOST
#endif

#define ROLE_KEY_LONGPRESS_MS  1500u
#define ROLE_RF_CTRL_ENTER_BOOT 0x01u

void RoleManager_Init(void);
void RoleManager_Task(void);
uint8_t RoleManager_GetMode(void);
uint8_t RoleManager_TakeRemoteBootRequest(void);
uint8_t RoleManager_TakeRemoteBootCommand(void);
void RoleManager_RequestRemoteBootCommand(void);
void RoleManager_TargetBootPulse(void);

/* PA3 drives the two opposite-polarity LEDs: 0, 1, or high impedance. */
void RoleManager_LedLow(void);
void RoleManager_LedHigh(void);
void RoleManager_LedOff(void);

#endif
