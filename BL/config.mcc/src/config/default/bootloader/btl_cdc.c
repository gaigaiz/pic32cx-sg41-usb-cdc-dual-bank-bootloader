/*
 * btl_cdc.c - USB CDC virtual-serial front end for the bootloader.
 *
 * Compiled only when BTL_USE_CDC == 1 (see bootloader/btl_channel.h).
 *
 * It implements the same byte-level primitives as the SERCOM4 UART front end,
 * but on top of the Harmony USB Device + CDC ACM stack. The frame parser,
 * command_task, flash_task, dual-bank erase/swap, CRC and run_Application logic
 * in bootloader_uart.c / bootloader_common.c are shared and unchanged.
 *
 * Polled design (no RTOS):
 *   - btl_cdc_initialize()  : bring up USB driver + device layer once.
 *   - btl_cdc_service()     : called from the bootloader task loop; runs the
 *                             driver/device Tasks, opens the device layer,
 *                             re-arms the CDC read and pumps buffered TX bytes.
 *   - btl_io_rx_ready/rx_byte / tx_byte / tx_flush : primitives consumed by
 *                             bootloader_uart.c.
 */

/* btl_channel.h MUST be included before the #if: it defines BTL_USE_CDC.
 * If the #if appears above this include, the macro is undefined (==0) here
 * and the whole file pre-processes to an empty translation unit, producing
 * undefined references to btl_io_* / btl_cdc_* at link time. */
#include "bootloader/btl_channel.h"

#if BTL_USE_CDC

#include "definitions.h"
#include "usb/usb_device.h"
#include "usb/usb_device_cdc.h"
#include "driver/usb/usbfsv1/drv_usbfsv1.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Local data                                                                 */
/* -------------------------------------------------------------------------- */

/* Entity of "extern SYSTEM_OBJECTS sysObj;" declared in definitions.h.
 * Harmony normally generates this in system.c, which this bootloader build does
 * not compile. The =1 front end provides it here; it stays absent in UART mode,
 * where the only references are discarded by --gc-sections. */
SYSTEM_OBJECTS sysObj;

/* USB driver + device layer objects; sysObj.* are also consumed by tasks.c. */
static DRV_USBFSV1_INIT drvInit = { 0 };
static USB_DEVICE_HANDLE usbdHandle = USB_DEVICE_HANDLE_INVALID;
static volatile bool usbdConfigured = false;
static volatile bool cdcRegistered = false;
/* D+ pull-up is enabled only once per session; see btl_attach_once(). */
static volatile bool btlAttached = false;

/* CDC receive: one 64-byte read slot (bulk EP max packet) + software ring. */
#define CDC_SLOT_SIZE               64U
static uint8_t rxSlot[CDC_SLOT_SIZE] __attribute__((aligned(16)));
static USB_DEVICE_CDC_TRANSFER_HANDLE rxHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;

/* Large enough to absorb USB bulk-OUT bursts (up to 64B/frame) while the frame
 * parser drains the ring; a 256B ring overflowed and dropped DATA payload bytes. */
#define RX_RING_SIZE                1024U
static uint8_t rxRing[RX_RING_SIZE];
static volatile uint16_t rxHead = 0;
static volatile uint16_t rxTail = 0;

/* CDC transmit: software ring + one pending 64-byte slot. */
#define TX_RING_SIZE                256U
/* Polled-loop guard: maximum btl_service_tx() passes that may keep txBusy set
 * while data is waiting before the previous WRITE_COMPLETE is treated as lost
 * and the transfer slot is force-released so the next pass can resubmit.
 * Buffered bytes are never discarded; this only avoids a permanent TX stall. */
#define TX_BUSY_STALL_LIMIT         32U
static uint8_t txRing[TX_RING_SIZE];
static volatile uint16_t txHead = 0;
static volatile uint16_t txTail = 0;
static uint8_t txSlot[CDC_SLOT_SIZE] __attribute__((aligned(16)));
static volatile bool txBusy = false;
static USB_DEVICE_CDC_TRANSFER_HANDLE txHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
/* Consecutive service passes that txBusy stayed asserted while TX data waited. */
static uint16_t txBusyStall = 0U;

/* EP0 control-stage buffers for CDC ACM class requests. The host asserts
 * SET_CONTROL_LINE_STATE when opening the port; GET/SET_LINE_CODING carry a
 * 7-byte coding. The ACM layer only forwards these as events; the application
 * must complete the data/status stages, otherwise the host control IRP hangs
 * and CreateFile on the virtual COM port blocks or returns access denied. */
static USB_CDC_LINE_CODING cdcLineCodingGet;
static USB_CDC_LINE_CODING cdcLineCodingSet;

/* Forward declarations */
static void btl_cdc_event(USB_DEVICE_CDC_INDEX instanceIndex, USB_DEVICE_CDC_EVENT event,
                          void *pData, uintptr_t context);

static inline void btl_irq_lock(void)
{
    __disable_irq();
}

static inline void btl_irq_unlock(void)
{
    __enable_irq();
}

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/* Re-arm the bulk-IN host->device read once the device is configured. */
static void btl_arm_read(void)
{
    if (!usbdConfigured)
    {
        return;
    }
    if (rxHandle != USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID)
    {
        return;
    }

    if (USB_DEVICE_CDC_Read(USB_DEVICE_CDC_INDEX_0, &rxHandle, rxSlot, sizeof(rxSlot))
            != USB_DEVICE_CDC_RESULT_OK)
    {
        rxHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    }
}

/* Attach (enable the D+ pull-up) at most once per session. The driver's
 * DRV_USBFSV1_DEVICE_Attach() writes USB_INTFLAG = USB_DEVICE_INTFLAG_Msk on
 * every call, which clears all latched EORST/SETUP flags; calling it every loop
 * could erase a flag before DRV_USBFSV1_Tasks_ISR() consumes it. Re-attach is
 * allowed only after POWER_REMOVED clears btlAttached. */
static void btl_attach_once(void)
{
    if ((usbdHandle != USB_DEVICE_HANDLE_INVALID) && (!btlAttached))
    {
        btlAttached = true;
        USB_DEVICE_Attach(usbdHandle);
    }
}

/* Device layer event: attach on VBUS, configure -> register CDC + arm read. */
static void btl_usbd_event(USB_DEVICE_EVENT event, void *pData, uintptr_t context)
{
    (void)pData;
    (void)context;

    switch (event)
    {
        case USB_DEVICE_EVENT_POWER_DETECTED:
            btl_attach_once();
            break;

        case USB_DEVICE_EVENT_POWER_REMOVED:
            if (usbdHandle != USB_DEVICE_HANDLE_INVALID)
            {
                USB_DEVICE_Detach(usbdHandle);
            }
            btlAttached = false;
            usbdConfigured = false;
            break;

        case USB_DEVICE_EVENT_CONFIGURED:
            usbdConfigured = true;
            if (!cdcRegistered)
            {
                USB_DEVICE_CDC_EventHandlerSet(USB_DEVICE_CDC_INDEX_0, btl_cdc_event, (uintptr_t)0);
                cdcRegistered = true;
            }
            btl_arm_read();
            break;

        case USB_DEVICE_EVENT_RESET:
        case USB_DEVICE_EVENT_DECONFIGURED:
            usbdConfigured = false;
            rxHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            break;

        default:
            break;
    }
}

/* CDC function driver event: move received bytes into the RX ring, pump TX. */
static void btl_cdc_event(USB_DEVICE_CDC_INDEX instanceIndex, USB_DEVICE_CDC_EVENT event,
                          void *pData, uintptr_t context)
{
    uint16_t n;
    uint16_t i;
    USB_DEVICE_CDC_EVENT_DATA_READ_COMPLETE *rd;

    (void)instanceIndex;
    (void)context;

    switch (event)
    {
        case USB_DEVICE_CDC_EVENT_READ_COMPLETE:
            rd = (USB_DEVICE_CDC_EVENT_DATA_READ_COMPLETE *)pData;
            n = (uint16_t)rd->length;

            btl_irq_lock();
            for (i = 0U; i < n; i++)
            {
                uint16_t next = (uint16_t)((rxHead + 1U) % RX_RING_SIZE);
                if (next == rxTail)
                {
                    break; /* ring full: drop the remaining bytes */
                }
                rxRing[rxHead] = rxSlot[i];
                rxHead = next;
            }
            btl_irq_unlock();

            rxHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            btl_arm_read();
            break;

        case USB_DEVICE_CDC_EVENT_WRITE_COMPLETE:
            txBusy = false;
            txBusyStall = 0U;
            txHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            break;

        /* Zero-data-stage ACM write. Acknowledge the status stage so the host
         * open request completes; usbser waits on this when it asserts DTR/RTS
         * while opening the COM port. */
        case USB_DEVICE_CDC_EVENT_SET_CONTROL_LINE_STATE:
            (void)USB_DEVICE_ControlStatus(usbdHandle, USB_DEVICE_CONTROL_STATUS_OK);
            break;

        case USB_DEVICE_CDC_EVENT_SEND_BREAK:
            (void)USB_DEVICE_ControlStatus(usbdHandle, USB_DEVICE_CONTROL_STATUS_OK);
            break;

        /* Host reads the coding: answer a fixed 115200-8N1. The bit rate has
         * no physical meaning on a virtual serial link. */
        case USB_DEVICE_CDC_EVENT_GET_LINE_CODING:
            cdcLineCodingGet.dwDTERate   = 115200U;
            cdcLineCodingGet.bCharFormat = USB_CDC_LINE_CODING_STOP_1_BIT;
            cdcLineCodingGet.bParityType = USB_CDC_LINE_CODING_PARITY_NONE;
            cdcLineCodingGet.bDataBits   = USB_CDC_LINE_CODING_DATA_8_BIT;
            (void)USB_DEVICE_ControlSend(usbdHandle,
                    &cdcLineCodingGet, sizeof(cdcLineCodingGet));
            break;

        /* Host writes a coding: arm the data-stage receive; the status stage
         * is acknowledged in the DATA_RECEIVED event below. */
        case USB_DEVICE_CDC_EVENT_SET_LINE_CODING:
            (void)USB_DEVICE_ControlReceive(usbdHandle,
                    &cdcLineCodingSet, sizeof(cdcLineCodingSet));
            break;

        case USB_DEVICE_CDC_EVENT_CONTROL_TRANSFER_DATA_RECEIVED:
            /* SET_LINE_CODING data stage finished; finish its status stage. */
            (void)USB_DEVICE_ControlStatus(usbdHandle, USB_DEVICE_CONTROL_STATUS_OK);
            break;

        case USB_DEVICE_CDC_EVENT_CONTROL_TRANSFER_DATA_SENT:
            /* ControlSend data stage done; the library closes the transfer. */
            break;

        case USB_DEVICE_CDC_EVENT_CONTROL_TRANSFER_ABORTED:
            break;

        default:
            break;
    }
}

/* Submit buffered TX bytes whenever the previous transfer has completed.
 * Polled robustness: a missing WRITE_COMPLETE must not permanently block TX. */
static void btl_service_tx(void)
{
    uint16_t avail;
    uint16_t n;
    uint16_t i;

    if (txBusy)
    {
        /* Only count a stall while there is data actually waiting to be sent. */
        btl_irq_lock();
        avail = (uint16_t)((txHead + TX_RING_SIZE - txTail) % TX_RING_SIZE);
        btl_irq_unlock();

        if (avail == 0U)
        {
            return;
        }

        txBusyStall++;
        if (txBusyStall < TX_BUSY_STALL_LIMIT)
        {
            return; /* normal in-flight wait; completion callback expected */
        }

        /* Completion event presumed lost: release the slot and let the next
         * pass resubmit. Ring bytes are kept; nothing is dropped. */
        txBusy = false;
        txBusyStall = 0U;
        txHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
        return;
    }

    btl_irq_lock();
    avail = (uint16_t)((txHead + TX_RING_SIZE - txTail) % TX_RING_SIZE);
    btl_irq_unlock();

    if (avail == 0U)
    {
        return;
    }

    n = (avail > CDC_SLOT_SIZE) ? CDC_SLOT_SIZE : avail;

    btl_irq_lock();
    for (i = 0U; i < n; i++)
    {
        txSlot[i] = txRing[txTail];
        txTail = (uint16_t)((txTail + 1U) % TX_RING_SIZE);
    }
    btl_irq_unlock();

    if (USB_DEVICE_CDC_Write(USB_DEVICE_CDC_INDEX_0, &txHandle, txSlot, n,
            USB_DEVICE_CDC_TRANSFER_FLAGS_DATA_COMPLETE) == USB_DEVICE_CDC_RESULT_OK)
    {
        txBusy = true;
        txBusyStall = 0U;
    }
    else
    {
        /* Submit rejected (e.g. queue full): nothing was queued. Clear busy so
         * the next pass retries; never leave txBusy stuck true. */
        txBusy = false;
        txHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    }
}

/* -------------------------------------------------------------------------- */
/* Public channel entry points (declared in btl_channel.h)                   */
/* -------------------------------------------------------------------------- */

void btl_cdc_initialize(void)
{
    drvInit.moduleInit.value = 0;
    drvInit.usbID = USB_REGS;
    drvInit.runInStandby = false;
    drvInit.suspendInSleep = false;

    /* Four NVIC vectors: OTHER / SOF_HSOF / TRCPT0 / TRCPT1. */
    drvInit.interruptSource  = USB_OTHER_IRQn;
    drvInit.interruptSource1 = USB_SOF_HSOF_IRQn;
    drvInit.interruptSource2 = USB_TRCPT0_IRQn;
    drvInit.interruptSource3 = USB_TRCPT1_IRQn;

    drvInit.operationSpeed = USB_SPEED_FULL;
    drvInit.operationMode = DRV_USBFSV1_OPMODE_DEVICE;

    /* Device mode: VBUS comparator unused (NULL = session assumed valid). */
    drvInit.rootHubAvailableCurrent = 0U;
    drvInit.portPowerEnable = NULL;
    drvInit.portIndication = NULL;
    drvInit.portOverCurrentDetect = NULL;
    drvInit.vbusComparator = NULL;

    sysObj.drvUSBFSV1Object =
        DRV_USBFSV1_Initialize(DRV_USBFSV1_INDEX_0, (const SYS_MODULE_INIT *)&drvInit);

    sysObj.usbDevObject0 =
        USB_DEVICE_Initialize(USB_DEVICE_INDEX_0, (const SYS_MODULE_INIT *)&usbDevInitData);

    /* Polled operation. This build's startup vector table only contains the
     * initial SP and Reset vectors (no USB external-interrupt vectors), and
     * plib_nvic.c enabled the four USB lines during SYS_Initialize. Disable
     * them at the NVIC here (this runs after SYS_Initialize) and service the
     * device stack by polling DRV_USBFSV1_Tasks_ISR() in btl_cdc_service(), so
     * a pending USB line cannot jump through an unused vector slot. */
    (void)SYS_INT_SourceDisable(USB_OTHER_IRQn);
    (void)SYS_INT_SourceDisable(USB_SOF_HSOF_IRQn);
    (void)SYS_INT_SourceDisable(USB_TRCPT0_IRQn);
    (void)SYS_INT_SourceDisable(USB_TRCPT1_IRQn);
}

void btl_cdc_service(void)
{
    /* Driver level: VBUS session edge detection (polled). */
    DRV_USBFSV1_Tasks(sysObj.drvUSBFSV1Object);

    /* Device layer state machine MUST run before USB_DEVICE_Open(): the open
     * call returns INVALID until USB_DEVICE_Tasks() advances the instance from
     * OPENING to READY (that step also opens the driver handle internally). */
    USB_DEVICE_Tasks(sysObj.usbDevObject0);

    /* Open the device layer once; retry until the device layer is ready. */
    if (usbdHandle == USB_DEVICE_HANDLE_INVALID)
    {
        usbdHandle = USB_DEVICE_Open(USB_DEVICE_INDEX_0, DRV_IO_INTENT_READWRITE);
        if (usbdHandle != USB_DEVICE_HANDLE_INVALID)
        {
            USB_DEVICE_EventHandlerSet(usbdHandle, btl_usbd_event, (uintptr_t)0);
        }
    }

    if (usbdHandle != USB_DEVICE_HANDLE_INVALID)
    {
        /* Enable the D+ pull-up a single time per session (see btl_attach_once). */
        btl_attach_once();

        /* Polled device-stack service: USB reset, EP0 SETUP (device/config
         * descriptors) and transfer-complete events are handled here because
         * the startup table has no USB IRQ vectors. Safe when idle: the driver
         * only acts on set-and-enabled peripheral flags. */
        DRV_USBFSV1_Tasks_ISR(sysObj.drvUSBFSV1Object);
    }

    if (usbdConfigured)
    {
        btl_arm_read();
    }

    btl_service_tx();
}

bool btl_io_rx_ready(void)
{
    bool ready;

    btl_irq_lock();
    ready = (rxHead != rxTail);
    btl_irq_unlock();

    return ready;
}

uint8_t btl_io_rx_byte(void)
{
    uint8_t b = 0U;

    btl_irq_lock();
    if (rxHead != rxTail)
    {
        b = rxRing[rxTail];
        rxTail = (uint16_t)((rxTail + 1U) % RX_RING_SIZE);
    }
    btl_irq_unlock();

    return b;
}

void btl_io_tx_byte(uint8_t b)
{
    btl_irq_lock();
    if (((txHead + 1U) % TX_RING_SIZE) != txTail)
    {
        txRing[txHead] = b;
        txHead = (uint16_t)((txHead + 1U) % TX_RING_SIZE);
    }
    btl_irq_unlock();
}

void btl_io_tx_flush(void)
{
    /* Bounded wait: pump the stack until the TX queue drains. If the host is
     * not connected/configured this returns instead of blocking the bootloader. */
    uint32_t guard = 500000UL;
    bool pending;

    while (guard-- > 0U)
    {
        btl_cdc_service();

        btl_irq_lock();
        pending = ((txHead != txTail) || txBusy);
        btl_irq_unlock();

        if (!pending)
        {
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* USB vector handlers (reserved for a future interrupt-driven build).        */
/* This polled build disables the four USB NVIC lines in btl_cdc_initialize()*/
/* and drives the stack from btl_cdc_service(), so these handlers are never   */
/* entered; unreferenced copies are dropped by --gc-sections. They are kept  */
/* ready in case the startup vector table is later completed for IRQ use.     */
/* -------------------------------------------------------------------------- */

void USB_OTHER_Handler(void)
{
    DRV_USBFSV1_Tasks_ISR(sysObj.drvUSBFSV1Object);
}

void USB_SOF_HSOF_Handler(void)
{
    DRV_USBFSV1_Tasks_ISR(sysObj.drvUSBFSV1Object);
}

void USB_TRCPT0_Handler(void)
{
    DRV_USBFSV1_Tasks_ISR(sysObj.drvUSBFSV1Object);
}

void USB_TRCPT1_Handler(void)
{
    DRV_USBFSV1_Tasks_ISR(sysObj.drvUSBFSV1Object);
}

#endif /* BTL_USE_CDC */
