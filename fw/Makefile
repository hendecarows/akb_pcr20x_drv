VID=0x04b4
PID=0x1004
BASENAME=akb_pcr20x
FIRMWARE=build/$(BASENAME).fw
SOURCES=akb_pcr20x_fw.c fw.c i2c.c
A51_SOURCES=dscr.a51
DESTDIR=/lib/firmware
FX2LIB_DIR=fx2lib
FX2LIB_REPO=https://github.com/djmuhlestein/fx2lib.git
FX2LIB_REF=master
PYTHON:=python3

ifeq ($(wildcard $(FX2LIB_DIR)/lib/fx2.mk),)
$(info fx2lib not found. Cloning $(FX2LIB_REPO) ($(FX2LIB_REF))...)
$(shell git clone --branch $(FX2LIB_REF) $(FX2LIB_REPO) $(FX2LIB_DIR))
ifeq ($(wildcard $(FX2LIB_DIR)/lib/fx2.mk),)
$(error Failed to clone fx2lib into $(FX2LIB_DIR))
endif
endif

include ./fx2lib/lib/fx2.mk

.PHONY: all clean-extra install update

all: $(FIRMWARE)

$(FIRMWARE): build/$(BASENAME).ihx
	$(PYTHON) ./fx2lib/utils/ihx2iic.py -v $(VID) -p $(PID) $< $@

update:
	git -C $(FX2LIB_DIR) pull

clean: clean-extra

clean-extra:
	rm -rf build fx2lib

install: $(FIRMWARE)
	@echo "Installing $< to $(DESTDIR)/"
	sudo cp -f $< $(DESTDIR)/