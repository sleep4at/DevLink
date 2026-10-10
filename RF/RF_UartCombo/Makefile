.DEFAULT_GOAL := all
TOOLCHAIN_PREFIX ?= riscv-wch-elf-
CC      := $(TOOLCHAIN_PREFIX)gcc
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy
SIZE    := $(TOOLCHAIN_PREFIX)size

ROOT := ../..
APP_SOURCES := $(wildcard APP/*.c)
DRIVER_SOURCES := $(wildcard $(ROOT)/SRC/StdPeriphDriver/*.c)
STARTUP_SOURCE := $(ROOT)/SRC/Startup/startup_CH572.S
SOURCES := $(APP_SOURCES) $(DRIVER_SOURCES) $(STARTUP_SOURCE)
APP_OBJECTS := $(patsubst APP/%.c,obj/APP/%.o,$(APP_SOURCES))
DRIVER_OBJECTS := $(patsubst $(ROOT)/SRC/StdPeriphDriver/%.c,obj/StdPeriphDriver/%.o,$(DRIVER_SOURCES))
STARTUP_OBJECT := obj/Startup/startup_CH572.o
OBJECTS := $(APP_OBJECTS) $(DRIVER_OBJECTS) $(STARTUP_OBJECT)

CFLAGS := -march=rv32imc_zba_zbb_zbc_zbs_xw -mabi=ilp32 -mcmodel=medany \
	-msmall-data-limit=8 -mno-save-restore -Os -fsigned-char \
	-ffunction-sections -fdata-sections -fno-common \
	--param=highcode-gen-section-name=1 -MMD -MP -std=gnu99
CPPFLAGS := -IAPP/include -I$(ROOT)/SRC/StdPeriphDriver/inc \
	-I$(ROOT)/SRC/RVMSIS -I$(ROOT)/RF/LIB
LDFLAGS := -TLd/Link.ld -nostartfiles -Wl,--gc-sections \
	-Wl,--print-memory-usage -Wl,-Map,obj/RF_UartCombo.map \
	-L$(ROOT)/RF/LIB -L$(ROOT)/SRC/StdPeriphDriver
LDLIBS := -lISP572 -lm -lCH57xRF

.PHONY: all clean
-include $(OBJECTS:.o=.d)
all: obj/RF_UartCombo.hex

obj/RF_UartCombo.elf: $(OBJECTS) Ld/Link.ld
	"$(CC)" $(CFLAGS) $(LDFLAGS) --specs=nano.specs --specs=nosys.specs \
		-o $@ $(OBJECTS) $(LDLIBS)

obj/RF_UartCombo.hex: obj/RF_UartCombo.elf
	"$(OBJCOPY)" -O ihex $< $@
	"$(SIZE)" $<

obj/APP/%.o: APP/%.c
	mkdir -p $(dir $@)
	"$(CC)" $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj/StdPeriphDriver/%.o: $(ROOT)/SRC/StdPeriphDriver/%.c
	mkdir -p $(dir $@)
	"$(CC)" $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj/Startup/%.o: $(ROOT)/SRC/Startup/%.S
	mkdir -p $(dir $@)
	"$(CC)" $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f obj/APP/*.o obj/StdPeriphDriver/*.o obj/Startup/*.o \
		obj/APP/*.d obj/StdPeriphDriver/*.d obj/Startup/*.d \
		obj/RF_UartCombo.elf obj/RF_UartCombo.hex obj/RF_UartCombo.map
