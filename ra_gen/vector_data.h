/* generated vector header file - do not edit */
        #ifndef VECTOR_DATA_H
        #define VECTOR_DATA_H
        #ifdef __cplusplus
        extern "C" {
        #endif
                /* Number of interrupts allocated */
        #ifndef VECTOR_DATA_IRQ_COUNT
        #define VECTOR_DATA_IRQ_COUNT    (11)
        #endif
        /* ISR prototypes */
        void sci_uart_rxi_isr(void);
        void sci_uart_txi_isr(void);
        void sci_uart_tei_isr(void);
        void sci_uart_eri_isr(void);
        void rtc_carry_isr(void);
        void gpt_counter_overflow_isr(void);
        void usbfs_int_isr(void);
        void iic_master_rxi_isr(void);
        void iic_master_txi_isr(void);
        void iic_master_tei_isr(void);
        void iic_master_eri_isr(void);

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
        #define VECTOR_NUMBER_IIC0_RXI ((IRQn_Type) 7) /* IIC0 RXI (Receive data full) */
        #define IIC0_RXI_IRQn          ((IRQn_Type) 7) /* IIC0 RXI (Receive data full) */
        #define VECTOR_NUMBER_IIC0_TXI ((IRQn_Type) 8) /* IIC0 TXI (Transmit data empty) */
        #define IIC0_TXI_IRQn          ((IRQn_Type) 8) /* IIC0 TXI (Transmit data empty) */
        #define VECTOR_NUMBER_IIC0_TEI ((IRQn_Type) 9) /* IIC0 TEI (Transmit end) */
        #define IIC0_TEI_IRQn          ((IRQn_Type) 9) /* IIC0 TEI (Transmit end) */
        #define VECTOR_NUMBER_IIC0_ERI ((IRQn_Type) 10) /* IIC0 ERI (Transfer error) */
        #define IIC0_ERI_IRQn          ((IRQn_Type) 10) /* IIC0 ERI (Transfer error) */
        /* The number of entries required for the ICU vector table. */
        #define BSP_ICU_VECTOR_NUM_ENTRIES (11)

        #ifdef __cplusplus
        }
        #endif
        #endif /* VECTOR_DATA_H */