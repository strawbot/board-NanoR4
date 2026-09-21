/* generated vector source file - do not edit */
        #include "bsp_api.h"
        /* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
        #if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = sci_uart_rxi_isr, /* SCI2 RXI (Receive data full) */
            [1] = sci_uart_txi_isr, /* SCI2 TXI (Transmit data empty) */
            [2] = sci_uart_tei_isr, /* SCI2 TEI (Transmit end) */
            [3] = sci_uart_eri_isr, /* SCI2 ERI (Receive error) */
            [4] = rtc_carry_isr, /* RTC CARRY (Carry interrupt) */
            [5] = gpt_counter_overflow_isr, /* GPT2 COUNTER OVERFLOW (Overflow) */
#ifdef USE_NATIVE_USB_STACK
            [6] = usbfs_native_isr, /* USBFS INT — native r_usb_basic smoke test build only
                                        (env:nano_r4_usbtest). Wraps r_usb_basic's own
                                        usbfs_interrupt_handler with an ISR-fire counter,
                                        Board/usb_native_test.c. See Docs/usb-handoff.md,
                                        "Native stack smoke test". */
#else
            [6] = usbfs_int_isr, /* USBFS INT (USBFS interrupt) — was usbfs_interrupt_handler
                                     (r_usb_basic's own ISR, from the dead native USB stack
                                     still configured in RASC's Stacks tab); that was silently
                                     stealing every USB interrupt from TinyUSB's usbfs_int_isr
                                     (Board/usb_init.c). Regenerating from RASC will put this
                                     back unless that dead stack is also removed there — see
                                     Docs/usb-handoff.md. */
#endif
            /* [7]/[8]/[9] (USBFS RESUME / FIFO0 / FIFO1) removed: those are
             * r_usb_basic-only interrupt lines (excluded from the build, see
             * platformio.ini) that TinyUSB's RUSB2 port doesn't use — it
             * handles resume via INTENB0.RSME on the combined USBFS_INT line
             * (slot 6) instead. Leaving them unrouted (no g_interrupt_event_link_select
             * entry below) means the ICU never fires them, so no vector is needed. */
            [10] = iic_master_rxi_isr, /* IIC1 RXI (Receive data full) */
            [11] = iic_master_txi_isr, /* IIC1 TXI (Transmit data empty) */
            [12] = iic_master_tei_isr, /* IIC1 TEI (Transmit end) */
            [13] = iic_master_eri_isr, /* IIC1 ERI (Transfer error) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SCI2_RXI,GROUP0), /* SCI2 RXI (Receive data full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TXI,GROUP1), /* SCI2 TXI (Transmit data empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TEI,GROUP2), /* SCI2 TEI (Transmit end) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI2_ERI,GROUP3), /* SCI2 ERI (Receive error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_RTC_CARRY,GROUP4), /* RTC CARRY (Carry interrupt) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_GPT2_COUNTER_OVERFLOW,GROUP5), /* GPT2 COUNTER OVERFLOW (Overflow) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_USBFS_INT,GROUP6), /* USBFS INT (USBFS interrupt) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_IIC1_RXI,GROUP2), /* IIC1 RXI (Receive data full) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_IIC1_TXI,GROUP3), /* IIC1 TXI (Transmit data empty) */
            [12] = BSP_PRV_VECT_ENUM(EVENT_IIC1_TEI,GROUP4), /* IIC1 TEI (Transmit end) */
            [13] = BSP_PRV_VECT_ENUM(EVENT_IIC1_ERI,GROUP5), /* IIC1 ERI (Transfer error) */
        };
        #endif
        #endif