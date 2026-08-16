# akb_pcr20x_drv

[AKB-PCR20X][link_akb] 用の Linux DVB ドライバです。

* USB Bridge : Cypress EZ-USB FX2 CY7C68013A
* CPLD : Altera MaxII EPM570T100C5N
* Demodulator : TOSHIBA TC90532XBG
* Tuner ISDB-S : STMicroelectronic STV6110a
* Tuner ISDB-T : MaxLinear MxL135RF

ハードウェアはオリジナル状態(ロジックを書き換えていない)のみの対応で ISDB-S と ISDB-T
の同時使用不可、LNB 電源の供給はできません。

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

AKB-PCR20X に採用されている EZ-USB FX2 (VID: 0x0b4b, PID: 0x8613)
に対してカーネルドライバ usbtest がロードされないようにブラックリストに追加します。

```bash
sudo cp etc/blacklist_usbtest.conf /etc/modprobe.d/
```

### DKMS によるインストール

以下のコマンドを実行し DKMS でインストールします。

```bash
sudo make install-dkms
```

### ファームウェア

動作にはファームウェア `/lib/firmware/akb_pcr20x.fw` が必要です。`fw/` ディレクトリで [fx2lib][link_fx2lib] 使用のファームウェアを sdcc でコンパイルしインストールします。

```bash
cd fw
make
sudo make install
```

## 使用方法

ロードに成功すると、ISDB-S 用 frontend0 と ISDB-T 用 frontend1 の2個の forntend が作成されます。

* ISDB-S : `/dev/dvb/adapterX/frontend0`
* ISDB-T : `/dev/dvb/adapterX/frontend1`

dvbv5-zap では -f オプションを使用して frontend 番号を指定します。

```bash
# ISDB-S : BS/CS110の場合
dvbv5-zap -a 0 -f 0 -c /usr/local/etc/dvbv5/dvbv5_channels_isdbs.conf -r -P -t 10 -o a.ts BS01_1

# ISDB-T : 地デジの場合
dvbv5-zap -a 0 -f 1 -c /usr/local/etc/dvbv5/dvbv5_channels_isdbt.conf -r -P -t 10 -o b.ts 13
```

### BonDriver_LinuxDVB

[BonDriver_LinuxDVB][link_bondvb] の設定は、以下を参考に各チャンネル空間の Frontend 番号を定義して下さい。

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

[Mirakurun][link_mirakurun] の場合、録画コマンド(command) には dvbv5-zap ではなく、
チャンネル名から放送種別(types)を判定し frontend 番号を切り替えるコマンドを登録する必要があります。
付属の `etc/recakb.sh` を `/opt/mirakurun/opt/bin` にコピーし録画コマンドとして
command: に登録してて下さい。

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

### ドロップに関して

以下の環境でテストしたところ N100 のみドロップが頻発しました。

* Intel Core i5 6500
* Intel Celeron J4105
* Intel N100

cpupower を使用して省電力設定を無効化することで改善します。

```bash
sudo cpupower idle-set -D 10
```

再起動後も有効にするには、systemd で起動時に実行するように設定します。

```ini
[Unit]
Description=Disable deep C-states for TS recording stability
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/bin/cpupower idle-set -D 10
RemainAfterExit=yes

[Unit]
[Install]
WantedBy=multi-user.target
```

## ライセンス

[GPL v2 License][link_gpl]

[link_akb]: http://www.akibastock.com/pages/pcr20x.html
[link_fx2lib]: https://github.com/djmuhlestein/fx2lib
[link_bondvb]: https://github.com/hendecarows/BonDriver_LinuxDVB
[link_mirakurun]: https://github.com/chinachu/mirakurun
[link_gpl]: LICENSE
