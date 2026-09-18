TARGET := akb_pcr20x
VERSION ?= $(shell grep -E '^PACKAGE_VERSION=' dkms.conf | sed -e 's/^PACKAGE_VERSION="\(.*\)"/\1/')
DEBUG ?= 0
KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m += src/

ifeq ($(M),)
	M := $(shell pwd)
endif

.PHONY: clean clangd load unload install-dkms uninstall-dkms

all:
	$(MAKE) -C $(KDIR) M=$(M) modules

clean:
	$(MAKE) -C $(KDIR) M=$(M) clean

clangd:
	@mkdir -p build
	@$(MAKE) -C /usr/src/linux-headers-$(shell uname -r) M=$(M) compile_commands.json
	@mv compile_commands.json build/

install-dkms:
	@echo "Installing $(TARGET) via DKMS..."
	@sudo mkdir -p /usr/src/$(TARGET)-$(VERSION)
	@sudo cp -r . /usr/src/$(TARGET)-$(VERSION)
	@sudo dkms add -m $(TARGET) -v $(VERSION)
	@sudo dkms build -m $(TARGET) -v $(VERSION)
	@sudo dkms install -m $(TARGET) -v $(VERSION)
	@sudo rmmod $(TARGET) 2>/dev/null || true
	@sudo modprobe $(TARGET)

uninstall-dkms:
	@echo "Uninstalling $(TARGET) from DKMS..."
	@sudo dkms remove -m $(TARGET) -v $(VERSION) --all
	@sudo rm -rf /usr/src/$(TARGET)-$(VERSION)

load:
	@echo "Loading dependent modules..."
	@sudo modprobe dvb_core
	@sudo modprobe dvb_usb_v2
	@sudo modprobe tc90522
	@echo "Installing $(TARGET)..."
	@sudo insmod src/stv6110a.ko
	@sudo insmod src/mxl135rf.ko
	@sudo insmod src/$(TARGET).ko
	@dmesg | tail -n 20

unload:
	@echo "Removing $(TARGET)..."
	@sudo rmmod $(TARGET) || true
	@echo "Unloading dependent modules if not in use..."
	@sudo rmmod stv6110a || true
	@sudo rmmod mxl135rf || true
	@sudo rmmod tc90522 || true
	@sudo rmmod dvb_usb_v2 || true
	@dmesg | tail -n 10
