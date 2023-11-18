all:	$(patsubst %.png,%.mono,$(wildcard *.png))

%.mono:   %.png
	convert $< -dither None -monochrome $@
