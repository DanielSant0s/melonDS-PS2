EE_BIN = melonDS.elf
GIT_VERSION := $(shell git describe --abbrev=6 --dirty --always --tags)

BIN2S = $(PS2SDK)/bin/bin2s

CPPSOURCES  := src src/ps2
INCLUDES := src

EE_LIBS = -L$(PS2SDK)/ports/lib -L$(PS2DEV)/gsKit/lib/ -lpatches -lfileXio -lpad -ldebug -lgskit_toolkit -lgskit -ldmakit -lpng -lz -lmc -laudsrv

EE_INCS += -I$(PS2DEV)/gsKit/include -I$(PS2SDK)/ports/include -I$(PS2SDK)/ports/include/zlib

IOP_MODULES = src/sio2man.o src/mcman.o src/mcserv.o src/padman.o src/libsd.o \
			  src/usbd.o src/audsrv.o src/bdm.o src/bdmfs_vfat.o \
			  src/usbmass_bd.o

CPPFILES   := $(foreach dir,$(CPPSOURCES), $(wildcard $(dir)/*.cpp))
BINFILES := $(foreach dir,$(DATA), $(wildcard $(dir)/*.bin))
EE_OBJS     := $(IOP_MODULES) $(addsuffix .o,$(BINFILES)) $(CPPFILES:.cpp=.o)

export INCLUDE	:= $(foreach dir,$(INCLUDES),-I$(dir))

EE_CXXFLAGS += -mtune=r5900 -msingle-float -fno-exceptions -std=gnu++11 -D__PS2__

all: $(EE_BIN)

#-------------------- Embedded IOP Modules ------------------------#
src/sio2man.s: $(PS2SDK)/iop/irx/sio2man.irx
	echo "Embedding SIO2MAN Driver..."
	$(BIN2S) $< $@ sio2man_irx
	
src/mcman.s: $(PS2SDK)/iop/irx/mcman.irx
	echo "Embedding MCMAN Driver..."
	$(BIN2S) $< $@ mcman_irx

src/mcserv.s: $(PS2SDK)/iop/irx/mcserv.irx
	echo "Embedding MCSERV Driver..."
	$(BIN2S) $< $@ mcserv_irx

src/padman.s: $(PS2SDK)/iop/irx/padman.irx
	echo "Embedding PADMAN Driver..."
	$(BIN2S) $< $@ padman_irx
	
src/libsd.s: $(PS2SDK)/iop/irx/libsd.irx
	echo "Embedding LIBSD Driver..."
	$(BIN2S) $< $@ libsd_irx

src/usbd.s: $(PS2SDK)/iop/irx/usbd.irx
	echo "Embedding USB Driver..."
	$(BIN2S) $< $@ usbd_irx

src/audsrv.s: $(PS2SDK)/iop/irx/audsrv.irx
	echo "Embedding AUDSRV Driver..."
	$(BIN2S) $< $@ audsrv_irx

src/bdm.s: $(PS2SDK)/iop/irx/bdm.irx
	echo "Embedding Block Device Manager(BDM)..."
	$(BIN2S) $< $@ bdm_irx

src/bdmfs_vfat.s: $(PS2SDK)/iop/irx/bdmfs_vfat.irx
	echo "Embedding BDM VFAT Driver..."
	$(BIN2S) $< $@ bdmfs_vfat_irx

src/usbmass_bd.s: $(PS2SDK)/iop/irx/usbmass_bd.irx
	echo "Embedding BD USB Mass Driver..."
	$(BIN2S) $< $@ usbmass_bd_irx

#------------------------------------------------------------------#

clean:
	@rm -rf $(EE_BIN) $(EE_OBJS)
	rm -f src/sio2man.s
	rm -f src/mcman.s
	rm -f src/mcserv.s
	rm -f src/padman.s
	rm -f src/libsd.s
	rm -f src/bdm.s
	rm -f src/usbd.s
	rm -f src/audsrv.s
	rm -f src/bdmfs_vfat.s
	rm -f src/usbmass_bd.s

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal_cpp