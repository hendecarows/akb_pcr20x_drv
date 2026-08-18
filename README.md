# akb_pcr20x_drv

[AKB-PCR20X][link_akb] 用の Linux DVB ドライバです。

* USB Bridge : Cypress EZ-USB FX2 CY7C68013A
* CPLD : Altera Max II EPM570T100C5N
* Demodulator : TOSHIBA TC90532XBG
* Tuner ISDB-S : STMicroelectronics STV6110A
* Tuner ISDB-T : MaxLinear MxL135RF

本ドライバは、ロジックを書き換えていないオリジナル状態のハードウェアのみに対応しています。

* ISDB-S と ISDB-Tの同時使用はできません
* LNB 電源の供給はできません

## インストール

インストールには開発ツール、git、DKMS、カーネルヘッダが必要です。

```bash
# Ubuntu
sudo apt install -y build-essential sdcc git dkms linux-headers-generic-hwe-xx.xx
```

```bash
git clone https://github.com/hendecarows/akb_pcr20x_drv.git
cd akb_pcr20x_drv
```

### usbtest の無効化

USB Bridge EZ-USB FX2 (VID: 0x0b4b, PID: 0x8613) に対してカーネルドライバ
usbtest がロードされないようにブラックリストに追加します。

```bash
sudo cp etc/blacklist_usbtest.conf /etc/modprobe.d/
```

### ドライバのインストール

以下のコマンドで DKMS によるインストールが可能です。

```bash
sudo make install-dkms
```

### ファームウェアのインストール

動作にはファームウェア `/lib/firmware/akb_pcr20x.fw` が必要です。
オリジナルCD付属のファームウェアでは EOVERFLOW エラーが頻発するため、[fx2lib][link_fx2lib]
を用いて移植・対策を行ったファームウェアを fw ディレクトリ内に用意しています。

```bash
cd fw
make
sudo make install
```

## 使用方法

ロードに成功すると、ISDB-S 用 frontend0 と ISDB-T 用 frontend1 の2個の frontend が作成されます。

* ISDB-S : `/dev/dvb/adapterX/frontend0`
* ISDB-T : `/dev/dvb/adapterX/frontend1`

dvbv5-zap を使用する場合、放送種別 (ISDB-S,ISDB-T) に応じて -f オプションでフロントエンド番号を変更します。

```bash
# ISDB-S : BS/CS110の場合
dvbv5-zap -a 0 -f 0 -c /usr/local/etc/dvbv5/dvbv5_channels_isdbs.conf -r -P -t 10 -o bs.ts BS01_0

# ISDB-T : 地デジの場合
dvbv5-zap -a 0 -f 1 -c /usr/local/etc/dvbv5/dvbv5_channels_isdbt.conf -r -P -t 10 -o gr.ts 13
```

### BonDriver_LinuxDVB

[BonDriver_LinuxDVB][link_bondvb] をコミット [0bdcccb][link_bondvb_commit] 以降に更新し、
ini ファイルの各 `[Space.XXX]` セクションにて、以下の様に放送種別に応じたフロントエンド番号 (DvbFrontend) を設定して下さい。

```ini
[Space.UHF]
Name=地デジ(UHF)
System=ISDB-T
DvbFrontend=1
...
[Space.BS]
Name=BS
System=ISDB-S
LnbPowerMode=0
DvbFrontend=0
...
[Space.CS110]
Name=CS110
System=ISDB-S
LnbPowerMode=0
DvbFrontend=0
```

### Mirakurun

本ドライバは ISDB-S, ISDB-T でフロントエンドが分かれているため、[Mirakurun][link_mirakurun]
の tuners.yml 内の command: に dvbv5-zap を直接指定できません。

チャンネル文字列（recpt1 互換）から放送種別を判定し、適切なフロントエンド番号で
dvbv5-zap を呼び出すスクリプト recakb.sh を etc ディレクトリに用意しています。

1. recakb.sh を `/opt/mirakurun/opt/bin/` にコピー（実行権限を付与）
2. tuners.yml の command: に登録

```yml
# tuners.yml
- name: adapter0
  types:
    - GR
    - BS
    - CS
  dvbDevicePath: /dev/dvb/adapter0/dvr0
  command: recakb.sh -a 0 <channel>
  decoder: arib-b25-stream-test
  isDisabled: false
```

### TS ドロップ対策

以下の環境でテストしたところ Intel N100 のみ TS ドロップが頻発することを確認しています。

* Intel Core i5-6500
* Intel Celeron J4105
* Intel N100

CPU の C-state 制御による省電力機能を抑止（深い C-state を無効化）することで解消します。

```bash
sudo cpupower idle-set -D 10
```

再起動後も永続化するには、例えば `/etc/systemd/system/cpupower-idle.service` を作成して起動時に実行するよう設定します。

```ini
[Unit]
Description=Disable deep C-states for TS recording stability
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/bin/cpupower idle-set -D 10
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

## ライセンス

[GPL v2 License][link_gpl]

[link_akb]: http://www.akibastock.com/pages/pcr20x.html
[link_fx2lib]: https://github.com/djmuhlestein/fx2lib
[link_bondvb]: https://github.com/hendecarows/BonDriver_LinuxDVB
[link_bondvb_commit]: https://github.com/hendecarows/BonDriver_LinuxDVB/commit/0bdcccb3588c731c26eb9089c55457c0bbb0b73a
[link_mirakurun]: https://github.com/chinachu/mirakurun
[link_gpl]: LICENSE
