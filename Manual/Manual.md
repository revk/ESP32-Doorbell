# Manual

## Connections

The [EPD75](https://github.com/revk/ESP32-GFX/tree/main/PCB/EPD75) board has power connections, and pads for `1` and `2` which are two inputs. A bell push, if needed, should be connected to `1`. Power can be via the `+`/`-` pads or USB and can be 5V to 35V.

The PCB sticks directly to the Waveshare 7.5" display, and can easily be mounted to a door with a double layer of gekko tape.

## Basic working

### Idle image

The idle image shows normally, and can be overlayed with a QR code. A small time (HH:MM) is also shown bottom right. The idle image is rechecked from a web server every hour, or if changed for any reason. It also has automatica seasonable adjustments.

|Name|Time period|
|----|-----------|
|`east`|Good Friday to Easter Monday|
|`hall`|From 4pm on 31st Oct|
|`xmas`|1st to 25th Dec|
|`year`|1st to 7th Jan|
|`idle`|All other times|

### Active image

When the bell push is pressed, or a `push` MQTT or webhook used, the display changes to the active image which states for a `holdtime` (normally 30 seconds) before reverting to the idle image. This can be a preset active image (set by MQTT or web hook), or can be set automatically based on tracking two tasmota switch states.

|Busy|Away|Name|Example|
|----|----|----|----|
|On|On|`Wait`|Please wait, we are on the way|
|Off|On|`Busy`|We are busy, please leave by the door|
|Any|Off|`Away`|We are not available, please leave somewhere secure|

Note that if the switch state changes whilst the active image is displayed, it changes and the timer restarts.

### QR code

The idle and active images have an overlay of a QR code on the bottom left if the `postcode` setting is set. This encodes the date/time (to minute) and postcode. It is intended for a delivery confirmation photo. Ideally allow 100x100 pixels for this in designs of images. Note a small time (HH:MM) is shown bottom right on the idle image.

## LEDs

The rear LEDs show a colour related to an image - this is by default black (off) when idle and blue when active.

However, if the image file name is prefixed with a letter and `:` then this is used to set the colour for that image. E.g. an active image name of `R:HoHoHo` would load `HoHoHo.mono`, and when displayed show red LEDs.

## Image files

The image files are loaded from a web server. The `imageurl` setting is used to set this. It is recommended that `http://` is used rather than `https://` - this is for performance and memory reasons. For security and reliability it is recommended the server be on the local network, e.g. a Raspberry pi.

The files are loaded from the `imageurl` with `/` and the image name and `.mono`. The image name has any colour prefix removed first. The file itself is expected to be a 48000 byte binary image file for 480 x 800 epaper panel. Note that the panel is normally mounted landscape, so this file needs to have been rotated. To convert an 800x480 `png` to a `mono` file use *ImageMagick*, e.g.

`convert `*sourcepng*` -dither None -monochrome -rotate -90 -depth 1 GRAY:`*targetmono*

Note, the web interface links to the same URL with `.png` to show the current image files.

## MQTT settings

Settings can be changed via MQTT as per the [RevK library](https://github.com/revk/ESP32-RevK). You can change a setting by using the topic `setting/Doorbell`. Not that `Doorbell` is all units, and can instead be the *hostname* or *MAC address* of a specific unit. You can set an individual setting, e.g. `setting/Doorbell/imageidle Example`, or use JSON to set multiple settings, e.g. `setting/Doorbell {"image":{"idle":"Example","xmas":"HoHoHo"}}`

You can see all settings by sending `setting/Doorbell` with no payload. There are a number of settings that are part of the library. There are also a number of GPIO settings not covered here (see the code for these). The main settings are as follows, settings starting `image` and `tas` can be grouped as an object to save space.

|Setting|Meaning|
|-------|-------|
|`holdtime`|How long to show active image, seconds, default 30|
|`toot`|If set, send an MQTT topic `toot` with payload `@` and the value of this setting whenever bell push activated. Works with `mqttoot` service to send to a mastodon server as a DM|
|`postcode`|Set the postcode to enable the QR code on display|
|`tasbell`|The name of the tasmota device to use for the bell push, it sends `POWER` with `ON` to the device|
|`tasaway`|The name of the tasmota device that is a switch for *away*, if the light is off it is assumed you are away|
|`tasbusy`|The name of the tasmota device that is a switch for *busy*, if the light is off it is assumed you are busy (unless *away*)|
|`imageurl`|The URL for the image files (see above). Default `https://ota.revk.uk/Doorbell`|
|`imageidle`|The name for the idle image by default. Default `Example`|
|`imagexmas`|The name for the idle image at Christmas|
|`imageyear`|The name for the idle image at New Year|
|`imagehall`|The name for the idle image at Halloween|
|`imagewait`|The name for the *wait* state active image when tracking `tasbusy` and/or `tasaway`. Default `Wait`|
|`imagebusy`|The name for the *busy* state active image when tracking `tasbusy` and/or `tasaway`. Default `Busy`|
|`imagewait`|The name for the *away* state active image when tracking `tasbusy` and/or `tasaway`. Default `Away`|

The unit reboots after a setting change.

## MQTT commands

In addition to commands defined in the *RevK* library, there are a number of specific commands that can be sent. They are of the form `command/Doorbell/` and the command name. As above `Doorbell` can be *hostname* or *MAC address* of a specific unit. Some commands take a payload.

|Command|Meaning|
|-------|-------|
|`active`|Set the current active image name|
|`push`|Activate the bell pushed state and display active message, if a payload is provided this does a one off image display using the payload as image name (and colour prefix)|
|`cancel`|Cancel the current active image and revert to idle image|
|`message`|Display a text message, the payload, separate lines using `/`. This is displated for the current hold time|

## Web hooks

Web hooks allow similar commands without the use of MQTT. These make use of a query string as a parameter if needed.

|URL|Meaning|
|---|-------|
|`/push`|Activate the bell pushed state and display active message, if a query is provided this does a one off image display using the payload as image name (and colour prefix)|
|`/active`|Set the current active image name|
|`/message`|Display a text message, the payload, separate lines using `/`. This is displated for the current hold time|
