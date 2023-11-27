all:	$(patsubst %.png,%.mono,$(wildcard *.png))

%.mono:   %.png
	convert $< -dither None -monochrome -rotate -90 -depth 1 GRAY:$@
