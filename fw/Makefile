# This is based on https://github.com/lowbyteproductions/bare-metal-series/tree/29d3eeffb1ef67f36934b67f0e2f2d58cea4d690 and the libopencm3 example Makefiles.

# Increase verbosity with 'make V=1'
ifneq ($(V),1)
# Better to keep command lines themselves verbose
Q		:= #@
NULL		:= 2>/dev/null
endif

SRC_DIR     = src
INC_DIR     = include
OPENCM3_DIR = ../libopencm3

BINARY = firmware

# Basic Device Setup
LIBNAME		= opencm3_stm32f4
DEFS		+= -DSTM32F4
FP_FLAGS	?= -mfloat-abi=hard -mfpu=fpv4-sp-d16
ARCH_FLAGS	= -mthumb -mcpu=cortex-m4 $(FP_FLAGS)

# Linkerscript
LDSCRIPT 	= stm32f446re.ld
LDLIBS		+= -l$(LIBNAME)
LDFLAGS		+= -L$(OPENCM3_DIR)/lib

# Includes
DEFS		+= -I$(OPENCM3_DIR)/include
DEFS		+= -I$(INC_DIR)

# Executables
include $(OPENCM3_DIR)/mk/gcc-config.mk

STFLASH		= $(shell which st-flash)
OPT		:= -Os
DEBUG		:= -ggdb3
CSTD		?= -std=c23

# Source files
OBJS		+= $(SRC_DIR)/$(BINARY).o
OBJS		+= $(SRC_DIR)/core/system.o
OBJS		+= $(SRC_DIR)/core/timer.o

# C flags
TGT_CFLAGS	+= $(OPT) $(CSTD) $(DEBUG)
TGT_CFLAGS	+= $(ARCH_FLAGS)
TGT_CFLAGS	+= -Wextra -Wshadow -Wimplicit-function-declaration
TGT_CFLAGS	+= -Wredundant-decls -Wmissing-prototypes -Wstrict-prototypes
TGT_CFLAGS	+= -fno-common -ffunction-sections -fdata-sections

# C++ flags
TGT_CXXFLAGS	+= $(OPT) $(CXXSTD) $(DEBUG)
TGT_CXXFLAGS	+= $(ARCH_FLAGS)
TGT_CXXFLAGS	+= -Wextra -Wshadow -Wredundant-decls  -Weffc++
TGT_CXXFLAGS	+= -fno-common -ffunction-sections -fdata-sections

# C & C++ preprocessor common flags
TGT_CPPFLAGS	+= -MD
TGT_CPPFLAGS	+= -Wall -Wundef
TGT_CPPFLAGS	+= $(DEFS)

# Linker flags
TGT_LDFLAGS		+= --static -nostartfiles
TGT_LDFLAGS		+= -T$(LDSCRIPT)
TGT_LDFLAGS		+= $(ARCH_FLAGS) $(DEBUG)
TGT_LDFLAGS		+= -Wl,-Map=$(*).map -Wl,--cref
TGT_LDFLAGS		+= -Wl,--gc-sections
ifeq ($(V),99)
TGT_LDFLAGS		+= -Wl,--print-gc-sections
endif

# Used libraries
LDLIBS		+= -Wl,--start-group -lc -lgcc -lnosys -Wl,--end-group

###############################################################################

.SUFFIXES: .elf .bin .hex .srec .list .map .images
.SECONDEXPANSION:
.SECONDARY:

all: elf bin

elf: $(BINARY).elf
bin: $(BINARY).bin
hex: $(BINARY).hex
srec: $(BINARY).srec
list: $(BINARY).list
GENERATED_BINARIES=$(BINARY).elf $(BINARY).bin $(BINARY).hex $(BINARY).srec $(BINARY).list $(BINARY).map

images: $(BINARY).images
flash: $(BINARY).flash

$(OPENCM3_DIR)/lib/lib$(LIBNAME).a:
ifeq (,$(wildcard $@))
	$(warning $(LIBNAME).a not found, attempting to rebuild in $(OPENCM3_DIR))
	$(MAKE) -C $(OPENCM3_DIR)
endif

# Define a helper macro for debugging make errors online
# you can type "make print-OPENCM3_DIR" and it will show you
# how that ended up being resolved by all of the included
# makefiles.
print-%:
	@echo $*=$($*)

%.images: %.bin %.hex %.srec %.list %.map
	@#printf "*** $* images generated ***\n"

%.bin: %.elf
	$(Q)$(OBJCOPY) -Obinary $(*).elf $(*).bin

%.hex: %.elf
	$(Q)$(OBJCOPY) -Oihex $(*).elf $(*).hex

%.srec: %.elf
	$(Q)$(OBJCOPY) -Osrec $(*).elf $(*).srec

%.list: %.elf
	$(Q)$(OBJDUMP) -S $(*).elf > $(*).list

%.elf %.map: $(OBJS) $(LDSCRIPT) $(OPENCM3_DIR)/lib/lib$(LIBNAME).a Makefile
	$(Q)$(LD) $(TGT_LDFLAGS) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $(*).elf

%.o: %.c
	$(Q)$(CC) $(TGT_CFLAGS) $(CFLAGS) $(TGT_CPPFLAGS) $(CPPFLAGS) -o $(*).o -c $(*).c

%.o: %.S
	$(Q)$(CC) $(TGT_CFLAGS) $(CFLAGS) -o $(*).o -c $(*).S

%.o: %.cxx
	$(Q)$(CXX) $(TGT_CXXFLAGS) $(CXXFLAGS) $(TGT_CPPFLAGS) $(CPPFLAGS) -o $(*).o -c $(*).cxx

%.o: %.cpp
	$(Q)$(CXX) $(TGT_CXXFLAGS) $(CXXFLAGS) $(TGT_CPPFLAGS) $(CPPFLAGS) -o $(*).o -c $(*).cpp

clean:
	$(Q)$(RM) $(GENERATED_BINARIES) generated.* $(OBJS) $(OBJS:%.o=%.d)


.PHONY: images clean elf bin hex srec list

-include $(OBJS:.o=.d)
