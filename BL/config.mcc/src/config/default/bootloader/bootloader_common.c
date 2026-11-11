/*******************************************************************************
  Bootloader Common Source File

  File Name:
    bootloader_common.c

  Summary:
    This file contains common definitions and functions.

  Description:
    This file contains common definitions and functions.
 *******************************************************************************/

// DOM-IGNORE-BEGIN
/*******************************************************************************
* Copyright (C) 2019 Microchip Technology Inc. and its subsidiaries.
*
* Subject to your compliance with these terms, you may use Microchip software
* and any derivatives exclusively with Microchip products. It is your
* responsibility to comply with third party license terms applicable to your
* use of third party software (including open source software) that may
* accompany Microchip software.
*
* THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
* EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
* WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
* PARTICULAR PURPOSE.
*
* IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
* INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
* WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
* BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
* FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
* ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
* THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 *******************************************************************************/
// DOM-IGNORE-END

// *****************************************************************************
// *****************************************************************************
// Section: Include Files
// *****************************************************************************
// *****************************************************************************

#include "bootloader_common.h"

// *****************************************************************************
// *****************************************************************************
// Section: Type Definitions
// *****************************************************************************
// *****************************************************************************

/* Bootloader Major and Minor version sent for a Read Version command (MAJOR.MINOR)*/
#define BTL_MAJOR_VERSION       3U
#define BTL_MINOR_VERSION       7U

/* TEMP USB bring-up switch: force the bootloader to stay resident and never
 * jump to the APP, so the CDC enumeration path can be tested on a board that
 * may still hold stale non-FF data at APP_START_ADDRESS. Set to 0 (or delete
 * this line) to restore the normal APP-boot decision. NOTE: also affects the
 * UART build; remove before the =0 regression test. */
#define BTL_TEMP_FORCE_STAY_IN_BL 0U

#define ASM_VECTOR              asm("bx %0"::"r" (reset_vector))

// *****************************************************************************
// *****************************************************************************
// Section: Global objects
// *****************************************************************************
// *****************************************************************************

// *****************************************************************************
// *****************************************************************************
// Section: Bootloader Local Functions
// *****************************************************************************
// *****************************************************************************

// *****************************************************************************
// *****************************************************************************
// Section: Bootloader Global Functions
// *****************************************************************************
// *****************************************************************************

/* MISRA C-2023 Rule 8.6 and 5.8 deviated below. Deviation record ID -
   H3_MISRAC_2023_R_8_6_DR_1, H3_MISRAC_2023_R_5_8_DR_1 */
bool __WEAK bootloader_Trigger(void)
{
    /* Function can be overriden with custom implementation */
    return false;
}

void __WEAK SYS_DeInitialize( void *data )
{
    /* Function can be overriden with custom implementation */
}

uint16_t __WEAK bootloader_GetVersion( void )
{
    /* Function can be overriden with custom implementation */
    uint16_t btlVersion = (((BTL_MAJOR_VERSION & (uint16_t)0xFFU) << 8) | (BTL_MINOR_VERSION & (uint16_t)0xFFU));

    return btlVersion;
}


/* MISRA C-2023 Rule 10.1, 10.4, 11.1, 11.6 deviated below. Deviation record ID -
   H3_MISRAC_2023_R_10_1_DR_1, H3_MISRAC_2023_R_10_4_DR_1, H3_MISRAC_2023_R_11_1_DR_1 & H3_MISRAC_2023_R_11_6_DR_1 */


/* Function to Generate CRC using the device service unit peripheral on programmed data */
uint32_t bootloader_CRCGenerate(uint32_t start_addr, uint32_t size)
{
    uint32_t crc  = 0xffffffffU;

    PAC_PeripheralProtectSetup (PAC_PERIPHERAL_DSU, PAC_PROTECTION_CLEAR);

    (void) DSU_CRCCalculate (start_addr, size, crc, &crc);

    PAC_PeripheralProtectSetup (PAC_PERIPHERAL_DSU, PAC_PROTECTION_SET);

    return crc;
}

/* Trigger a reset */
void bootloader_TriggerReset(void)
{
    NVIC_SystemReset();
}

void run_Application(uint32_t address)
{
    uint32_t msp            = *(uint32_t *)(address);
    uint32_t reset_vector   = *(uint32_t *)(address + 4U);

    if (msp == 0xffffffffU)
    {
        return;
    }

    /* Call Deinitialize routine to free any resources acquired by Bootloader */
    SYS_DeInitialize(NULL);

    __set_MSP(msp);

    ASM_VECTOR;
}



/* MISRAC 2023 deviation block end */

/* Erase the 8KB state block and program one 512B page holding
 * expected CRC + APP version + retry_cnt + bankswapped, rest 0xFF.
 * addr: physical block address (0x7E000 active, 0xFE000 inactive).
 */
static void FailSafeWrite(uint32_t addr, uint32_t expected_crc, uint32_t app_version,
                          uint32_t retry_cnt, uint32_t bankswapped)
{
    static uint32_t page_buffer[PAGE_SIZE / 4U];
    uint32_t i;

    for (i = 0U; i < (PAGE_SIZE / 4U); i++)
    {
        page_buffer[i] = 0xFFFFFFFFU;
    }
    page_buffer[FAILSAFE_EXPECTED_CRC_OFFSET / 4U] = expected_crc;
    page_buffer[FAILSAFE_APP_VERSION_OFFSET / 4U] = app_version;
    page_buffer[FAILSAFE_RETRY_CNT_OFFSET / 4U] = retry_cnt;
    page_buffer[FAILSAFE_BANKSWAPPED_OFFSET / 4U] = bankswapped;

    (void) NVMCTRL_BlockErase(addr);
    while (NVMCTRL_IsBusy())
    {
        /* wait for block erase complete */
    }
    (void) NVMCTRL_PageWrite(page_buffer, addr);
    while (NVMCTRL_IsBusy())
    {
        /* wait for page write complete */
    }
}

/* Check if the last reset was a WDT timeout. */
static bool IsWdtReset(void)
{
    return ((RSTC_REGS->RSTC_RCAUSE & RSTC_RCAUSE_WDT_Msk) != 0U);
}

/* Called on every cold boot with no trigger magic present.
 * Returns true when the APP may be jumped to; false to stay in serial
 * upgrade mode.
 */
bool bootloader_BootCheck(void)
{
    uint32_t expected_crc;
    uint32_t actual_crc;
    uint32_t retry_cnt;
    uint32_t bankswapped;
    uint32_t version;

#if BTL_TEMP_FORCE_STAY_IN_BL
    /* TEMP USB bring-up: stay in BL unconditionally; skip APP CRC / retry /
     * bank-swap jump logic while validating USB CDC enumeration. */
    return false;
#endif

    if (*(uint32_t *)APP_START_ADDRESS == 0xFFFFFFFFU)
    {
        return false;   /* empty APP area */
    }

    expected_crc = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_EXPECTED_CRC_OFFSET);
    actual_crc   = bootloader_CRCGenerate(APP_START_ADDRESS, APP_REGION_SIZE);

    /* A recorded expected CRC must match; a blank sector (first debugger flash)
     * skips the CRC check.
     */
    if ((expected_crc != 0xFFFFFFFFU) && (expected_crc != actual_crc))
    {
        return false;   /* APP region corrupted */
    }

    version = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_APP_VERSION_OFFSET);
    if (version == 0xFFFFFFFFU)
    {
        version = 0U;
    }
    retry_cnt = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_RETRY_CNT_OFFSET);
    if (retry_cnt == 0xFFFFFFFFU)
    {
        retry_cnt = 0U;
    }
    bankswapped = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_BANKSWAPPED_OFFSET);
    if (bankswapped == 0xFFFFFFFFU)
    {
        bankswapped = 0U;
    }

    /* Only WDT timeout resets increment retry_cnt. POR / soft reset don't. */
    if (IsWdtReset())
    {
        retry_cnt++;
        FailSafeWrite(FAILSAFE_SECTOR_ADDR, expected_crc, version, retry_cnt, bankswapped);
    }

    /* After 3 WDT failures, bank-swap once. If the swapped bank also fails
     * 3 times, stay in BL (both banks dead).
     */
    if (retry_cnt >= WDT_RETRY_THRESHOLD)
    {
        if (bankswapped == 0U)
        {
            /* First bank failure: swap to the other bank, reset retry counter. */
            FailSafeWrite(FAILSAFE_SECTOR_ADDR, expected_crc, version, 0U, 1U);
            NVMCTRL_BankSwap();
            /* Will not return from here. */
        }
        /* Both banks failed: stay in BL. */
        return false;
    }

    return true;
}

/* End of a successful OTA, before BankSwap: store the expected CRC of the
 * freshly programmed inactive-bank APP, bump version, clear retry_cnt / bankswapped.
 */
void bootloader_RecordAppCrc(uint32_t expected_crc)
{
    uint32_t old_version = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_APP_VERSION_OFFSET);
    uint32_t new_version;

    if (old_version == 0xFFFFFFFFU)
    {
        old_version = 0U;
    }
    new_version = old_version + 1U;

    FailSafeWrite(INACTIVE_BANK_START + FAILSAFE_SECTOR_ADDR, expected_crc, new_version, 0U, 0U);
}

/* Read the current APP version number (0xFFFFFFFF = treated as 0). */
uint32_t bootloader_AppVersionGet(void)
{
    uint32_t version = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_APP_VERSION_OFFSET);
    if (version == 0xFFFFFFFFU)
    {
        version = 0U;
    }
    return version;
}