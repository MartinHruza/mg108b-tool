CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

flash_mg108b: flash_mg108b.c

clean:
	rm -f flash_mg108b

.PHONY: clean
