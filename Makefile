#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := Doorbell
SUFFIX := $(shell components/ESP32-RevK/buildsuffix)

all:	main/images.c
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

main/images.h: $(patsubst %.svg,%.h,$(wildcard images/*.svg)) $(patsubst %.png,%.h,$(wildcard images/*.png))
	grep -h 'const unsigned char' images/*.h | sed 's/const unsigned char image_\([A-Za-z0-9]*\).*/extern const unsigned char image_\1[];extern const unsigned int image_\1_size;/' > main/images.h

main/images.c: main/images.h
	cat images/*.h > main/images.c

images/%.mono:    images/%.svg
	inkscape --export-background=WHITE --export-type=png --export-filename=/tmp/images.png $<
	file /tmp/images.png
	convert /tmp/images.png -dither None -monochrome $@
	rm -f /tmp/images.png

images/%.mono:   images/%.png
	convert $< -dither None -monochrome $@

images/%.h:      images/%.mono
	echo "const unsigned char image_$(patsubst images/%.h,%,$@)[]={" > $@
	od -Anone -tx1 -v -w64 $< | sed 's/ \(..\)/0x\1,/g' >> $@
	echo "};" >> $@
	echo "const unsigned int image_$(patsubst images/%.h,%,$@)_size=sizeof(image_$(patsubst images/%.h,%,$@));" >> $@

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
