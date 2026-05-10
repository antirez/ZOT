#
# Circle bare-metal ZX Spectrum frontend for Raspberry Pi 3B+
#

# Path to your Circle checkout (override from command line if needed):
#   make CIRCLEHOME=/path/to/circle
CIRCLEHOME ?= ../../circle
RASPPI ?= 3
AARCH ?= 32

VPATH = ..

OBJS = main.o kernel.o compat.o z80.o spectrum.o tzx.o

LIBS = $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/sound/libsound.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/fs/fat/libfatfs.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

CFLAGS += -I..

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
