XLEN ?= 32
TOOLDIR ?= /opt/riscv-gnu-toolchain

TARGET ?= opaesim

VORTEX_DRV_PATH ?= $(realpath ../../../driver)
VORTEX_RT_PATH ?= $(realpath ../../../runtime)

LLVM_PREFIX ?= /opt/llvm-riscv
RISCV_PREFIX ?= $(TOOLDIR)/bin/riscv$(XLEN)-unknown-elf
RISCV_SYSROOT ?= $(TOOLDIR)/riscv$(XLEN)-unknown-elf

CXXFLAGS += -std=c++11 -Wall -Wextra -Wfatal-errors
CXXFLAGS += -I$(VORTEX_RT_PATH)/include -I$(VORTEX_RT_PATH)/../hw/rtl
CXXFLAGS += -I../common

LDFLAGS += -L$(VORTEX_RT_PATH)/stub -lvortex

# Debugigng
ifdef DEBUG
	CXXFLAGS += -g -O0
else    
	CXXFLAGS += -O2 -DNDEBUG
endif

PROJECT = dma

SRCS = main.cpp

# Kernel sources
KERNEL_SRCS = kernel.cpp

all: $(PROJECT) kernel.vxbin

kernel.vxbin: $(KERNEL_SRCS) common.h
	$(VORTEX_RT_PATH)/vxcc $(VORTEX_RT_PATH)/vx_start.s $(KERNEL_SRCS) \
		$(CXXFLAGS) -O3 -march=rv$(XLEN)imf -mabi=ilp32f \
		-Xlinker -T$(VORTEX_RT_PATH)/vx_link$(XLEN).ld \
		-Xlinker --defsym=STARTUP_ADDR=0x80000000 \
		-nostartfiles -fdata-sections -ffunction-sections \
		-I$(VORTEX_RT_PATH)/include \
		-o kernel.vxbin

$(PROJECT): $(SRCS) common.h
	$(CXX) $(CXXFLAGS) $(SRCS) $(LDFLAGS) -o $@

run-simx: $(PROJECT) kernel.vxbin   
	LD_LIBRARY_PATH=$(VORTEX_RT_PATH)/simx:$(LD_LIBRARY_PATH) ./$(PROJECT) -n64

run-rtlsim: $(PROJECT) kernel.vxbin   
	LD_LIBRARY_PATH=$(VORTEX_RT_PATH)/rtlsim:$(LD_LIBRARY_PATH) ./$(PROJECT) -n64

run-opae: $(PROJECT) kernel.vxbin   
	SCOPE_JSON_PATH=$(VORTEX_RT_PATH)/opae/scope.json LD_LIBRARY_PATH=$(VORTEX_RT_PATH)/opae:$(LD_LIBRARY_PATH) ./$(PROJECT)

.depend: $(SRCS)
	$(CXX) $(CXXFLAGS) -MM $^ > .depend;

clean:
	rm -rf $(PROJECT) kernel.vxbin *.o .depend

ifneq ($(MAKECMDGOALS),clean)
    -include .depend
endif

.PHONY: all clean run-simx run-rtlsim run-opae
