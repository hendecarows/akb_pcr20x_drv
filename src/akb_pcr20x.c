// SPDX-License-Identifier: GPL-2.0

#include <linux/limits.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/usb.h>

#include "dvb_usb.h"
#include "tc90522.h"
#include "stv6110x.h"
#include "mxl5007t.h"

#define MAX_I2C_BUFFER_SIZE 64

enum akb_pcr20x_constans {
	FX2_REG_CPU_RESET = 0xe600,
	FX2_REQ_FIRMWARE_DOWNLOAD = 0xa0,
	FX2_CTRL_ENDPOINT = 0,
	FX2_CTRL_REQUEST_TYPE = USB_RECIP_DEVICE | USB_DIR_OUT | USB_TYPE_VENDOR,
	FX2_CTRL_TIMEOUT = 2000,
    FX2_EP6IN_START = 0x50,
    FX2_EP6IN_STOP = 0x51,
	FX2_PORT_CFG = 0x54,
    FX2_PORT_WRITE = 0x58,
	FX2_IFCONFIG = 0x59,
	FX2_MODE_IDLE = 0x5a,
	FX2_I2C_READ = 0x60,
	FX2_I2C_WRITE = 0x61,
	FX2_PIO_START = 0x20,
};

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

struct akb_pcr20x {
	u8 wbuf[MAX_I2C_BUFFER_SIZE];
	u8 rbuf[MAX_I2C_BUFFER_SIZE];
	struct mutex mutex_fe;
	struct dvb_frontend *active_fe;
	u32 open_count;
	struct address {
		u8 demod_s;
		u8 tuner_s;
		u8 demod_t;
		u8 tuner_t;
	} i2c_addr;
	struct client {
		struct i2c_client *demod_s;
		struct i2c_client *tuner_s;
		struct i2c_client *demod_t;
		struct i2c_client *tuner_t;
	} i2c_clt;
	struct config {
		struct tc90522_config demod_s;
		struct stv6110x_config tuner_s;
		struct tc90522_config demod_t;
		struct mxl5007t_config tuner_t;
	} config;
	int (*set_frontend_s)(struct dvb_frontend *fe);
	int (*set_frontend_t)(struct dvb_frontend *fe);
};

static struct dvb_usb_device_properties akb_pcr20x_props;

struct akb_pcr20x_reg_val {
	u8 reg;
	u8 val;
};


static int akb_pcr20x_usb_control_write(struct dvb_usb_device *d, u8 request, u16 value, u16 index, const void *data,
										u16 size)
{
	struct usb_device *udev = d->udev;

	dvb_usb_dbg_usb_control_msg(udev, request, FX2_CTRL_REQUEST_TYPE, value, index, data, size);
	int ret = usb_control_msg_send(udev, FX2_CTRL_ENDPOINT, request, FX2_CTRL_REQUEST_TYPE, value, index, data, size,
								   FX2_CTRL_TIMEOUT, GFP_KERNEL);

	if (ret < 0) {
		dev_err(&udev->dev, "%s: usb control write failed %d: %02x %2x %04x %04x %d : %*ph\n", __func__, ret, request,
				FX2_CTRL_REQUEST_TYPE, value, index, size, size, data);
		return ret;
	}

	return 0;
}

static int akb_pcr20x_fx2_cpu_run(struct dvb_usb_device *d)
{
	dev_dbg(&d->udev->dev, "%s\n", __func__);

	u8 command = 0;
	return akb_pcr20x_usb_control_write(d, FX2_REQ_FIRMWARE_DOWNLOAD, FX2_REG_CPU_RESET, 0, &command, 1);
}

static int akb_pcr20x_fx2_cpu_stop(struct dvb_usb_device *d)
{
	dev_dbg(&d->udev->dev, "%s\n", __func__);

	u8 command = 1;
	return akb_pcr20x_usb_control_write(d, FX2_REQ_FIRMWARE_DOWNLOAD, FX2_REG_CPU_RESET, 0, &command, 1);
}

static int akb_pcr20x_download_firmware(struct dvb_usb_device *d, const struct firmware *fw)
{
	dev_dbg(&d->udev->dev, "%s\n", __func__);

	struct device *dev = &d->udev->dev;
	int ret = 0;

	u8 dev_id = d->udev->descriptor.bcdDevice >> 8;
	if (dev_id == 0xff) {
		dev_id = d->udev->descriptor.bcdDevice & 0xff;
	} else {
		dev_id = 0;
	}
	dev_dbg(dev, "%s: device id: %02x.\n", __func__, dev_id);

	// ファームウェアの最低サイズ
	// ヘッダ + データサイズ + アドレス + データ : 8 + 2 + 2 + 1 = 13
	dev_dbg(dev, "%s: firmware size: %zu\n", __func__, fw->size);
	if (fw->size < 13) {
		return -EINVAL;
	}

	u8 *buf = kmalloc(fw->size, GFP_KERNEL);
	if (!buf) {
		dev_err(dev, "%s: kmalloc(fw->size: %zu) failed.\n", __func__, fw->size);
		return -ENOMEM;
	}
	memcpy(buf, fw->data, fw->size);

	// FX2 の CPU 停止
	ret = akb_pcr20x_fx2_cpu_stop(d);
	if (ret < 0) {
		goto fail;
	}

	// ファームウェア形式
	//
	// c2 47 05 31 21 00 00 04 : 先頭から8バイトはスキップ
	// 00 03 : データサイズ (2bytes)
	// 00 00 : アドレス (2bytes)
	// 02 09 3f : データ (データサイズbytes)
	// ...
	// 80 01 : データサイズの先頭ビットが1なら終端
	// e6 00 : アドレス (2bytes)
	// 00 : アドレスが 0xe600 でデータが 0x00 の場合 CPU を起動

	u16 value = 0;
	u16 index = 0;
	u16 size = 0;
	const u8 *end = buf + fw->size;
	bool cpu_run = false;
	u8 *p = buf + 8; // 先頭から 8 バイトをスキップしてデータサイズに移動
	size_t remain = end - p;
	int len = 0;

	while (remain >= 4) {
		len = (p[0] << 8) | p[1];
		value = (p[2] << 8) | p[3];
		size = len & 0x7fff;

		if (remain < 4 + (size_t)size) {
			dev_err(dev, "%s: firmware truncated.\n", __func__);
			ret = -EINVAL;
			goto fail;
		}

		p += 4; // データサイズとアドレスをスキップしてデータに移動
		ret = akb_pcr20x_usb_control_write(d, FX2_REQ_FIRMWARE_DOWNLOAD, value, index, p, size);
		if (ret < 0) {
			goto fail;
		}

		if (len & 0x8000) {
			// データサイズの先頭ビットが1の場合は終端データ
			if (size == 1 && value == FX2_REG_CPU_RESET && p[0] == 0) {
				// 終端データがアドレス 0xe600 でデータが 0 の場合は CPU 起動コマンドを実行済み
				cpu_run = true;
			}
			ret = 0;
			break;
		}

		p += size;
		remain = end - p;
	}

	if (ret == 0 && cpu_run == false) {
		// CPU が起動コマンドを実行していない場合は実行
		ret = akb_pcr20x_fx2_cpu_run(d);
		if (ret < 0) {
			goto fail;
		}
	}

	msleep(100);

fail:
	kfree(buf);
	return ret;
}

static int akb_pcr20x_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct dvb_usb_device *d = i2c_get_adapdata(adap);
	struct akb_pcr20x *priv = d_to_priv(d);
	u8 *wbuf = priv->wbuf;
	u8 *rbuf = priv->rbuf;

	if (mutex_lock_interruptible(&d->i2c_mutex)) {
		return -ERESTARTSYS;
	}

	int ret = 0;
	for (int i = 0; i < num; i++) {
		u16 addr = msgs[i].addr;
		u16 len = msgs[i].len;
		u16 flags = msgs[i].flags;

		if (len == 0 || len > U8_MAX) {
			// I2C メッセージ長さが 0 の場合、u8 の最大を超える場合はサポートしない
			dev_err(&adap->dev, "%s: invalid i2c transfer length addr: %04x len: %u\n", __func__, addr, len);
			ret = -EOPNOTSUPP;
			goto fail;
		}

		if ((addr > U8_MAX) || (flags & I2C_M_TEN)) {
			// I2C アドレスが 8bit を超える場合はサポートしない
			dev_err(&adap->dev, "%s: invalid i2c address size addr: %04x len: %u\n", __func__, addr, len);
			ret = -EOPNOTSUPP;
			goto fail;
		}

		if ((flags & I2C_M_RD) != 0) {
			// I2C Read 単独はサポートしない
			// 必ず直前が I2C Write で、同じ I2C アドレスが必要
			dev_err(&adap->dev, "%s: standalone i2c read without prior i2c write is not supported addr: %02x len: %u\n",
					__func__, addr, len);
			ret = -EOPNOTSUPP;
			goto fail;
		}

		// I2C Read 単独は除外したので、I2C Write + Read か I2C Write 単独の場合

		if ((i + 1 < num) && (msgs[i + 1].flags & I2C_M_RD) && (addr == msgs[i + 1].addr)) {
			// 次の I2C メッセージが I2C Read で、同じアドレスの場合は I2C Write + Read として処理する

			// ISDB-T demodulator のアドレス 0x8b から 3 バイト読み取る
			// BW: 61 10 01 8b 60 10 03
			// BR: 00 0e 47

			// ISDB-S demodulator のアドレス 0xbc から 2 バイト読み取る
			// BW: 61 11 01 bc 60 11 02
			// BR: 41 df

			// [0] FX2_I2C_WRITE : 0x61
			// [1] I2C Write の I2C アドレス ISDB-T: 0x10, ISDB-S: 0x11
			// [2] I2C Write のデータ長
			// [3] レジスタアドレス
			// [4] FX2_I2C_READ : 0x60
			// [5] I2C Read のアドレス : [1] と同じ
			// [6] I2C Read のデータ長

			int rlen = msgs[i + 1].len;
			int wlen = 3 + len + 3;

			if (rlen == 0 || rlen > U8_MAX) {
				// I2C メッセージ長さが 0 の場合、u8 の最大を超える場合はサポートしない
				dev_err(&adap->dev, "%s: invalid i2c read length addr: %04x len: %d\n", __func__, addr, rlen);
				ret = -EOPNOTSUPP;
				goto fail;
			}

			if (rlen > MAX_I2C_BUFFER_SIZE) {
				// バッファサイズを超える場合はエラー
				dev_err(&adap->dev, "%s: i2c read length %d exceeds max %d\n", __func__, rlen, MAX_I2C_BUFFER_SIZE);
				ret = -EINVAL;
				goto fail;
			}

			if (wlen > MAX_I2C_BUFFER_SIZE) {
				// バッファサイズを超える場合はエラー
				dev_err(&adap->dev, "%s: i2c write length %d exceeds max %d\n", __func__, wlen, MAX_I2C_BUFFER_SIZE);
				ret = -EINVAL;
				goto fail;
			}

			wbuf[0] = FX2_I2C_WRITE;
			wbuf[1] = (u8)addr;
			wbuf[2] = (u8)len;
			memcpy(&wbuf[3], msgs[i].buf, len);
			wbuf[3 + len] = FX2_I2C_READ;
			wbuf[4 + len] = wbuf[1];
			wbuf[5 + len] = (u8)rlen;

			#ifdef DEBUG
			print_hex_dump_bytes("BW: ", DUMP_PREFIX_NONE, wbuf, wlen);
			#endif

			ret = dvb_usbv2_generic_rw(d, wbuf, wlen, rbuf, rlen);

			if (ret < 0) {
				dev_err(&adap->dev, "%s: dvb_usbv2_generic_rw failed: %d\n", __func__, ret);
				dev_err(&adap->dev, "%s: wlen = %d rlen = %d\n", __func__, wlen, rlen);
				print_hex_dump(KERN_ERR, "BW: ", DUMP_PREFIX_NONE, 16, 1, wbuf, wlen, true);
				print_hex_dump(KERN_ERR, "BR: ", DUMP_PREFIX_NONE, 16, 1, rbuf, rlen, true);
				if (ret == -EOVERFLOW) {
					dev_err(&adap->dev, "%s: babble detected, clearing halt\n", __func__);
					usb_clear_halt(d->udev, usb_rcvbulkpipe(d->udev, d->props->generic_bulk_ctrl_endpoint_response));
				}
				goto fail;
			}

			#ifdef DEBUG
			if (rlen > 0) {
				print_hex_dump_bytes("BR: ", DUMP_PREFIX_NONE, rbuf, rlen);
			}
			#endif

			// I2C Read のデータを I2C メッセージのバッファにコピーする
			memcpy(msgs[i + 1].buf, rbuf, rlen);

			// I2C Write + Read として一括で処理したので I2C Read をスキップする
			i++;
		} else {
			// I2C Write 単独の場合

			// ISDB-T demodulator レジスタ 0x03 に 1 バイト書き込む
			// BW: 61 10 02 03 00
			// [0] 0x61 : FX2_I2C_WRITE
			// [1] 0x10 : demodulator I2C アドレス ISDB-T 0x10
			// [2] 0x02 : I2C データ長
			// [3] 0x03 : demodulator レジスタアドレス
			// [4] 0x00 : 値

			// ISDB-T tuner レジスタ 0x02 に 1 バイト書き込む
			// BW: 61 10 04 fe c0 02 00
			// [0] 0x61 : FX2_I2C_WRITE
			// [1] 0x10 : demodulator I2C アドレス ISDB-T 0x10
			// [2] 0x04 : I2C データ長
			// [3] 0xfe : demodulator I2C スルーレジスタ
			// [4] 0xc0 : tuner I2C アドレス
			// [5] 0x02 : tuner レジスタアドレス
			// [6] 0x00 : 値

			// ISDB-S demodulator レジスタに 1 バイト書き込む
			// BW: 61 10 02 03 00
			// ISDB-S tuner レジスタに 1 バイト書き込む
			// BW: 61 11 04 fe c6 00 07

			int wlen = 3 + len;

			if (wlen > MAX_I2C_BUFFER_SIZE) {
				// バッファサイズを超える場合はエラー
				dev_err(&adap->dev, "%s: i2c write length %d exceeds max %d\n", __func__, wlen, MAX_I2C_BUFFER_SIZE);
				ret = -EINVAL;
				goto fail;
			}

			wbuf[0] = FX2_I2C_WRITE;
			wbuf[1] = (u8)addr;
			wbuf[2] = (u8)len;
			memcpy(&wbuf[3], msgs[i].buf, len);

			#ifdef DEBUG
			print_hex_dump_bytes("BW: ", DUMP_PREFIX_NONE, wbuf, wlen);
			#endif

			ret = dvb_usbv2_generic_write(d, wbuf, wlen);

			if (ret < 0) {
				dev_err(&adap->dev, "%s: dvb_usbv2_generic_write failed %d\n", __func__, ret);
				goto fail;
			}
		}
	}

	// すべての I2C メッセージが正常に処理された場合は、I2C メッセージの数を返す
	ret = num;

fail:
	mutex_unlock(&d->i2c_mutex);
	return ret;
}

static u32 akb_pcr20x_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm akb_pcr20x_i2c_algo = {
	.master_xfer = akb_pcr20x_i2c_xfer,
	.functionality = akb_pcr20x_i2c_func,
};

static int akb_pcr20x_i2c_write(struct i2c_adapter *adap, u8 addr, const struct akb_pcr20x_reg_val *regval, int len)
{
	int ret = 0;

	if (!regval || len <= 0) {
		return -EINVAL;
	}

	for (int i = 0; i < len; i++) {
		u8 buf[2] = { regval[i].reg, regval[i].val };
		struct i2c_msg msg = {
			.addr = addr,
			.flags = 0,
			.buf = buf,
			.len = 2
		};

		ret = i2c_transfer(adap, &msg, 1);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int akb_pcr20x_transfer_init(struct dvb_usb_device *d)
{
	struct akb_pcr20x *priv = d_to_priv(d);
	u8 *wbuf = priv->wbuf;
	const u16 wlen = 6;
	int ret = 0;

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	wbuf[0] = FX2_PORT_CFG;
	wbuf[1] = 0;
	wbuf[2] = FX2_PIO_START;
	wbuf[3] = FX2_MODE_IDLE;
	wbuf[4] = FX2_IFCONFIG;
	wbuf[5] = 0xe3;

	#ifdef DEBUG
	print_hex_dump_bytes("BW: ", DUMP_PREFIX_NONE, wbuf, wlen);
	#endif

	ret = dvb_usbv2_generic_write(d, wbuf, wlen);
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: dvb_usbv2_generic_write failed %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

static int akb_pcr20x_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	dev_dbg(&intf->dev, "%s\n", __func__);

	int ret = dvb_usbv2_probe(intf, id);

	if (ret < 0) {
		dev_err(&intf->dev, "%s: dvb_usbv2_probe failed %d\n", __func__, ret);
		return ret;
	}

	return ret;
}

static int akb_pcr20x_init(struct dvb_usb_device *d)
{
	struct akb_pcr20x *priv = d_to_priv(d);

	mutex_init(&priv->mutex_fe);
	priv->active_fe = NULL;
	priv->open_count = 0;

	return 0;
}

static int akb_pcr20x_identify_state(struct dvb_usb_device *d, const char **name)
{
	dev_dbg(&d->udev->dev, "%s\n", __func__);

	u16 vid = le16_to_cpu(d->udev->descriptor.idVendor);
	u16 pid = le16_to_cpu(d->udev->descriptor.idProduct);

	if (vid == 0x04b4 && pid == 0x8613) {
		// firmware ロード前
		dev_dbg(&d->udev->dev, "%s COLD\n", __func__);
		return COLD;
	} else if (vid == 0x04b4 && pid == 0x1004) {
		// firmware ロード後
		dev_dbg(&d->udev->dev, "%s WARM\n", __func__);
		return WARM;
	}

	return -ENODEV;
}

static int akb_pcr20x_ts_bus_control(struct dvb_frontend *fe, int acquire)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	struct akb_pcr20x *priv = d_to_priv(d);

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	mutex_lock(&priv->mutex_fe);

	if (acquire) {
		// アプリケーションが frontend を open した時
		if (priv->active_fe && priv->active_fe != fe) {
			// 既に相方が使用中の場合は、EBUSYを返す
			mutex_unlock(&priv->mutex_fe);
			return -EBUSY;
		}

		priv->active_fe = fe;
		priv->open_count++;
	} else {
		// アプリケーションが frontend を close した時
		if (priv->active_fe == fe && priv->open_count > 0) {
			priv->open_count--;
			if (priv->open_count == 0) {
				priv->active_fe = NULL;
			}
		}
	}

	mutex_unlock(&priv->mutex_fe);
	return 0;
}

static int akb_pcr20x_set_frontend_s(struct dvb_frontend *fe)
{
	static const struct akb_pcr20x_reg_val regval_t[] = {
		{ .reg = 0x0e, .val = 0x77 },
		{ .reg = 0x0f, .val = 0x77 },
	};

	static const struct akb_pcr20x_reg_val regval_s[] = {
		{ .reg = 0x06, .val = 0x40 },
		{ .reg = 0x07, .val = 0x11 },
		{ .reg = 0x08, .val = 0x11 },
		{ .reg = 0x85, .val = 0x7a },
		{ .reg = 0x8e, .val = 0x26 },
		{ .reg = 0xa3, .val = 0xf7 },
	};

	struct dvb_usb_device *d = fe_to_d(fe);
	struct akb_pcr20x *priv = d_to_priv(d);
	int ret = 0;

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	if (!priv->set_frontend_s) {
		dev_err(&d->udev->dev, "%s: set_frontend_s is missing!\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&priv->mutex_fe);

	if (priv->active_fe && priv->active_fe != fe) {
		ret = -EBUSY;
		goto last;
	}

	ret = akb_pcr20x_i2c_write(&d->i2c_adap, priv->i2c_addr.demod_t, regval_t, ARRAY_SIZE(regval_t));
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: i2c_write for demod_t failed: %d\n", __func__, ret);
		goto last;
	}

	ret = akb_pcr20x_i2c_write(&d->i2c_adap, priv->i2c_addr.demod_s, regval_s, ARRAY_SIZE(regval_s));
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: i2c_write for demod_s failed: %d\n", __func__, ret);
		goto last;
	}

	ret = priv->set_frontend_s(fe);
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: set_frontend_s failed: %d\n", __func__, ret);
		goto last;
	}

	ret = 0;

last:
	mutex_unlock(&priv->mutex_fe);
	return ret;
}

static int akb_pcr20x_set_frontend_t(struct dvb_frontend *fe)
{
	static const struct akb_pcr20x_reg_val regval_t[] = {
		{ .reg = 0x0e, .val = 0x11 },
		{ .reg = 0x0f, .val = 0x11 },
		{ .reg = 0x71, .val = 0x20 },
		{ .reg = 0x76, .val = 0x08 },
	};

	struct dvb_usb_device *d = fe_to_d(fe);
	struct akb_pcr20x *priv = d_to_priv(d);
	int ret = 0;

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	if (!priv->set_frontend_t) {
		dev_err(&d->udev->dev, "%s: set_frontend_t is missing!\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&priv->mutex_fe);

	if (priv->active_fe && priv->active_fe != fe) {
		ret = -EBUSY;
		goto last;
	}

	ret = akb_pcr20x_i2c_write(&d->i2c_adap, priv->i2c_addr.demod_t, regval_t, ARRAY_SIZE(regval_t));
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: i2c_write for demod_t failed: %d\n", __func__, ret);
		goto last;
	}

	ret = priv->set_frontend_t(fe);
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: set_frontend_t failed: %d\n", __func__, ret);
		goto last;
	}

	ret = 0;

last:
	mutex_unlock(&priv->mutex_fe);
	return ret;
}

static int akb_pcr20x_frontend_attach_s(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);
	struct akb_pcr20x *priv = d_to_priv(d);

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	priv->config.demod_s = (struct tc90522_config) {
		.fe = NULL,
		.split_tuner_read_i2c = false,
		.tuner_i2c = NULL,
	};

	priv->config.tuner_s = (struct stv6110x_config) {
		.addr = priv->i2c_addr.tuner_s,
		.refclk = 16000000,
		.clk_div = 1,
	};

	struct i2c_client *demod_clt = dvb_module_probe("tc90522", TC90522_I2C_DEV_SAT, &d->i2c_adap, priv->i2c_addr.demod_s, &priv->config.demod_s);

	if (IS_ERR_OR_NULL(demod_clt) || !priv->config.demod_s.fe) {
		dev_err(&d->udev->dev, "%s: failed to probe tc90522 ISDB-S demodulator: %ld\n", __func__, demod_clt ? PTR_ERR(demod_clt) : 0);
		priv->i2c_clt.demod_s = NULL;
		return -ENODEV;
	}

	dev_dbg(&d->udev->dev, "%s: ISDB-S demodulator probed successfully\n", __func__);

	priv->i2c_clt.demod_s = demod_clt;
	adap->fe[0] = priv->config.tuner_s.frontend = priv->config.demod_s.fe;
	priv->set_frontend_s = adap->fe[0]->ops.set_frontend;
	adap->fe[0]->ops.set_frontend = akb_pcr20x_set_frontend_s;
	adap->fe[0]->ops.ts_bus_ctrl = akb_pcr20x_ts_bus_control;

	struct i2c_client *tuner_clt = dvb_module_probe("stv6110a", NULL, priv->config.demod_s.tuner_i2c, priv->i2c_addr.tuner_s, &priv->config.tuner_s);

	if (IS_ERR_OR_NULL(tuner_clt)) {
		dev_err(&d->udev->dev, "%s: failed to probe stv6110a ISDB-S tuner: %ld\n", __func__, tuner_clt ? PTR_ERR(tuner_clt) : 0);
		priv->i2c_clt.demod_s = NULL;
		return -ENODEV;
	}

	dev_dbg(&d->udev->dev, "%s: ISDB-S tuner probed successfully\n", __func__);

	priv->i2c_clt.tuner_s = tuner_clt;

	return 0;
}

static int akb_pcr20x_frontend_attach_t(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);
	struct akb_pcr20x *priv = d_to_priv(d);

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	priv->config.demod_t = (struct tc90522_config) {
		.fe = NULL,
		.split_tuner_read_i2c = false,
		.tuner_i2c = NULL,
	};

	priv->config.tuner_t = (struct mxl5007t_config) {
		.if_diff_out_level = 0,	// ISDB-T の場合使用しない
		.clk_out_amp = MxL_CLKOUT_AMP_0_94V,
		.xtal_freq_hz = MxL_XTAL_16_MHZ,
		.if_freq_hz = MxL_IF_4_MHZ,
		.invert_if = 0,
		.loop_thru_enable = 0,
		.clk_out_enable = 0,
		.frontend = NULL
	};

	struct i2c_client *demod_clt = dvb_module_probe("tc90522", TC90522_I2C_DEV_TER, &d->i2c_adap, priv->i2c_addr.demod_t, &priv->config.demod_t);

	if (IS_ERR_OR_NULL(demod_clt)) {
		dev_err(&d->udev->dev, "%s: failed to probe tc90522 ISDB-T demodulator: %ld\n", __func__, demod_clt ? PTR_ERR(demod_clt) : 0);
		priv->i2c_clt.demod_t = NULL;
		return -ENODEV;
	}

	dev_dbg(&d->udev->dev, "%s: ISDB-T demodulator probed successfully\n", __func__);

	priv->i2c_clt.demod_t = demod_clt;
	adap->fe[1] = priv->config.tuner_t.frontend = priv->config.demod_t.fe;
	priv->set_frontend_t = adap->fe[1]->ops.set_frontend;
	adap->fe[1]->ops.set_frontend = akb_pcr20x_set_frontend_t;
	adap->fe[1]->ops.ts_bus_ctrl = akb_pcr20x_ts_bus_control;

	struct i2c_client *tuner_clt = dvb_module_probe("mxl135rf", NULL, priv->config.demod_t.tuner_i2c, priv->i2c_addr.tuner_t, &priv->config.tuner_t);

	if (IS_ERR_OR_NULL(tuner_clt)) {
		dev_err(&d->udev->dev, "%s: failed to probe mxl135rf ISDB-T tuner: %ld\n", __func__, tuner_clt ? PTR_ERR(tuner_clt) : 0);
		priv->i2c_clt.demod_t = NULL;
		return -ENODEV;
	}

	dev_dbg(&d->udev->dev, "%s: ISDB-T tuner probed successfully\n", __func__);

	priv->i2c_clt.tuner_t = tuner_clt;

	return 0;
}

static int akb_pcr20x_frontend_attach(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);
	struct akb_pcr20x *priv = d_to_priv(d);
	int ret = 0;

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	priv->i2c_addr.demod_s = 0x11;
	priv->i2c_addr.demod_t = 0x10;
	priv->i2c_addr.tuner_s = 0x63;
	priv->i2c_addr.tuner_t = 0x60;

	ret = akb_pcr20x_transfer_init(d);
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: initialization failed: %d\n", __func__, ret);
		return ret;
	}

	ret = akb_pcr20x_frontend_attach_s(adap);
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: failed to attach ISDB-S frontend: %d\n", __func__, ret);
		return ret;
	}

	ret = akb_pcr20x_frontend_attach_t(adap);
	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: failed to attach ISDB-T frontend: %d\n", __func__, ret);
		return ret;
	}

	return 0;
}

static int akb_pcr20x_frontend_detach(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);
	struct akb_pcr20x *priv = d_to_priv(d);

	dev_dbg(&d->udev->dev, "%s\n", __func__);

	if (priv->i2c_clt.demod_s) {
		dvb_module_release(priv->i2c_clt.demod_s);
		priv->i2c_clt.demod_s = NULL;
	}

	if (priv->i2c_clt.demod_t) {
		dvb_module_release(priv->i2c_clt.demod_t);
		priv->i2c_clt.demod_t = NULL;
	}

	return 0;
}

static int akb_pcr20x_streaming_ctrl(struct dvb_frontend *fe, int on)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	struct akb_pcr20x *priv = d_to_priv(d);
	int ret = 0;

	if (on) {
		dev_dbg(&d->udev->dev, "%s: starting stream transfer...\n", __func__);

		priv->wbuf[0] = FX2_EP6IN_START;
		priv->wbuf[1] = FX2_PORT_WRITE;
		priv->wbuf[2] = FX2_PIO_START;

		ret = dvb_usbv2_generic_write_locked(d, priv->wbuf, 3);
	} else {
		dev_dbg(&d->udev->dev, "%s: stopping stream transfer...\n", __func__);

		priv->wbuf[0] = FX2_EP6IN_STOP;
		priv->wbuf[1] = FX2_MODE_IDLE;

		ret = dvb_usbv2_generic_write_locked(d, priv->wbuf, 2);
	}

	if (ret < 0) {
		dev_err(&d->udev->dev, "%s: %s stream transfer failed %d\n", __func__, on ? "starting" : "stopping", ret);
		return ret;
	}

	return 0;
}

static const struct usb_device_id akb_pcr20x_id_table[] = {
	{ DVB_USB_DEVICE(0x04b4, 0x8613, &akb_pcr20x_props, "CY7C68013 EZ-USB FX2", NULL) },
	{ DVB_USB_DEVICE(0x04b4, 0x1004, &akb_pcr20x_props, "AKB-PCR20X", NULL) },
	{}
};
MODULE_DEVICE_TABLE(usb, akb_pcr20x_id_table);

static struct dvb_usb_device_properties akb_pcr20x_props = {
	.driver_name = KBUILD_MODNAME,
	.owner = THIS_MODULE,
	.adapter_nr = adapter_nr,
	.size_of_priv = sizeof(struct akb_pcr20x),

	.firmware = "akb_pcr20x.fw",
	.download_firmware = akb_pcr20x_download_firmware,
	.init = akb_pcr20x_init,
	.identify_state = akb_pcr20x_identify_state,

	.i2c_algo = &akb_pcr20x_i2c_algo,
	.frontend_attach = akb_pcr20x_frontend_attach,
	.frontend_detach = akb_pcr20x_frontend_detach,
	.streaming_ctrl = akb_pcr20x_streaming_ctrl,

	.generic_bulk_ctrl_endpoint = 0x01,
	.generic_bulk_ctrl_endpoint_response = 0x81,

	.num_adapters = 1,
	.adapter = { {
		.stream = DVB_USB_STREAM_BULK(0x86, 8, 32 * 512),
	} }
};

static struct usb_driver akb_pcr20x_driver = {
	.name = "akb_pcr20x",
	.probe = akb_pcr20x_probe,
	.disconnect = dvb_usbv2_disconnect,
	.id_table = akb_pcr20x_id_table,
};

module_usb_driver(akb_pcr20x_driver);

MODULE_AUTHOR("hendecarows");
MODULE_DESCRIPTION("Driver for AKB-PCR20X USB ISDB-T/S Receiver");
MODULE_LICENSE("GPL");