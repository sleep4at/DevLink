#include "role_manager.h"
#include "CH57x_flash.h"
#include "CH57x_gpio.h"
#include "CH57x_sys.h"
#include "ISP572.h"

#define ROLE_KEY_PIN          GPIO_Pin_7
#define REMOTE_BOOT_KEY_PIN   GPIO_Pin_2
#define TARGET_RESET_PIN      GPIO_Pin_2
#define TARGET_BOOT_PIN       GPIO_Pin_7
#define ROLE_LED_PIN          GPIO_Pin_3

/* Keep this page separate from the existing pairing record at 0x3B000. */
#define ROLE_CONFIG_FLASH_ADDR  0x03A000u
#define ROLE_CONFIG_MAGIC       0x524F4C45u
#define ROLE_CONFIG_VERSION     1u

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic;
    uint8_t version;
    uint8_t mode;
    uint16_t check;
} role_config_t;

static volatile uint8_t s_role_key_edge;
static volatile uint8_t s_boot_key_edge;
static volatile uint8_t s_remote_boot_request;
static volatile uint8_t s_remote_boot_command;
static uint8_t s_mode;
static uint8_t s_role_key_handled;
static uint32_t s_role_key_down_tick;

static uint16_t RoleConfig_Check(uint8_t mode)
{
    return (uint16_t)(0xA55Au ^ ROLE_CONFIG_VERSION ^ mode);
}

static uint8_t RoleConfig_Load(void)
{
    const role_config_t *cfg = (const role_config_t *)ROLE_CONFIG_FLASH_ADDR;
    if ((cfg->magic == ROLE_CONFIG_MAGIC) &&
        (cfg->version == ROLE_CONFIG_VERSION) &&
        (cfg->mode <= ROLE_MODE_DEVICE) &&
        (cfg->check == RoleConfig_Check(cfg->mode))) {
        return cfg->mode;
    }
    return ROLE_DEFAULT_MODE;
}

static void RoleConfig_SaveAndReset(uint8_t mode)
{
    uint32_t irq_status;
    role_config_t cfg;
    cfg.magic = ROLE_CONFIG_MAGIC;
    cfg.version = ROLE_CONFIG_VERSION;
    cfg.mode = mode;
    cfg.check = RoleConfig_Check(mode);

    /* Flash erase/write must run in thread context, never inside GPIO IRQ. */
    SYS_DisableAllIrq(&irq_status);
    if (FLASH_ROM_ERASE(ROLE_CONFIG_FLASH_ADDR, 4096) == 0 &&
        FLASH_ROM_WRITE(ROLE_CONFIG_FLASH_ADDR, &cfg, sizeof(cfg)) == 0 &&
        FLASH_ROM_VERIFY(ROLE_CONFIG_FLASH_ADDR, &cfg, sizeof(cfg)) == 0) {
        PFIC_SystemReset();
    }
    SYS_RecoverIrq(irq_status);
}

void RoleManager_Init(void)
{
    s_mode = RoleConfig_Load();
    s_role_key_edge = 0;
    s_boot_key_edge = 0;
    s_remote_boot_request = 0;
    s_remote_boot_command = 0;
    s_role_key_handled = 0;

    GPIOA_ModeCfg(ROLE_KEY_PIN, GPIO_ModeIN_PU);
    GPIOA_ITModeCfg(ROLE_KEY_PIN, GPIO_ITMode_FallEdge);
    if (s_mode == ROLE_MODE_HOST) {
        GPIOA_ModeCfg(REMOTE_BOOT_KEY_PIN, GPIO_ModeIN_PU);
        GPIOA_ITModeCfg(REMOTE_BOOT_KEY_PIN, GPIO_ITMode_FallEdge);
    } else {
        /* Do not drive the target's reset line while the device role is idle. */
        GPIOA_ModeCfg(TARGET_RESET_PIN, GPIO_ModeIN_Floating);
    }
    GPIOA_ClearITFlagBit(ROLE_KEY_PIN | REMOTE_BOOT_KEY_PIN);
    PFIC_EnableIRQ(GPIO_A_IRQn);
    RoleManager_LedOff();
}

uint8_t RoleManager_GetMode(void)
{
    return s_mode;
}

/* Shared PA interrupt: ISR records edges only; debounce, Flash, and RF work
 * are deliberately deferred to the main loop. */
__INTERRUPT __HIGH_CODE
void GPIOA_IRQHandler(void)
{
    if (GPIOA_ReadITFlagBit(ROLE_KEY_PIN)) {
        GPIOA_ClearITFlagBit(ROLE_KEY_PIN);
        s_role_key_edge = 1;
    }
    if ((s_mode == ROLE_MODE_HOST) && GPIOA_ReadITFlagBit(REMOTE_BOOT_KEY_PIN)) {
        GPIOA_ClearITFlagBit(REMOTE_BOOT_KEY_PIN);
        s_boot_key_edge = 1;
    }
}

void RoleManager_Task(void)
{
    if (s_role_key_edge) {
        s_role_key_edge = 0;
        s_role_key_down_tick = SYS_GetSysTickCnt();
        s_role_key_handled = 0;
    }
    if (!s_role_key_handled && !GPIOA_ReadPortPin(ROLE_KEY_PIN)) {
        uint32_t elapsed = SYS_GetSysTickCnt() - s_role_key_down_tick;
        uint32_t threshold = (GetSysClock() / 1000u) * ROLE_KEY_LONGPRESS_MS;
        if (elapsed >= threshold) {
            s_role_key_handled = 1;
            RoleConfig_SaveAndReset((uint8_t)(s_mode ^ 1u));
        }
    }
    if (GPIOA_ReadPortPin(ROLE_KEY_PIN)) {
        s_role_key_handled = 0;
    }

    if ((s_mode == ROLE_MODE_HOST) && s_boot_key_edge) {
        s_boot_key_edge = 0;
        if (!GPIOA_ReadPortPin(REMOTE_BOOT_KEY_PIN)) {
            s_remote_boot_request = 1;
        }
    }
}

uint8_t RoleManager_TakeRemoteBootRequest(void)
{
    uint8_t pending = s_remote_boot_request;
    s_remote_boot_request = 0;
    return pending;
}

uint8_t RoleManager_TakeRemoteBootCommand(void)
{
    uint8_t pending = s_remote_boot_command;
    s_remote_boot_command = 0;
    return pending;
}

void RoleManager_RequestRemoteBootCommand(void)
{
    s_remote_boot_command = 1;
}

/* Device role only. Call from main-loop context after a validated RF command. */
void RoleManager_TargetBootPulse(void)
{
    if (s_mode != ROLE_MODE_DEVICE) {
        return;
    }
    PFIC_DisableIRQ(GPIO_A_IRQn);
    GPIOA_SetBits(TARGET_RESET_PIN | TARGET_BOOT_PIN);
    GPIOA_ModeCfg(TARGET_RESET_PIN | TARGET_BOOT_PIN, GPIO_ModeOut_PP_5mA);
    mDelaymS(1);
    GPIOA_ResetBits(TARGET_RESET_PIN);
    mDelaymS(1);
    GPIOA_SetBits(TARGET_RESET_PIN);
    mDelaymS(1);
    GPIOA_ResetBits(TARGET_BOOT_PIN);
    mDelaymS(50);
    GPIOA_ModeCfg(TARGET_RESET_PIN, GPIO_ModeIN_Floating);
    GPIOA_ModeCfg(TARGET_BOOT_PIN, GPIO_ModeIN_PU);
    GPIOA_ITModeCfg(TARGET_BOOT_PIN, GPIO_ITMode_FallEdge);
    GPIOA_ClearITFlagBit(TARGET_BOOT_PIN);
    PFIC_EnableIRQ(GPIO_A_IRQn);
}

void RoleManager_LedLow(void)
{
    GPIOA_ResetBits(ROLE_LED_PIN);
    GPIOA_ModeCfg(ROLE_LED_PIN, GPIO_ModeOut_PP_5mA);
}

void RoleManager_LedHigh(void)
{
    GPIOA_SetBits(ROLE_LED_PIN);
    GPIOA_ModeCfg(ROLE_LED_PIN, GPIO_ModeOut_PP_5mA);
}

void RoleManager_LedOff(void)
{
    GPIOA_ModeCfg(ROLE_LED_PIN, GPIO_ModeIN_Floating);
}
