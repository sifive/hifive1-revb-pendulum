
APP_NAME = pendulum
BOARD = hifive1_revb

APP_DIR = $(shell pwd)/$(APP_NAME)
BUILD_DIR = $(shell pwd)/build/

OBJCOPY = riscv64-unknown-elf-objcopy

.PHONY: all
all: $(BUILD_DIR)/zephyr/zephyr.elf

.PHONY: hex
hex: $(BUILD_DIR)/zephyr/zephyr.elf
	$(OBJCOPY) -O ihex $(BUILD_DIR)/zephyr/zephyr.elf zephyr.hex

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/build.ninja: $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -GNinja -DBOARD=$(BOARD) $(APP_DIR)

$(BUILD_DIR)/zephyr/zephyr.elf: $(BUILD_DIR)/build.ninja
	ninja -C $(BUILD_DIR)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) zephyr.hex

