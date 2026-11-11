/*
 * custom_trigger.c - Firmware Trigger 强定义
 *
 * MCC 生成的 bootloader_common.c 里 bootloader_Trigger() 是 __WEAK return false，
 * 即使勾了 "Trigger From Firmware" 也不会检测 RAM 魔数。本文件覆盖弱函数，
 * 实现 Microchip 官方文档推荐的 Firmware Trigger：
 *   APP 在 RAM 前 16 字节（4 个 uint32_t）写 0x5048434D 后 NVIC_SystemReset()，
 *   BL 启动时检测到魔数就进升级模式；否则直接 run_Application(APP_START_ADDRESS)。
 *
 * 参考：bootloader_common.h 第 151-158 行示例代码
 */

#include <stdint.h>
#include <stdbool.h>

#define BTL_TRIGGER_RAM_START   0x20000000UL
#define BTL_TRIGGER_PATTERN     0x5048434DUL   /* "MCHP" 小端 */
#define BTL_TRIGGER_LEN_WORDS   4             /* 16 字节 = 4 个字 */

bool bootloader_Trigger(void)
{
    volatile uint32_t *ram = (volatile uint32_t *)BTL_TRIGGER_RAM_START;

    for (int i = 0; i < BTL_TRIGGER_LEN_WORDS; i++) {
        if (ram[i] != BTL_TRIGGER_PATTERN) {
            return false;
        }
    }

    /* 检测到魔数：清掉 RAM 前 16 字节，防止下次复位又误进升级模式 */
    ram[0] = 0;
    ram[1] = 0;
    ram[2] = 0;
    ram[3] = 0;

    return true;
}
