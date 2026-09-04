PRINTER_IP ?= 192.168.179.180
QUEUE      ?= CLP620ND
CFLAGS     ?= -O2 -Wall -Wextra
ARCHS      ?= -arch arm64 -arch x86_64

.PHONY: all clean install uninstall test probe lint ppd

all: filter/rastertoclp620 ppd

filter/rastertoclp620: src/rastertoclp620.c
	@mkdir -p filter
	$(CC) $(CFLAGS) $(ARCHS) -o $@ $< -lcups

ppd:            ## regenerate the PPD
	./tools/genppd.sh

lint: all       ## syntax-check scripts and validate the PPD
	@for f in scripts/*.sh tools/*.sh; do bash -n $$f && echo "$$f OK"; done
	@cupstestppd ppd/Samsung-CLP-620ND.ppd

install: all    ## install driver + create queue (needs sudo)
	sudo ./scripts/install.sh $(PRINTER_IP) $(QUEUE)

uninstall:      ## remove queue + driver
	sudo ./scripts/uninstall.sh $(QUEUE)

test:           ## print the colour test page
	./scripts/testprint.sh $(QUEUE)

probe:          ## dump printer config over PJL
	./scripts/probe.sh $(PRINTER_IP) "INFO CONFIG"

clean:
	rm -f filter/rastertoclp620 ppd/Samsung-CLP-620ND.ppd
