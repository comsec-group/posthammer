.PHONY: all clean
SHELL := /bin/bash

all: posthammer.js
	echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
	echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
	touch dump
	mv -n dump dump.old
	../jsshell-130/js $< a2 |& tee dump

# --outFile does not work!?
posthammer.js: posthammer.ts
	tsc -p . --out $@

# Gave it this weird __ suffix because otherwise tsc --out posthammer.ts will
# somehow also compile it!?
mem.js: mem.ts__
	tsc --out $@

mem: mem.js
	echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
	echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
	strace -k ../jsshell-130/js $<

clean:
	rm -f posthammer.js mem.js
