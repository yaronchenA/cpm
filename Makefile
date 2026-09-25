# CPM prototype firmware — NUCLEO-F302R8

TARGET  := cpm
BUILD   := build
SRCS    := src/main.c src/startup_stm32f302x8.c
LDSCRIPT := stm32f302r8.ld

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

# Soft-float on purpose: production CPM is M0/G0 class, don't depend on the FPU
# (prototype_plan.md §2.1).
CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
CFLAGS   := $(CPUFLAGS) -Os -g3 -Wall -Wextra -std=c11 -ffunction-sections -fdata-sections
LDFLAGS  := $(CPUFLAGS) -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs

OBJS := $(SRCS:%.c=$(BUILD)/%.o)

# Pin OpenOCD to the Nucleo's ST-LINK/V2-1 (PID 374b, or 3752 after a firmware
# update) so the SIU Discovery board (ST-LINK/V2, 3748) can stay plugged in too.
STLINK_VID_PID := 0x0483 0x374b 0x0483 0x3752

.PHONY: all clean flash

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin
	$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

flash: $(BUILD)/$(TARGET).elf
	openocd -f board/st_nucleo_f3.cfg -c "hla_vid_pid $(STLINK_VID_PID)" -c "program $< verify reset exit"

clean:
	rm -rf $(BUILD)
