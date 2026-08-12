// Board/limb_io_cli.c — see limb_io_cli.h. Thin CLI stack-word wrappers
// over Board/limb_io; no I2C or hardware knowledge lives here.

#include "limb_io_cli.h"
#include "limb_io.h"
#include "mpu6050.h"
#include "i2c1.h"
#include "cli.h"
#include "printers.h"

// ADS1015 single-ended code (0-2047, +-4.096V full scale, ~2mV/count) ->
// millivolts, for human-readable status only (limb_io itself only ever
// deals in raw codes — see ads1015.h).
static uint32_t adc_code_to_mv(uint16_t code) {
    return (uint32_t)code * 4096U / 2048U;
}

void limb_io_cli_status(void) {
    print("limb_io:"); printCr();
    for (uint8_t limb = 0; limb < LIMB_IO_LIMBS; limb++) {
        print("  limb "); printDec(limb);
        print("  adc_fail="); printDec(limb_io_adc_fail_count(limb));
        print("  dac_fail="); printDec(limb_io_dac_fail_count(limb));
        printCr();
        for (uint8_t sub = 0; sub < 4U; sub++) {
            uint8_t ch = (uint8_t)(limb * 4U + sub);
            print("    ch "); printDec(ch);
            print("  adc="); printDec(limb_io_get_adc(ch));
            print(" ("); printDec((Cell)adc_code_to_mv(limb_io_get_adc(ch))); print("mV)");
            print("  dac="); printDec(limb_io_get_dac(ch));
            printCr();
        }
    }

    print("  imu_fail="); printDec(limb_io_imu_fail_count()); printCr();
    mpu6050_data_t imu;
    if (limb_io_get_imu(&imu)) {
        print("  accel "); printDec(imu.accel_x); print(" "); printDec(imu.accel_y); print(" "); printDec(imu.accel_z); printCr();
        print("  gyro  "); printDec(imu.gyro_x);  print(" "); printDec(imu.gyro_y);  print(" "); printDec(imu.gyro_z);  printCr();
        print("  temp  "); printFloat0(mpu6050_temp_c(imu.temp_raw), 1); print(" C"); printCr();
    } else {
        print("  imu: no reading yet"); printCr();
    }
}

void limb_io_cli_dac_set(void) {   // ( code ch -- )
    Cell ch   = ret();
    Cell code = ret();
    if (code < 0)    code = 0;
    if (code > 4095) code = 4095;
    if (!limb_io_set_dac((uint8_t)ch, (uint16_t)code)) {
        print("limbio: dac write failed"); printCr();
    }
}

void limb_io_cli_assign(void) {
    for (uint8_t limb = 0; limb < LIMB_IO_LIMBS; limb++) {
        print("limb "); printDec(limb); print(": ");
        print(limb_io_assign_one(limb) ? "ok" : "FAILED");
        printCr();
    }
}

void limb_io_cli_assign_one(void) {   // ( limb -- )
    Cell limb = ret();
    print("limb "); printDec(limb); print(": ");
    print(limb_io_assign_one((uint8_t)limb) ? "ok" : "FAILED");
    printCr();
}

void limb_io_cli_reset_one(void) {   // ( limb -- )
    Cell limb = ret();
    print("limb "); printDec(limb); print(" -> 0x60: ");
    print(limb_io_reset_one((uint8_t)limb) ? "ok" : "FAILED");
    printCr();
}

// i2cscan — probe every valid 7-bit address (0x08-0x77; 0x00-0x07 and
// 0x78-0x7F are reserved by the I2C spec) and print the ones that ACK.
void i2c1_cli_scan(void) {
    print("scanning IIC1 (0x08-0x77)..."); printCr();
    uint8_t found = 0;
    for (uint8_t addr = 0x08U; addr <= 0x77U; addr++) {
        if (i2c1_probe(addr)) {
            print("  found"); printHex2(addr); printCr();
            found++;
        }
    }
    print("scan done, "); printDec(found); print(" device(s)"); printCr();
}

void limb_io_cli_isolated_read(void) {   // ( limb count -- )
    Cell count = ret();
    Cell limb  = ret();
    if (limb < 0 || limb >= LIMB_IO_LIMBS) {
        print("i2ctest: bad limb"); printCr();
        return;
    }
    limb_io_isolated_read_ch0((uint8_t)limb, (uint8_t)count);
}

void limb_io_cli_debug_ch0(void) {   // ( limb -- )
    Cell limb = ret();
    if (limb < 0 || limb >= LIMB_IO_LIMBS) {
        print("i2cregs: bad limb"); printCr();
        return;
    }
    limb_io_debug_ch0((uint8_t)limb);
}
