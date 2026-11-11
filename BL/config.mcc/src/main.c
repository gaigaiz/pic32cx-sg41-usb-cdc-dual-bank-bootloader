/*******************************************************************************
  Main Source File

  Company:
    Microchip Technology Inc.

  File Name:
    main.c

  Summary:
    This file contains the "main" function for bootloader project.

  Description:
    This file contains the "main" function for bootloader project.  The
    "main" function calls the "SYS_Initialize" function to initialize
    all modules in the system.
    It calls "bootloader_start" once system is initialized.
 *******************************************************************************/

// DOM-IGNORE-BEGIN
/*******************************************************************************
* Copyright (C) 2023 Microchip Technology Inc. and its subsidiaries.
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
// Section: Included Files
// *****************************************************************************
// *****************************************************************************

#include <stddef.h>                     // Defines NULL
#include <stdbool.h>                    // Defines true
#include <stdlib.h>                     // Defines EXIT_FAILURE
#include "definitions.h"                // SYS function prototypes
#include "bootloader/btl_channel.h"    // BTL_USE_CDC channel selection


// *****************************************************************************
// *****************************************************************************
// Section: Main Entry Point
// *****************************************************************************
// *****************************************************************************

int main ( void )
{
    /* Initialize all modules */
    SYS_Initialize ( NULL );

#if BTL_USE_CDC
    btl_cdc_initialize();
#endif

#if !BTL_USE_CDC
    /* 启动提示：用寄存器直接发，不依赖 PLIB */
    const char *msg = "BL READY\r\n";
    for (int i = 0; msg[i] != '\0'; i++) {
        while (!(SERCOM4_REGS->USART_INT.SERCOM_INTFLAG & SERCOM_USART_INT_INTFLAG_DRE_Msk));
        SERCOM4_REGS->USART_INT.SERCOM_DATA = msg[i];
    }

    /* 打印当前 APP 版本号 */
    {
        uint32_t ver = bootloader_AppVersionGet();
        char vbuf[16];
        int vlen = 0;
        vbuf[vlen++] = 'A';
        vbuf[vlen++] = 'P';
        vbuf[vlen++] = 'P';
        vbuf[vlen++] = ' ';
        vbuf[vlen++] = 'V';
        if (ver >= 10U) {
            vbuf[vlen++] = (char)('0' + (ver / 10U));
        }
        vbuf[vlen++] = (char)('0' + (ver % 10U));
        vbuf[vlen++] = '.';
        vbuf[vlen++] = '0';
        vbuf[vlen++] = '\r';
        vbuf[vlen++] = '\n';
        vbuf[vlen] = '\0';
        for (int i = 0; vbuf[i] != '\0'; i++) {
            while (!(SERCOM4_REGS->USART_INT.SERCOM_INTFLAG & SERCOM_USART_INT_INTFLAG_DRE_Msk));
            SERCOM4_REGS->USART_INT.SERCOM_DATA = vbuf[i];
        }
    }
#endif /* !BTL_USE_CDC: SERCOM4 startup banner only */

    while (true)
    {
        bootloader_UART_Tasks();
    }

    /* Execution should not come here during normal operation */
    return ( EXIT_FAILURE );
}


/*******************************************************************************
 End of File
*/

