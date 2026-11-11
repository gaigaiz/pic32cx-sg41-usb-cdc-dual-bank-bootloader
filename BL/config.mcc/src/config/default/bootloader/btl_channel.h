#ifndef BTL_CHANNEL_H
#define BTL_CHANNEL_H

/*
 * Bootloader transport channel abstraction (byte-level front end).
 *
 * BTL_USE_CDC selects the byte front end at compile time:
 *   0 = SERCOM4 USART (legacy UART bootloader; default / fallback)
 *   1 = USB CDC virtual serial (implemented later in btl_cdc.c)
 *
 * The upper layer (frame parsing, command_task, flash_task, dual-bank swap,
 * CRC, run_Application) is shared and unchanged. Only these byte primitives
 * differ between channels. btl_cdc_service() exists on both builds so the task
 * loop can call it unconditionally; it is a no-op in UART mode.
 */

#include <stdint.h>
#include <stdbool.h>
#include "definitions.h"

#define BTL_USE_CDC   1

#if BTL_USE_CDC

void btl_cdc_initialize(void);
bool btl_io_rx_ready(void);
uint8_t btl_io_rx_byte(void);
void btl_io_tx_byte(uint8_t b);
void btl_io_tx_flush(void);
void btl_cdc_service(void);

#else

static inline bool btl_io_rx_ready(void)
{
    return SERCOM4_USART_ReceiverIsReady();
}

static inline uint8_t btl_io_rx_byte(void)
{
    return SERCOM4_USART_ReadByte();
}

static inline void btl_io_tx_byte(uint8_t b)
{
    SERCOM4_USART_WriteByte(b);
}

static inline void btl_io_tx_flush(void)
{
    while (SERCOM4_USART_TransmitComplete() == false)
    {
        /* wait until the last byte has shifted out */
    }
}

static inline void btl_cdc_service(void)
{
    /* UART mode: there is no USB task to service */
}

static inline void btl_cdc_initialize(void)
{
    /* UART mode: nothing to initialize on the USB side */
}

#endif

#endif /* BTL_CHANNEL_H */
