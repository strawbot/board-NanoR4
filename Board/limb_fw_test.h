// Board/limb_fw_test.h — see limb_fw_test.c.
#ifndef BOARD_LIMB_FW_TEST_H
#define BOARD_LIMB_FW_TEST_H

// limb_fw_test_cli — CLI word: ( -- ) streams a small synthetic test
// image to the limb-bus node via MAINT_FW_BEGIN/CHUNK/END and reports
// pass/fail. Inert (prints "not built" and does nothing else) unless
// LIMB_UART_DEV.
void limb_fw_test_cli(void);

// limb_fw_redpath_cli — CLI word: ( -- ) simulates a controller reboot
// mid-transfer (a second BEGIN with no abort/reset message) and
// confirms the node recovers cleanly while rejecting the stale
// pre-reboot session's token. See limb_fw_test.c's own comment.
void limb_fw_redpath_cli(void);

// limb_fw_timeout_cli — CLI word: ( -- ) BEGINs a session, then goes
// quiet for longer than the node's idle timeout, then confirms the node
// has abandoned the session. Takes about 2.7 seconds to run.
void limb_fw_timeout_cli(void);

#endif // BOARD_LIMB_FW_TEST_H
