CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)

mgctl: mgctl.c

clean:
	rm -f mgctl

.PHONY: clean
