// Board/limb_fw_test.h — see limb_fw_test.c.
#ifndef BOARD_LIMB_FW_TEST_H
#define BOARD_LIMB_FW_TEST_H

// limb_fw_test_cli — CLI word: ( -- ) streams a small synthetic test
// image to the limb-bus node via MAINT_FW_BEGIN/CHUNK/END and reports
// pass/fail. Inert (prints "not built" and does nothing else) unless
// LIMB_UART_DEV.
void limb_fw_test_cli(void);

#endif // BOARD_LIMB_FW_TEST_H
