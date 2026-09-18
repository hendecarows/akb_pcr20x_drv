//
//  akb_pcr20x_fw.c
//  AKB-PCR20X用ファームウェア
//
//  移植元: fifo.c (オリジナル付属CD)
//  移植先: fx2lib (https://github.com/djmuhlestein/fx2lib)
//
#include <fx2macros.h>
#include <fx2regs.h>
#include <fx2ints.h>
#include <delay.h>
#include <setupdat.h>
#include <i2c.h>

#define SYNCDELAY SYNCDELAY4

// ベンダーコマンド定義
#define MODE_IDLE       (0<<6)
#define MODE_REGR       (1<<6)
#define MODE_REGW       (2<<6)
#define MODE_START      (3<<6)

#define CMD_EP6IN_START     0x50
#define CMD_EP6IN_STOP      0x51
#define CMD_EP2OUT_START    0x52
#define CMD_EP2OUT_STOP     0x53
#define CMD_PORT_CFG        0x54
#define CMD_REG_READ        0x55
#define CMD_REG_WRITE       0x56
#define CMD_PORT_READ       0x57
#define CMD_PORT_WRITE      0x58
#define CMD_IFCONFIG        0x59
#define CMD_MODE_IDLE       0x5a
#define CMD_EP4IN_START     0x5b
#define CMD_EP4IN_STOP      0x5c
#define CMD_I2C_READ        0x60   // adrs,len (最大16byte)
#define CMD_I2C_WRITE       0x61   // adrs,len,data... (最大16byte)
#define CMD_I2C_STATUS      0x62   // 引数なし。診断カウンタ12byte(WORDx6)を返す

#define I2C_BUF_LEN 16
static __xdata BYTE i2cbf[I2C_BUF_LEN];

static BYTE val_ifconfig;
static BYTE addr_mask;
static BYTE g_configuration = 1;
static BYTE g_alt_setting = 0;

// i2c.c 側で定義している診断カウンタ。
extern __xdata WORD i2c_berr_read_cnt;
extern __xdata WORD i2c_berr_write_cnt;
extern __xdata WORD i2c_timeout_read_cnt;
extern __xdata WORD i2c_timeout_write_cnt;

// このファイル内で数える、i2c_read/i2c_writeがFALSEを返した回数
static __xdata WORD i2c_read_errcnt = 0;
static __xdata WORD i2c_write_errcnt = 0;

void main_init(void)
{
    SETCPUFREQ(CLK_48M);   // CPUクロック48MHz (fx2lib delay.h のマクロ)

    // FDバスを8bitに設定
    EP2FIFOCFG = 0x04; SYNCDELAY;
    EP4FIFOCFG = 0x04; SYNCDELAY;
    EP6FIFOCFG = 0x04; SYNCDELAY;
    EP8FIFOCFG = 0x04; SYNCDELAY;

    EP1OUTCFG = 0xA0;
    EP1INCFG  = 0xA0;
    SYNCDELAY;
    EP1OUTBC = 0;
    REVCTL = 0x03; SYNCDELAY;

    EP2CFG = 0xA2;  // OUT, Size=512, buf=Quad, BULK
    SYNCDELAY;
    EP4CFG = 0xE2;  // IN,  Size=512, buf=Quad, BULK
    SYNCDELAY;
    EP6CFG = 0xE0;  // IN,  Size=512, buf=Quad, BULK
    SYNCDELAY;
    EP8CFG = 0x00;
    SYNCDELAY;

    FIFOPINPOLAR = 0x00; SYNCDELAY;
    EP4AUTOINLENH = 0x02; SYNCDELAY;
    EP4AUTOINLENL = 0x00; SYNCDELAY;
    EP6AUTOINLENH = 0x02; SYNCDELAY;
    EP6AUTOINLENL = 0x00; SYNCDELAY;

    FIFORESET = 0x80; SYNCDELAY;  // NAK-ALL 有効化 (競合防止)
    FIFORESET = 0x02; SYNCDELAY;  // FIFO2 リセット
    FIFORESET = 0x04; SYNCDELAY;  // FIFO4 リセット
    FIFORESET = 0x06; SYNCDELAY;  // FIFO6 リセット
    FIFORESET = 0x08; SYNCDELAY;  // FIFO8 リセット
    FIFORESET = 0x00; SYNCDELAY;  // NAK-ALL 解除

    AUTOPTRSETUP = 0x07;   // デュアルオートポインタ + インクリメント有効化

    IOD = 0x00;   // MODE_IDLE
    OED = 0xc0;   // MODE制御ピンを出力に
    PINFLAGSAB = 0xe8;  // FLAGA=ep2EF FLAGB=ep6FF
    SYNCDELAY;

    addr_mask = 0;
    val_ifconfig = 0;
}

void main_loop(void)
{
    BYTE i;
    BYTE cmd_cnt, ret_cnt, len;
    BYTE adrs;
    BOOL ret;

    if (!(EP1OUTCS & bmEPBUSY)) {
        // 前回のEP1IN応答がまだホストに送信し終えていない場合は
        // 今回のポーリングをスキップする (EOVERFLOW対策)
        if (EP1INCS & bmEPBUSY) {
            return;
        }

        cmd_cnt = EP1OUTBC;
        AUTOPTRH1 = MSB(&EP1OUTBUF);
        AUTOPTRL1 = LSB(&EP1OUTBUF);
        AUTOPTRH2 = MSB(&EP1INBUF);
        AUTOPTRL2 = LSB(&EP1INBUF);
        ret_cnt = 0;

        for (;;) {
            if (cmd_cnt-- == 0) {
                break;
            }

            switch (XAUTODAT1) {
            case CMD_I2C_READ:
                if (cmd_cnt < 2) {
                    cmd_cnt = 0;
                    break;
                }
                adrs = XAUTODAT1;
                len  = XAUTODAT1;
                cmd_cnt -= 2;

                if (len > I2C_BUF_LEN) {
                    cmd_cnt = 0;
                    break;
                }

                ret = i2c_read(adrs, len, i2cbf);

                if (ret) {
                    for (i = 0; i < len; i++) {
                        XAUTODAT2 = i2cbf[i];
                        ret_cnt++;
                    }
                } else {
                    i2c_read_errcnt++;
                }
                break;

            case CMD_I2C_WRITE:
                if (cmd_cnt < 2) {
                    cmd_cnt = 0;
                    break;
                }
                adrs = XAUTODAT1;
                len  = XAUTODAT1;
                cmd_cnt -= 2;

                if (len > I2C_BUF_LEN || cmd_cnt < len) {
                    cmd_cnt = 0;
                    break;
                }

                for (i = 0; i < len; i++) {
                    i2cbf[i] = XAUTODAT1;
                    cmd_cnt--;
                }

                ret = i2c_write(adrs, len, i2cbf, 0, NULL);
                if (!ret) {
                    i2c_write_errcnt++;
                }

                break;

            case CMD_I2C_STATUS:
                // 診断カウンタ12byte(WORD x6, MSBファースト)を返す
                XAUTODAT2 = MSB(i2c_read_errcnt);
                XAUTODAT2 = LSB(i2c_read_errcnt);
                XAUTODAT2 = MSB(i2c_write_errcnt);
                XAUTODAT2 = LSB(i2c_write_errcnt);
                XAUTODAT2 = MSB(i2c_berr_read_cnt);
                XAUTODAT2 = LSB(i2c_berr_read_cnt);
                XAUTODAT2 = MSB(i2c_berr_write_cnt);
                XAUTODAT2 = LSB(i2c_berr_write_cnt);
                XAUTODAT2 = MSB(i2c_timeout_read_cnt);
                XAUTODAT2 = LSB(i2c_timeout_read_cnt);
                XAUTODAT2 = MSB(i2c_timeout_write_cnt);
                XAUTODAT2 = LSB(i2c_timeout_write_cnt);
                ret_cnt += 12;
                break;

            case CMD_REG_WRITE:
                if (cmd_cnt < 2) {
                    cmd_cnt = 0;
                    break;
                }
                IOD = MODE_REGW | (IOD & addr_mask) | XAUTODAT1;
                IFCONFIG = 0xE0;
                OEB = 0xff;
                IOB = XAUTODAT1;
                cmd_cnt -= 2;
                break;

            case CMD_REG_READ:
                if (cmd_cnt < 1) {
                    cmd_cnt = 0;
                    break;
                }
                IOD = MODE_REGR | (IOD & addr_mask) | XAUTODAT1;
                IFCONFIG = 0xE0;
                OEB = 0x00;
                XAUTODAT2 = IOB;
                ret_cnt++;
                cmd_cnt -= 1;
                break;

            case CMD_PORT_WRITE:
                if (cmd_cnt < 1) {
                    cmd_cnt = 0;
                    break;
                }
                IOD = (IOD & ~addr_mask) | XAUTODAT1;
                cmd_cnt -= 1;
                break;

            case CMD_PORT_READ:
                XAUTODAT2 = IOD;
                ret_cnt++;
                break;

            case CMD_PORT_CFG:
                if (cmd_cnt < 2) {
                    cmd_cnt = 0;
                    break;
                }
                addr_mask = (XAUTODAT1 | 0xc0);
                OED = XAUTODAT1 | addr_mask;
                addr_mask = ~addr_mask;
                cmd_cnt -= 2;
                break;

            case CMD_IFCONFIG:
                if (cmd_cnt < 1) {
                    cmd_cnt = 0;
                    break;
                }
                val_ifconfig = XAUTODAT1;
                cmd_cnt -= 1;
                break;

            case CMD_EP6IN_START:
                IFCONFIG = val_ifconfig;
                FIFORESET = 0x06; SYNCDELAY;
                EP6FIFOCFG = 0x0C; SYNCDELAY;
                IOD = MODE_START | (IOD & 0x3f);
                break;

            case CMD_EP6IN_STOP:
                EP6FIFOCFG = 0x04;
                break;

            case CMD_EP4IN_START:
                IFCONFIG = val_ifconfig;
                FIFORESET = 0x04; SYNCDELAY;
                EP4FIFOCFG = 0x0C; SYNCDELAY;
                IOD = MODE_START | (IOD & 0x3f);
                break;

            case CMD_EP4IN_STOP:
                EP4FIFOCFG = 0x04;
                break;

            case CMD_EP2OUT_START:
                IFCONFIG = val_ifconfig;
                FIFORESET = 0x02; SYNCDELAY;
                OUTPKTEND = 0x82; SYNCDELAY;
                OUTPKTEND = 0x82; SYNCDELAY;
                OUTPKTEND = 0x82; SYNCDELAY;
                OUTPKTEND = 0x82; SYNCDELAY;
                EP2FIFOCFG = 0x10; SYNCDELAY;
                IOD = MODE_START | (IOD & 0x3f);
                break;

            case CMD_EP2OUT_STOP:
                EP2FIFOCFG = 0x04;
                break;

            case CMD_MODE_IDLE:
                IOD = MODE_IDLE | (IOD & 0x3f);
                break;
            }
        }

        EP1OUTBC = 0;
        if (ret_cnt) {
            EP1INBC = ret_cnt;
        }
    }
}

// 独自ベンダーコマンドは使用しない (元コードの VR_NAKALL_ON/OFF は
// FIFORESETのNAKALLビット直接操作で代替可能なため今回は省略)
BOOL handle_vendorcommand(BYTE cmd)
{
    (void)cmd;
    return FALSE;
}

// 単一インターフェース(0)のみサポート
BOOL handle_get_interface(BYTE ifc, BYTE* alt_ifc)
{
    if (ifc != 0) return FALSE;
    *alt_ifc = g_alt_setting;
    return TRUE;
}

BOOL handle_set_interface(BYTE ifc, BYTE alt_ifc)
{
    if (ifc != 0) return FALSE;
    g_alt_setting = alt_ifc;
    return TRUE;
}

BYTE handle_get_configuration(void)
{
    return g_configuration;
}

BOOL handle_set_configuration(BYTE cfg)
{
    g_configuration = cfg;
    return TRUE;
}

// エンドポイントのトグルリセット等が必要な場合はここに実装する。
// 今回のプロトコルはEP1(制御)/EP2/EP4/EP6のみで、SetInterfaceの
// 頻度も低いため最小実装とする。
void handle_reset_ep(BYTE ep)
{
    (void)ep;
}

// カスタムディスクリプタは使用しないため、標準処理(dscr.a51)に委ねる
BOOL handle_get_descriptor(void)
{
    return FALSE;
}
