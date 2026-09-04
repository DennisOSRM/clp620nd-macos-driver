PRINTER_IP ?= 192.168.179.180
QUEUE      ?= CLP620ND
CFLAGS     ?= -O2 -Wall -Wextra
ARCHS      ?= -arch arm64 -arch x86_64

.PHONY: all clean install uninstall test check probe fsdump lint ppd

all: filter/rastertoclp620 backend/clp620 ppd

filter/rastertoclp620: src/rastertoclp620.c
	@mkdir -p filter
	$(CC) $(CFLAGS) $(ARCHS) -o $@ $< -lcups

backend/clp620: src/clp620-backend.c
	@mkdir -p backend
	$(CC) $(CFLAGS) $(ARCHS) -o $@ $<

ppd:            ## regenerate the PPD
	./tools/genppd.sh

check: all      ## run the offline filter test suite (no printer needed)
	@mkdir -p test/tmp
	$(CC) $(CFLAGS) -o test/tmp/mkraster test/mkraster.c -lcups
	@python3 test/run_tests.py
	@python3 test/test_backend.py

lint: all       ## syntax-check scripts and validate the PPD
	@for f in scripts/*.sh tools/*.sh test/*.sh; do bash -n $$f && echo "$$f OK"; done
	@for f in tools/*.py test/*.py; do python3 -m py_compile $$f && echo "$$f OK"; done
	@cupstestppd ppd/Samsung-CLP-620ND.ppd

install: all    ## install driver + create queue (needs sudo)
	sudo ./scripts/install.sh $(PRINTER_IP) $(QUEUE)

uninstall:      ## remove queue + driver
	sudo ./scripts/uninstall.sh $(QUEUE)

test:           ## print the colour test page
	./scripts/testprint.sh $(QUEUE)

probe:          ## dump printer config over PJL
	./scripts/probe.sh $(PRINTER_IP) "INFO CONFIG"

fsdump:         ## walk the printer's PJL filesystem (read-only)
	./tools/pjl-fsdump.py $(PRINTER_IP)

clean:
	rm -f filter/rastertoclp620 backend/clp620 ppd/Samsung-CLP-620ND.ppd
	rm -rf test/tmp
