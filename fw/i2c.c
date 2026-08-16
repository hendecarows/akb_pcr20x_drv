/**
 * Copyright (C) 2009 Ubixum, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 **/

/*
 * オリジナル(fx2lib)からの変更点:
 *
 *  1. BERR(バスエラー)発生時のリトライ回数の上限(I2C_BERR_MAX_RETRY)を設定。
 *  2. bmDONE待ちループにも上限(I2C_WAIT_LIMIT)を設けた。
 *  3. 診断用カウンタを追加:
 *       i2c_berr_read_cnt / i2c_berr_write_cnt
 *       BERR発生回数 (リトライで最終的に成功した分も含む)
 *       i2c_timeout_read_cnt / i2c_timeout_write_cnt
 *       上記の上限に達し、FALSEで強制的に諦めた回数
 */

#include <fx2regs.h>
#include <fx2macros.h>
#include <i2c.h>
#include <delay.h>

#define i2c_printf(...)

volatile __xdata BOOL cancel_i2c_trans;
#define CHECK_I2C_CANCEL() if (cancel_i2c_trans) return FALSE

// BERRリトライの上限回数
#define I2C_BERR_MAX_RETRY   20

// bmDONE待ちループの上限
#define I2C_WAIT_LIMIT       60000U

// 診断用カウンタ (main_loop側の CMD_I2C_STATUS から参照する)
__xdata WORD i2c_berr_read_cnt = 0;
__xdata WORD i2c_berr_write_cnt = 0;
__xdata WORD i2c_timeout_read_cnt = 0;
__xdata WORD i2c_timeout_write_cnt = 0;

// bmDONEが立つまで待つ
static BOOL i2c_wait_done(void)
{
    WORD limit = I2C_WAIT_LIMIT;
    while (!(I2CS & bmDONE) && !cancel_i2c_trans) {
        if (--limit == 0) return FALSE;
    }
    return !cancel_i2c_trans;
}

// bmSTOPが下がるまで待つ
static void i2c_wait_stop(void)
{
    WORD limit = I2C_WAIT_LIMIT;
    while ((I2CS & bmSTOP) && !cancel_i2c_trans) {
        if (--limit == 0) break;
    }
}

BOOL i2c_write ( BYTE addr, WORD len, BYTE *addr_buf, WORD len2, BYTE* data_buf ) {

    WORD cur_byte;
    WORD total_bytes = len+len2;
    BYTE retry_count = 2;
    BYTE berr_retry = 0;
    cancel_i2c_trans = FALSE;

    step1:
        CHECK_I2C_CANCEL();
        cur_byte = 0;
        I2CS |= bmSTART;
        if (I2CS & bmBERR) {
            i2c_berr_write_cnt++;
            if (++berr_retry >= I2C_BERR_MAX_RETRY) {
                i2c_timeout_write_cnt++;
                return FALSE;
            }
            i2c_printf("Woops.. need to do the timer\n");
            delay(10);
            goto step1;
        }


        I2DAT = addr << 1;

        if (!i2c_wait_done()) {
            i2c_timeout_write_cnt++;
            return FALSE;
        }
        if (I2CS&bmBERR) {
            i2c_berr_write_cnt++;
            if (++berr_retry >= I2C_BERR_MAX_RETRY) {
                i2c_timeout_write_cnt++;
                return FALSE;
            }
            i2c_printf("bmBERR, going to step 1\n");
            goto step1;
        }


    if (!(I2CS & bmACK)) {
        I2CS |= bmSTOP;
        i2c_wait_stop();
        CHECK_I2C_CANCEL();
        --retry_count;
        if (!retry_count) {
            i2c_printf("No ack after writing address.! Fail\n");
            return FALSE;
        }
        delay(10);
        goto step1;
    }

    while (cur_byte < total_bytes) {
        I2DAT = cur_byte < len ? addr_buf[cur_byte] : data_buf[cur_byte-len];
        ++cur_byte;
        if (!i2c_wait_done()) {
            i2c_timeout_write_cnt++;
            return FALSE;
        }
        if (I2CS&bmBERR) {
         i2c_berr_write_cnt++;
         if (++berr_retry >= I2C_BERR_MAX_RETRY) {
             i2c_timeout_write_cnt++;
             return FALSE;
         }
         i2c_printf("bmBERR on byte %d. Going to step 1\n" , cur_byte-1);
         goto step1;
        }
        if (!(I2CS & bmACK)) {
            I2CS |= bmSTOP;
            i2c_wait_stop();
            i2c_printf("No Ack after byte %d. Fail\n", cur_byte-1);
            return FALSE;
        }
    }

    I2CS |= bmSTOP;
    i2c_wait_stop();
    CHECK_I2C_CANCEL();

    return TRUE;

}

BOOL i2c_read( BYTE addr, WORD len, BYTE* buf) {

    BYTE tmp;
    WORD cur_byte;
    BYTE berr_retry = 0;
    cancel_i2c_trans = FALSE;

    start:
        CHECK_I2C_CANCEL();
        cur_byte = 0;

        I2CS |= bmSTART;
        if (I2CS & bmBERR) {
            i2c_berr_read_cnt++;
            if (++berr_retry >= I2C_BERR_MAX_RETRY) {
                i2c_timeout_read_cnt++;
                return FALSE;
            }
            i2c_printf("Woops, step1 BERR, need to do timeout\n");
            delay(10);
            goto start;
        }

        I2DAT = (addr << 1) | 1;

        if (!i2c_wait_done()) {
            i2c_timeout_read_cnt++;
            return FALSE;
        }
        if (I2CS & bmBERR) {
            i2c_berr_read_cnt++;
            if (++berr_retry >= I2C_BERR_MAX_RETRY) {
                i2c_timeout_read_cnt++;
                return FALSE;
            }
            goto start;
        }

        if (!(I2CS&bmACK)) {
            I2CS |= bmSTOP;
            i2c_wait_stop();
            return FALSE;
        }

    if (len == 1) {
        I2CS |= bmLASTRD;
    }

    tmp = I2DAT;

    while (len>cur_byte+1) {

        if (!i2c_wait_done()) {
            i2c_timeout_read_cnt++;
            return FALSE;
        }
        if (I2CS&bmBERR) {
            i2c_berr_read_cnt++;
            if (++berr_retry >= I2C_BERR_MAX_RETRY) {
                i2c_timeout_read_cnt++;
                return FALSE;
            }
            goto start;
        }

        if (len==cur_byte+2) {
            I2CS |= bmLASTRD;
        }

        buf[cur_byte++] = I2DAT;
    }

    if (!i2c_wait_done()) {
        i2c_timeout_read_cnt++;
        return FALSE;
    }
    if (I2CS&bmBERR) {
        i2c_berr_read_cnt++;
        if (++berr_retry >= I2C_BERR_MAX_RETRY) {
            i2c_timeout_read_cnt++;
            return FALSE;
        }
        goto start;
    }
    I2CS |= bmSTOP;
    buf[cur_byte] = I2DAT;

    i2c_wait_stop();
    CHECK_I2C_CANCEL();

    return TRUE;
}


BOOL eeprom_write(BYTE prom_addr, WORD addr, WORD length, BYTE* buf) {
    BYTE addr_len = 0;
    BYTE data_buffer[3];
    WORD cur_byte = 0;

    while (cur_byte<length) {
        addr_len = 0;
        if (EEPROM_TWO_BYTE) {
            data_buffer[addr_len++] = MSB(addr);
        }
        data_buffer[addr_len++] = LSB(addr);
        data_buffer[addr_len++] = buf[cur_byte++];

        if (!i2c_write(prom_addr, addr_len, data_buffer, 0, NULL)) return FALSE;
        ++addr;
    }

    return TRUE;
}


BOOL eeprom_read(BYTE prom_addr, WORD addr, WORD length, BYTE *buf)
{
    BYTE eeprom_addr[2];
    BYTE addr_len = 0;
    if (EEPROM_TWO_BYTE) {
        eeprom_addr[addr_len++] = MSB(addr);
    }

    eeprom_addr[addr_len++] = LSB(addr);

    if (!i2c_write(prom_addr, addr_len, eeprom_addr, 0, NULL)) return FALSE;
    if (!i2c_read(prom_addr, length, buf)) return FALSE;

    return TRUE;
}
