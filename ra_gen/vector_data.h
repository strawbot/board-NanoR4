/* generated vector header file - do not edit */
        #ifndef VECTOR_DATA_H
        #define VECTOR_DATA_H
        #ifdef __cplusplus
        extern "C" {
        #endif
                /* Number of interrupts allocated */
        #ifndef VECTOR_DATA_IRQ_COUNT
        #define VECTOR_DATA_IRQ_COUNT    (22)
        #endif
        /* ISR prototypes */
        void sci_uart_rxi_isr(void);
        void sci_uart_txi_isr(void);
        void sci_uart_tei_isr(void);
        void sci_uart_eri_isr(void);
        void rtc_carry_isr(void);
        void gpt_counter_overflow_isr(void);
        /* ICU IRQ3 (P110/D12) edge counter -- added by hand, Board/edge_test.c */
        void edge_test_irq3_isr(void);
#ifdef USE_NATIVE_USB_STACK
        void usbfs_native_isr(void); /* env:nano_r4_usbtest only — see vector_data.c */
#else
        void usbfs_int_isr(void); /* was usbfs_interrupt_handler — see vector_data.c */
#endif
        /* usbfs_resume_handler/usbfs_d0fifo_handler/usbfs_d1fifo_handler removed
         * along with their vector slots — see vector_data.c. */
        void iic_master_rxi_isr(void);
        void iic_master_txi_isr(void);
        void iic_master_tei_isr(void);
        void iic_master_eri_isr(void);
        /* GPT7 frame timer for muscle_sr — added by hand, see Board/frame_timer.c */
        void frame_timer_edge_isr(void);
        void frame_timer_sample_isr(void);
        void frame_timer_frame_isr(void);

        /* Vector table allocations */
        #define VECTOR_NUMBER_SCI2_RXI ((IRQn_Type) 0) /* SCI2 RXI (Receive data full) */
        #define SCI2_RXI_IRQn          ((IRQn_Type) 0) /* SCI2 RXI (Receive data full) */
        #define VECTOR_NUMBER_SCI2_TXI ((IRQn_Type) 1) /* SCI2 TXI (Transmit data empty) */
        #define SCI2_TXI_IRQn          ((IRQn_Type) 1) /* SCI2 TXI (Transmit data empty) */
        #define VECTOR_NUMBER_SCI2_TEI ((IRQn_Type) 2) /* SCI2 TEI (Transmit end) */
        #define SCI2_TEI_IRQn          ((IRQn_Type) 2) /* SCI2 TEI (Transmit end) */
        #define VECTOR_NUMBER_SCI2_ERI ((IRQn_Type) 3) /* SCI2 ERI (Receive error) */
        #define SCI2_ERI_IRQn          ((IRQn_Type) 3) /* SCI2 ERI (Receive error) */
        #define VECTOR_NUMBER_RTC_CARRY ((IRQn_Type) 4) /* RTC CARRY (Carry interrupt) */
        #define RTC_CARRY_IRQn          ((IRQn_Type) 4) /* RTC CARRY (Carry interrupt) */
        #define VECTOR_NUMBER_GPT2_COUNTER_OVERFLOW ((IRQn_Type) 5) /* GPT2 COUNTER OVERFLOW (Overflow) */
        #define GPT2_COUNTER_OVERFLOW_IRQn          ((IRQn_Type) 5) /* GPT2 COUNTER OVERFLOW (Overflow) */
        #define VECTOR_NUMBER_USBFS_INT ((IRQn_Type) 6) /* USBFS INT (USBFS interrupt) */
        #define USBFS_INT_IRQn          ((IRQn_Type) 6) /* USBFS INT (USBFS interrupt) */
        #define VECTOR_NUMBER_USBFS_RESUME ((IRQn_Type) 7) /* USBFS RESUME (USBFS resume interrupt) */
        #define USBFS_RESUME_IRQn          ((IRQn_Type) 7) /* USBFS RESUME (USBFS resume interrupt) */
        #define VECTOR_NUMBER_USBFS_FIFO_0 ((IRQn_Type) 8) /* USBFS FIFO 0 (DMA/DTC transfer request 0) */
        #define USBFS_FIFO_0_IRQn          ((IRQn_Type) 8) /* USBFS FIFO 0 (DMA/DTC transfer request 0) */
        #define VECTOR_NUMBER_USBFS_FIFO_1 ((IRQn_Type) 9) /* USBFS FIFO 1 (DMA/DTC transfer request 1) */
        #define USBFS_FIFO_1_IRQn          ((IRQn_Type) 9) /* USBFS FIFO 1 (DMA/DTC transfer request 1) */
        #define VECTOR_NUMBER_IIC1_RXI ((IRQn_Type) 10) /* IIC1 RXI (Receive data full) */
        #define IIC1_RXI_IRQn          ((IRQn_Type) 10) /* IIC1 RXI (Receive data full) */
        #define VECTOR_NUMBER_IIC1_TXI ((IRQn_Type) 11) /* IIC1 TXI (Transmit data empty) */
        #define IIC1_TXI_IRQn          ((IRQn_Type) 11) /* IIC1 TXI (Transmit data empty) */
        #define VECTOR_NUMBER_IIC1_TEI ((IRQn_Type) 12) /* IIC1 TEI (Transmit end) */
        #define IIC1_TEI_IRQn          ((IRQn_Type) 12) /* IIC1 TEI (Transmit end) */
        #define VECTOR_NUMBER_IIC1_ERI ((IRQn_Type) 13) /* IIC1 ERI (Transfer error) */
        #define IIC1_ERI_IRQn          ((IRQn_Type) 13) /* IIC1 ERI (Transfer error) */
        #define VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_A ((IRQn_Type) 14) /* GPT7 CAPTURE COMPARE A */
        #define GPT7_CAPTURE_COMPARE_A_IRQn          ((IRQn_Type) 14) /* GPT7 CAPTURE COMPARE A */
        #define VECTOR_NUMBER_GPT7_CAPTURE_COMPARE_B ((IRQn_Type) 15) /* GPT7 CAPTURE COMPARE B */
        #define GPT7_CAPTURE_COMPARE_B_IRQn          ((IRQn_Type) 15) /* GPT7 CAPTURE COMPARE B */
        #define VECTOR_NUMBER_GPT7_COUNTER_OVERFLOW ((IRQn_Type) 16) /* GPT7 COUNTER OVERFLOW */
        #define GPT7_COUNTER_OVERFLOW_IRQn          ((IRQn_Type) 16) /* GPT7 COUNTER OVERFLOW */
        /* [17]-[20]: the limb-bus controller UART (SCI9), Board/limb_uart.c.
         * Added by hand -- no RASC instance exists for SCI9 in this project;
         * see that file's header comment and ra_gen/vector_data.c. */
        #define VECTOR_NUMBER_SCI9_RXI ((IRQn_Type) 17) /* SCI9 RXI (Receive data full) */
        #define SCI9_RXI_IRQn          ((IRQn_Type) 17) /* SCI9 RXI (Receive data full) */
        #define VECTOR_NUMBER_SCI9_TXI ((IRQn_Type) 18) /* SCI9 TXI (Transmit data empty) */
        #define SCI9_TXI_IRQn          ((IRQn_Type) 18) /* SCI9 TXI (Transmit data empty) */
        #define VECTOR_NUMBER_SCI9_TEI ((IRQn_Type) 19) /* SCI9 TEI (Transmit end) */
        #define SCI9_TEI_IRQn          ((IRQn_Type) 19) /* SCI9 TEI (Transmit end) */
        #define VECTOR_NUMBER_SCI9_ERI ((IRQn_Type) 20) /* SCI9 ERI (Receive error) */
        #define SCI9_ERI_IRQn          ((IRQn_Type) 20) /* SCI9 ERI (Receive error) */
        /* [21]: ICU IRQ3 on P110 (D12, limb-bus RX) as a plain edge-count
         * input, Board/edge_test.c -- bypasses SCI9 framing entirely for
         * wire-margin testing. Added by hand, same reason as SCI9 above. */
        #define VECTOR_NUMBER_ICU_IRQ3 ((IRQn_Type) 21) /* ICU IRQ3 (External pin interrupt 3) */
        #define ICU_IRQ3_IRQn          ((IRQn_Type) 21) /* ICU IRQ3 (External pin interrupt 3) */
        /* The number of entries required for the ICU vector table. */
        #define BSP_ICU_VECTOR_NUM_ENTRIES (22)

        #ifdef __cplusplus
        }
        #endif
        #endif /* VECTOR_DATA_H */