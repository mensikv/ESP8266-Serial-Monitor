# ESP8266 Serial Monitor
This project is developed for ESP8266 boards and it functions as a **serial interface monitor**. Once you connect RX and TX lines (crossed) from this project to any device, that provides ASCII data over serial interface (**3.3V** for ESP8266 only!), you will see the  communication over the lines. But that is not all:
### Features
* display incoming ASCII serial communication on OLED
* write messages and sent them over the serial interface
* change baud rate on the fly
* when connected to a wifi network, web page is provided, that shows the serial communication, enables writing new messages and changing the baud rate
* when connected to a wifi network, the project can connect to a MQTT broker and send all serial communication in the JSON format to the broker. MQTT interface also provides the posibility to change the baud rate and send messages

Project can be used in home automation systems for devices, that provide serial interface and cannot connect to the wifi.
**WIFI networks is not required**.

This project is inspired by [VT220 serial terminal for Arduino](https://innovationgarage.github.io/TTyGO/).

## Prerequisites
### Hardware
It is possible to use any ESP8266 board, but I would recommend:
* [Wemos (Lolin) D1 Mini](https://lolin.aliexpress.com/store/group/D1-D1-mini-Boards/1331105_505460007.html?spm=a2g0o.detail.0.0.57c7637a1P3kJY) (non-affiliated link)
* 3x buttons
* proto board
* OLED display 128x64px, either SSD1306 or SH1106
Connection between the components:
![Scheme](/images/ESM_scheme.png)

### Software
Project was tested with following versions:
* Arduino IDE 1.8.10
* ESP8266 core 2.6.3
* [ESP8266 Sketch Data Upload](https://github.com/esp8266/arduino-esp8266fs-plugin)

Libraries:
* [ArduinoJSON 6.12.0](https://github.com/bblanchon/ArduinoJson)
* [ESP_EEPROM 2.0.0](https://github.com/jwrw/ESP_EEPROM)
* [WifiManages 0.15.0-beta](https://github.com/tzapu/WiFiManager)
* [arduinoWebSockets 2.1.1](https://github.com/Links2004/arduinoWebSockets)
* [PubSubClient 2.7](https://github.com/knolleary/pubsubclient)

## Sending IR codes with a button
You can use a button to send selected IR codes, single click and double click is supported.
If you use Wemos D1 Mini with [IR shield](https://wiki.wemos.cc/products:d1_mini_shields:ir_controller_shield), use these steps:
1. Solder a button on the IR shield between pins D3 and GND.
1. Since D3 is by default used for IR LEDs, you must cut a trace on the back of the IR shield and join two solder pads - I have used pin D2 for IR LEDs.

## Setup
1. Download content of this repository to your Arduino folder (probably `~\Documents\Arduino\ESP8266_Serial_Monitor`, folder should contain one `*.ino` file and `data` folder) and open `ESP8266_Serial_Monitor.ino`.
1. Set pins for buttons - `#define PIN_BUTTON_LEFT 14` ...
1. Uncomment the correct driver for your display `U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);` or `U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);`
1. You can set a hostname by changing `#define ESP_HOSTNAME "SM001"`
1. If you want to enable MQTT API, uncomment `//#define USE_MQTT`
   1. In section `#ifdef USE_MQTT` set connection to your MQTT broker and topics, you want to use.
1. If you want to assign static IP address to the board in your WIFI network, uncomment and set following line `//wifiManager.setSTAStaticIPConfig(IPAddress(192,168,0,99), IPAddress(192,168,0,1), IPAddress(255,255,255,0));`
1. Choose the right board and select Flash size with at least 128kB SPIFFS, on Wemos D1 Mini I have used 4M (1M SPIFFS)
1. Upload the code.
1. Run ESP8266 Sketch Data Upload to upload content of `/data` folder to SPIFFS on ESP8266. Great article is [Chapter 11 - SPIFFS](https://tttapa.github.io/ESP8266/Chap11%20-%20SPIFFS.html). ESP8266 Sketch Data Upload plug-in to load data on SPIFFS can be downloaded from [here](https://github.com/esp8266/arduino-esp8266fs-plugin). `/data` folder contains html code, CSS styles and JavaScript code for the webserver.
   1. Before stating upload to SPIFFS make sure, that Serial monitor window is not opened, otherwise the upload will fail.

## Web page interface
If connectedt to a wifi network, then the ESP8266 will provide web page interface on the IP address of the device. On this page you can:
* view incoming messages over the serial interface
* set baud rate
* write messages that are then written the serial interface
Communication between the page and the webserver is done via Web Sockets, so it is really fast and without reloads.

## MQTT API
If you enable MQTT API, then the ESP8266 will wait for commands in topic:
* MQTT_TOPIC/cmd/bdr
  * payload = desired baudrate, for examle 9600, 115200
* MQTT_TOPIC/cmd/inp
  * payload = message that is sent over the serial interface
And in the topic *MQTT_TOPIC/rspn* you will receive incomming serial communication and notification about baud rate changes. All messages are in JSON format.

## STL for case
In folder `stl` you will find `*.stl` files for your 3D printer, so you can print nice case for Wemos D1 Mini soldered on the proto board with the buttons and display.
More information about the case and photos can be found on [Thingiverse](https://www.thingiverse.com/thing:4226281).
