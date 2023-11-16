#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := Doorbell
SUFFIX := $(shell components/ESP32-RevK/buildsuffix)

all:	
	@echo Make: $(PROJECT_NAME)$(SUFFIX).bin
	@idf.py build
	@cp build/$(PROJECT_NAME).bin $(PROJECT_NAME)$(SUFFIX).bin
	@echo Done: $(PROJECT_NAME)$(SUFFIX).bin

issue:
	-git pull
	-git submodule update --recursive
	-git commit -a -m checkpoint
	@make set
	cp $(PROJECT_NAME)*.bin release
	git commit -a -m release
	git push

set:    gd7965

gd7965:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-GD7965
	@make

main/images.h: $(patsubst %.svg,%.h,$(wildcard images/*.svg))
	cat images/*.h > main/images.h

images/%.png:    images/%.svg
	inkscape --export-background=WHITE --export-type=png --export-filename=$@ $<

images/%.bw:   images/%.png
	convert $< -resize 32x32 -depth 1 -grayscale average -negate $@

images/%.h:      images/%.bw
	echo "const uint8_t icon_$(patsubst images/%.h,%,$@)[]={" > $@
	od -Anone -tx1 -v -w64 $< | sed 's/ \(.\). \(.\)./0x\1\2,/g' >> $@
	echo "};" >> $@

flash:
	idf.py flash

monitor:
	idf.py monitor

clean:
	idf.py clean

menuconfig:
	idf.py menuconfig

pull:
	git pull
	git submodule update --recursive

update:
	git submodule update --init --recursive --remote
	-git commit -a -m "Library update"
