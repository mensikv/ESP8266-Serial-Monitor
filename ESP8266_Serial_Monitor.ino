/*
  ####
  ## ESP8266 Serial Monitor
  ####
  Detects ASCII communication over serial interface (RX pin) and can send serial ASCII data on the TX pin.
  Can function offline or online.
  When offline, only the OLED display and thrre buttons provide user interface.
  When online, there is web page provided, that displays serial communication (using WebSockets). You can also connect to yout MQTT broker and receive serial data over MQTT messages.

  HARDWARE:
  Wemos D1 Mini
  SSD1306 128x64 OLED display - connected via I2C, or you can use SH1106 displays - just change the constructor for U8G2
  3 button connected between D3, D4, D5 and GND

  SOFTWARE
  upload code and then upload files

  use ESP8266 Sketch Data Upload to upload files to ESP8266 SPIFFS: // https://github.com/esp8266/arduino-esp8266fs-plugin
    data\bootstrap.min.css.gz
    data\bootstrap.min.js.gz
    data\favicon.ico
    data\index.html.gz
    data\jquery-3.4.1.min.js.gz

  Tested with versions:
    Arduino IDE 1.8.10
    ESP8266 core 2.6.3

  Libraries:
    ArduinoJSON 6.12.0
    ESP_EEPROM 2.0.0
    WifiManages 0.15.0-beta
    PubSubClient 2.7

  WEB API:
    Starts HTTP server with one page, that shows the serial communication and allows to change the baud rate and send messages.

  MQTT API:
   Commands:
    /cmd/bdr ; payload: 115200
    /cmd/inp ; payload: message to sent over serial interface
   Returns JSON in:
    /rspn      ; payload: JSON

  BUTTON actions:
    only single click on all buttons, buttons have different meaning in different program states
*/

#include <Arduino.h>
#include <Wire.h>

#include <FS.h>                   // library for file system - for HTTP server
#include <ESP8266WiFi.h>          // ESP8266 Core WiFi Library
#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal - needed for WifiManager
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal

#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ESP_EEPROM.h>           // https://github.com/jwrw/ESP_EEPROM
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson
#include <WebSocketsServer.h>     // https://github.com/Links2004/arduinoWebSockets
#include <U8g2lib.h>              // https://github.com/olikraus/u8g2



#define ESP_HOSTNAME    "SM001"
#define VERSION "0.01"

// MQTT API - if it is not needed, it should be commented
//#define USE_MQTT
#ifdef USE_MQTT
  #include <PubSubClient.h>                     // https://github.com/knolleary/pubsubclient
  #define MQTT_SERVER "192.168.1.100"
  #define MQTT_USERNAME "user"
  #define MQTT_PASSWORD "pasw"
  #define MQTT_TOPIC           ESP_HOSTNAME
  #define MQTT_RSPN_TOPIC      MQTT_TOPIC "/rspn"                 // response JSON
  #define MQTT_CMD_TOPIC       MQTT_TOPIC "/cmd"                  // request messages
  #define MQTT_LAST_WILL_TOPIC MQTT_TOPIC "/live"
  #define MQTT_LAST_WILL "OFF"
  #define MQTT_MAX_PAYLOAD_LENGTH (ROW_LENGTH-2)
#endif


#define PIN_BUTTON_LEFT          14   // D5
#define PIN_BUTTON_OK             2   // D4
#define PIN_BUTTON_RIGHT          0   // D3

#define DISPLAY_HEIGHT_PX        64   // dimension of the display - height
#define DISPLAY_WIDTH_PX        128   // dimension of the display - width
#define KEYBOARD_ITEM_PADDING_PX  3   // left and right padding for individual elements in keyboard menu
#define KEYBOARD_OUTER_PADDING_PX 3   // left and right padding of the whole keyboard
#define DISPLAY_REFRESH_MS  1000/10   // time in ms after which OLED display is redrawn - 10x per second
 
#define ROW_LENGTH          DISPLAY_WIDTH_PX/4      // number of letters on one row in buffer for serial communication, (for 4*6px letters) == 32; LETTER = 4*6px - in reality 3 * 5
#define SERIAL_BUFFER_ROWS    (5*8)   // number of rows in buffer for serial communication
#define MENU_ITEM_MAX_LENGTH      7   // maximum character length of a menu item title
#define FAST_CLICKING_MS        300   // if interval between click is less then FAST_CLICKING_MS, then menu skip more than 1 item
#define SERIAL_MESSAGE_TIMEOUT  500   // if more time passes then SERIAL_MESSAGE_TIMEOUT and buffer for input Serial communication is not empty, it will be copied into serial buffer
#define DEFAULT_BAUD_RATE    115200
#define SHOW_ERROR_MS          5000

#define ARRAY_SIZE(A)             (sizeof(A) / sizeof((A)[0]))

// Choose the right display driver
//U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);


// structure for one menu item
typedef struct {
  char txt[MENU_ITEM_MAX_LENGTH];
  uint8_t txtWidth;
} TKeyboardItem;

// structure to hold important data for display
typedef struct {
  uint8_t  characterWidth;         // we use monospace fonts, so this is the width in pixel of all fonts characters
  uint32_t lastDisplayRefreshMS;   // last time we have refreshed the display
  uint8_t  menuIndex;              // actualy selected item in a menu
  uint8_t  menuStep;               // how many items in a menu are skipped during one buttion click
} TDisplaySetting;

// structure for serial buffer
typedef struct {
  char buffer[SERIAL_BUFFER_ROWS][ROW_LENGTH+1]; // total rows and length of rows - 32 letters per row + ending zero; full page = 8 rows - buffer for 5 pages
  char incomeBuffer[ROW_LENGTH+1];               // used to store income Serial communication, once full or timeout, the content is coppied to sr.buffer
  char sendBuffer[ROW_LENGTH+1-2];               // used to store texts, that is written to the serial interface, 2B are used for "> "
  uint8_t writeIndex;                            // index for row in .buffer, where we will copy new row
  uint8_t readIndex;                             // index for row in .buffer, that is the last row to display
  uint32_t lastMessageMS;                        // timestamp of last received message over Serial
} TSerialBuffer;

// structure for settings
typedef struct {
  uint32_t baudRate;                  // actual baud rate set on the serial interface
} TSetting;

 // structure, that is written into the EEPROM memory
typedef struct {                       
  uint32_t crc32;                       // 4 bytes - to check if data are valid
  TSetting setting;
} TEeprom;

enum TprogramStates {PROGRAM_RECEIVING, PROGRAM_BDR, PROGRAM_LETTER, PROGRAM_SCROLLING, PROGRAM_ERROR};   // program goes through states



// menus
uint32_t baudRateList[] = {1200L, 2400L, 4800L, 9600L, 19200L, 38400L, 57600L, 115200L};                     // supported baud rate speeds
char asciiList[] = {
  'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
  'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z',
  '0','1','2','3','4','5','6','7','8','9',
  '.',',','!','?','"','@','#','$','%','&','\'',
  '(',')','[',']','{','}','+','-','*','/','=','<','>',
  ':',';','^','~','_','\\','|',
  '\x20','\x8','\x18','\x4'};      // space - 32	20	SP (space); DEL - 8	08	BS	Backspace; CAN - 24	18	CAN	Cancel; SND - 4	04	EOT	End of Transmission
uint8_t         baudRateListCount;
uint8_t         asciiListCount;
TKeyboardItem  *baudRateKeyboard;   // ascii representation of supported baud rates
TKeyboardItem  *asciiKeyboard;      // ascii representation of letters for writing
TDisplaySetting dsp;                // 
TSerialBuffer   sr;                 // buffer for data received over Serial + additional data 
TEeprom         eeprom;
TSetting       *setting;            // setting of the Serial Monitor (like baud rate)

uint8_t         wifiON;             // 1 = connected to wifi, 0 = offline

uint32_t        programMS;          // actual time in program in ms
uint32_t        errorEnterMS;       // time when error occured
uint32_t        lastActionMS;       // last time user pressed a button
uint8_t         lastMenuAction;     // last pressed button
String          strVar;             // Strings for storing JSON - reserve for 512 characters
char            errStr[ROW_LENGTH + 1]; // variable to store runtime error 

enum TprogramStates programState;  // the state of the program
enum TprogramStates beforeErrorState;

// Web serever will run on port 80
ESP8266WebServer server(80);

// Websocket server will run on port 81
WebSocketsServer websocketServer = WebSocketsServer(81);

#ifdef USE_MQTT
  WiFiClient espWifiClient;
  PubSubClient mqtt_client(espWifiClient);
#endif



void switchToError(char *err) {
  beforeErrorState = programState;
  programState = PROGRAM_ERROR;
  errorEnterMS = millis();
  strcpy(errStr, err);
}

// ==================== WIFI AP configuration ====================
// When Wifi Manager could not connect to WIFI, it will start up its own AP with configuration portal. During configuration we display info on display
void startWMConfig(WiFiManager *wmp) {
  char txt[ROW_LENGTH+1];
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  sprintf(txt, "Wifi config mode");
  u8g2.drawStr(DISPLAY_WIDTH_PX/2 - u8g2.getStrWidth(txt)/2,7, txt);
  u8g2.drawHLine(0, 9, DISPLAY_WIDTH_PX);  
  sprintf(txt, "Join Wifi: %s", wmp->getConfigPortalSSID().c_str());
  u8g2.drawStr(DISPLAY_WIDTH_PX/2 - u8g2.getStrWidth(txt)/2, 25, txt);
  sprintf(txt, "Go to the URL: %s", WiFi.softAPIP().toString().c_str());
  u8g2.drawStr(DISPLAY_WIDTH_PX/2 - u8g2.getStrWidth(txt)/2, 33, txt);
  sprintf(txt, "Set the target Wifi network.");
  u8g2.drawStr(DISPLAY_WIDTH_PX/2 - u8g2.getStrWidth(txt)/2, 41, txt);

  u8g2.sendBuffer();
}



// ==================== EEPROM helpers ====================
uint8_t saveEEPROM() {
  eeprom.crc32 =  calculateCRC32( ((uint8_t*)&eeprom) + 4, sizeof( eeprom ) - 4 );  // 4B are discarded, because they corrspond to crc32 field

  EEPROM.put(0, eeprom);
  uint8_t result = EEPROM.commit();  
  return result;  // 1 = OK/ 0= problem during saving
}

uint8_t resetEEPROM() {
  setting->baudRate = DEFAULT_BAUD_RATE;
  return saveEEPROM();
}

uint32_t calculateCRC32( const uint8_t *data, size_t length ) {
  uint32_t crc = 0xffffffff;
  while ( length-- ) {
    uint8_t c = *data++;
    for ( uint32_t i = 0x80; i > 0; i >>= 1 ) {
      bool bit = crc & 0x80000000;
      if ( c & i ) {
        bit = !bit;
      }
      crc <<= 1;
      if ( bit ) {
        crc ^= 0x04c11db7;
      }
    }
  }
  return crc;
}



// ==================== Serial buffer helpers ====================
void resetSerialBuffer() {
  sr.incomeBuffer[0] = '\0';
  sr.writeIndex = 0;
  sr.readIndex = SERIAL_BUFFER_ROWS-1;
  sr.lastMessageMS = millis();
  for (uint8_t i = 0; i < SERIAL_BUFFER_ROWS; i++) {
    sr.buffer[i][0] = '\0';
    //sprintf(sr.buffer[i], "tst-%i", i);
  }
  if (wifiON) {
    if (websocketServer.connectedClients(false)) {
      // {"rsT":"out", "rsV":{"outT":"full", "outV":"", "outM":"R"}}
      const int capacityResponseJSON = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + ROW_LENGTH + 10;
      StaticJsonDocument<capacityResponseJSON> responseJSON;    
      responseJSON["rsT"] = "out";
      JsonObject rsV = responseJSON.createNestedObject("rsV");
      rsV["outT"] = "full";
      rsV["outV"] = "";
      rsV["outM"] = "R";
      strVar = "";
      serializeJson(responseJSON, strVar);      
      websocketServer.broadcastTXT(strVar);
    }
  }
}

uint32_t changeBaudRate(uint32_t br) {
  if (br != setting->baudRate) {
    setting->baudRate = br;
    Serial.updateBaudRate(setting->baudRate);
    if (!saveEEPROM()) {
      switchToError("Error during EEPROM saving");
    };
    resetSerialBuffer();
  }

  if (wifiON) {
    if (websocketServer.connectedClients()) {
      //   {"rsT":"bdr", "rsV":9600} - set baudrate - poslat po zmene baud rate
      const int capacityResponseJSON = JSON_OBJECT_SIZE(2) + ROW_LENGTH + 10;
      StaticJsonDocument<capacityResponseJSON> responseJSON;      
      responseJSON["rsT"] = "bdr";
      responseJSON["rsV"] = br;
      strVar = "";
      serializeJson(responseJSON, strVar);
      websocketServer.broadcastTXT(strVar);
    }
    
    #ifdef USE_MQTT
      mqtt_client.publish(MQTT_RSPN_TOPIC, strVar.c_str(), false);
    #endif
  }

  return br;
}

/**
 * msgType - 0==message received over Serial, 1 == message written to serial
 */
uint8_t addToSerialBuffer(char *msg, uint8_t msgType = 0) {
  uint8_t msgLen = strlen(msg);
  strcpy(sr.buffer[sr.writeIndex], msg);                  // copy the new text to serial buffer
  msg[0] = '\0';
  uint8_t idx = sr.writeIndex;
  if (programState != PROGRAM_SCROLLING) sr.readIndex = idx;
  sr.writeIndex = (sr.writeIndex+1)%SERIAL_BUFFER_ROWS;
  if (wifiON) {
    if (websocketServer.connectedClients()) {
      //{"rsT":"out", "rsV":{"outT":"inc", "outV":"text to add", "outM":"R"}}
      const int capacityResponseJSON = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + ROW_LENGTH+10;
      StaticJsonDocument<capacityResponseJSON> responseJSON;      
      responseJSON["rsT"] = "out";
      JsonObject rsV = responseJSON.createNestedObject("rsV");
      rsV["outT"] = "inc";
      rsV["outV"] = sr.buffer[idx];
      rsV["outM"] = (msgType)?"W":"R";
      strVar = "";
      serializeJson(responseJSON, strVar);
      websocketServer.broadcastTXT(strVar);
    } 

    #ifdef USE_MQTT
      mqtt_client.publish(MQTT_RSPN_TOPIC, strVar.c_str(), false);
    #endif
  }  

  return msgLen;
}

uint8_t writeToSerial(char *msg) {
  char txt[ROW_LENGTH+1]; 
  uint8_t msgLen = strlen(msg);
  if (msgLen) {
    while (Serial.availableForWrite() != UART_TX_FIFO_SIZE) yield();
    Serial.write(msg, msgLen);
    Serial.write('\r');
    Serial.write('\n');
    Serial.flush();

    txt[0] = '\0';
    strcpy(txt, "> ");
    strcat(txt, msg);
    msg[0] = '\0';
    addToSerialBuffer(txt, 1);
  }
  return msgLen;
}



// ==================== Display helpers ====================

/**
 * C substring function: It returns a pointer to the substring
 * startPos - from 0 to n-1
 */
char *substring(char *str, uint8_t startPos, uint8_t subLen) {
  char *subStr;
  uint8_t len = subLen;
  uint8_t c; 
  
  if (startPos >= strlen(str)) return NULL;
  if (!len) return NULL;
  
  if ((strlen(str) - startPos + 1) < len) len = strlen(str) - startPos + 1; 
    
  subStr = (char *) malloc((len + 1) * sizeof(char));
  
  if (subStr == NULL) {
    //printf("Unable to allocate memory.\n");
    //exit(1);
    return NULL;
  }
  
  for (c = 0 ; c < len ; c++) *(subStr + c) = *(str + c + startPos);      
  *(subStr + c) = '\0';
  
  return subStr;
}

void showError () {
  char *msg;

  u8g2.setDrawColor(1);
  u8g2.drawBox(15, 15, DISPLAY_WIDTH_PX - 30, DISPLAY_HEIGHT_PX - 30);
  u8g2.setClipWindow(17,17,DISPLAY_WIDTH_PX - 17, DISPLAY_HEIGHT_PX - 17);
  u8g2.setDrawColor(0);
  u8g2.setFontMode(1);

  uint8_t y = 25;
  uint8_t startSub = 0;
  while (msg = substring(errStr, startSub, 22)) {
    u8g2.drawStr(17, y, msg);
    startSub += 22;
    y += 8;
    free(msg);
  }

  u8g2.setMaxClipWindow();
  u8g2.setDrawColor(1);
}


void showSetupError (const char *errMsg) {
  u8g2.clearBuffer();
  
  strcpy(errStr, errMsg);
  showError();
  
  u8g2.sendBuffer();
  uint32_t tm = millis();
  while ((millis()-tm) < SHOW_ERROR_MS) yield();
}


/**
 * rounded == 0 - square box / 1 - rounded box
 * align == 1 - left / 2 - middle
 */
void drawBox(char *label, uint8_t x, uint8_t y, uint8_t width, uint8_t height, uint8_t rounded = 0, uint8_t align = 2) {
  u8g2.setDrawColor(1);                                                               // draw color is "lighted pixel"
  if (rounded) u8g2.drawRBox(x, y, width, height, 2);                                 // rounded box
  else u8g2.drawBox(x, y, width, height);                                             // square box
  u8g2.setFontMode(1);                                                                // do not write background color under text
  u8g2.setDrawColor(0);                                                               // 0 (clear pixel value in the display RAM) - draw color is dark pixel
  if (align == 1) u8g2.drawStr(x+1, y + height - 1, label);                           // left aling text - padding-left 1px
  else u8g2.drawStr(x + width/2 - u8g2.getStrWidth(label)/2, y + height - 1, label);  // center aling text
  u8g2.setDrawColor(1);                                                               // reset of drawing color
}


/**
 * buttonType - 0 = left / 1 = middle / 2 = right
 * button labels:
 *   PROGRAM_RECEIVING - CHNG PG  WRITE  CHNG BDR
 *   PROGRAM_BDR       - < SELECT >
 *   PROGRAM_LETTER    - < SELECT >
 *   PROGRAM_SCROLLING - UP END DOWN
 */
void drawUIButton(uint8_t buttonType) { 
  const uint8_t UIButtonWidth = 42;
  const uint8_t UIButtonHeight = 7;

  uint8_t x, y;
  char label[10];

  y = DISPLAY_HEIGHT_PX - UIButtonHeight;
  x = buttonType*(UIButtonWidth+1);  

  switch (buttonType) {
    case 0: // left button
      switch (programState) {
        case PROGRAM_RECEIVING: sprintf(label, "%s", F("CHNG PG")); break;
        case PROGRAM_SCROLLING: sprintf(label, "%s", F("UP")); break;
        default: sprintf(label, "%s", F("<")); 
      }
      break;
    case 1: // middle button
      switch (programState) {
        case PROGRAM_RECEIVING: sprintf(label, "%s", F("WRITE")); break;
        case PROGRAM_SCROLLING: sprintf(label, "%s", F("END")); break;
        default: sprintf(label, "%s", F("SELECT"));
      }
      break;
    case 2: // right button
      switch (programState) {
        case PROGRAM_RECEIVING: sprintf(label, "%s", F("CHNG BDR")); break;
        case PROGRAM_SCROLLING: sprintf(label, "%s", F("DOWN")); break;
        default: sprintf(label, "%s", F(">"));
      }
      break;
  }

  drawBox( label, x, y, UIButtonWidth, UIButtonHeight, 1, 2);
}


void drawMenu(TKeyboardItem *menu, uint8_t menuItemCount) {
  const uint8_t UIMenuWidth = DISPLAY_WIDTH_PX;
  const uint8_t UIMenuHeight = 7;
  const uint8_t UIMenuPositionX = 0;
  const uint8_t UIMenuPositionY = 48;

  u8g2.setDrawColor(1);                        // draw color is "lighted pixel"
  u8g2.drawBox(UIMenuPositionX, UIMenuPositionY, UIMenuWidth, UIMenuHeight);    // x(vertical), y(horizontal), w, h
  u8g2.setFontMode(1);                         // do not write background color under text
  u8g2.setDrawColor(0);                        // 0 (clear pixel value in the display RAM) - draw color is dark pixel

  uint8_t index = dsp.menuIndex;
  int8_t x = UIMenuWidth/2-1-menu[index].txtWidth/2;
  uint8_t y = UIMenuPositionY + UIMenuHeight - 1;  

  // lets find out the first item and its position, that could be printed out
  while (x > KEYBOARD_OUTER_PADDING_PX) { 
    if (index == 0) index = menuItemCount-1;
    else index--;
    x -= KEYBOARD_ITEM_PADDING_PX + menu[index].txtWidth;
  }
  x += KEYBOARD_ITEM_PADDING_PX + menu[index].txtWidth;
  index = (index+1)%menuItemCount;

  // lets print the menu from left to right with the selected item exactly in the middle
  while ((x+menu[index].txtWidth) < (UIMenuWidth - KEYBOARD_OUTER_PADDING_PX)) {
    if (index == dsp.menuIndex) {                          // selected menu item is displayed differently
      u8g2.setDrawColor(0);
      u8g2.drawBox(x-2, y-6, menu[index].txtWidth + 3, 7);
      u8g2.setDrawColor(1);
      u8g2.drawStr(x, y, menu[index].txt);
      u8g2.setDrawColor(0);
    } else {
      u8g2.drawStr(x, y, menu[index].txt);
    }
    x += menu[index].txtWidth + KEYBOARD_ITEM_PADDING_PX;
    index = (index+1)%menuItemCount;
  }

  u8g2.setDrawColor(1);
}


/**
 * actMs = actual program time - used to check, if display should be refreshed
 */
void processDisplay(uint32_t actMS) {  
  if ((dsp.lastDisplayRefreshMS + DISPLAY_REFRESH_MS) < actMS) {
    char txt[ROW_LENGTH+1];

    u8g2.clearBuffer();
    u8g2.setDrawColor(1);

    // IP box
    if (wifiON) {
      sprintf(txt, "IP: %s", WiFi.localIP().toString().c_str());
    } else {
      sprintf(txt, "Offline");
    }
    drawBox(txt, 0, 0, 80, 7, 0, 1);
    //BDR box
    sprintf(txt, "Bdr: %i", setting->baudRate);
    drawBox(txt, 82, 0, 46, 7, 0, 1);
    
    if (programState == PROGRAM_BDR) {
      drawMenu(baudRateKeyboard, baudRateListCount);    // baud rate menu
    } else if (programState == PROGRAM_LETTER) {
      drawMenu(asciiKeyboard, asciiListCount);          // letter menu
      sprintf(txt, "> %s", sr.sendBuffer);              // input text from user
      u8g2.drawStr(0, 46, txt);
    }

    // draw buttons on the bottom of the screen
    for (uint8_t i = 0; i < 3; i++) drawUIButton(i);
    
    // display serial buffer
    uint8_t displayRows;
    uint8_t y = 55;
    // RECEIVING/SCROLLING - 8rows
    // BDR - 6 rows
    // LETTER - 5 rows
    switch (programState) {
      case PROGRAM_LETTER: displayRows = 5; y-=2*6+3; break;
      case PROGRAM_BDR: displayRows = 6; y-=1*6+3;break;      
      default: displayRows = 8;
    }    

    if ((sr.incomeBuffer[0] > '\0') && (((sr.readIndex+1)%SERIAL_BUFFER_ROWS) == sr.writeIndex)) { // income buffer is not empty and we should display it, if we scroll, then we display only when we are at the end
      u8g2.drawStr(0, y, sr.incomeBuffer);
      y -= 6;
      displayRows--;  
    }

    // display serial buffer
    uint8_t i = sr.readIndex;
    while ((displayRows > 0) && (i != sr.writeIndex)){
      u8g2.drawStr(0, y, sr.buffer[i]);
      i = (i + SERIAL_BUFFER_ROWS - 1)%SERIAL_BUFFER_ROWS;      
      y -= 6;
      displayRows--;
    }

    if (programState == PROGRAM_ERROR) showError();
    
    u8g2.sendBuffer();

    dsp.lastDisplayRefreshMS = actMS;
  }
}


// ==================== Websocket received event processing ====================
// This function is executed when we receive event through Websocket interface
/**
 *   Request messages
       {"rqT":"bdr", "rqV": 9600} - set baudrate
       {"rqT":"inp", "rqV": "text to serial"} - send to serial

     Response messages:
       {"rsT":"err", "rsV":"error text"} - error response
       {"rsT":"out", "rsV":{"outT":"inc", "outV":"text to add", "outM":"R"}} - text to add to serial monitor,
       {"rsT":"out", "rsV":{"outT":"full", "outV":"text to start/refresh", "outM":"R"}} - text to refresh the serial monitor
       {"rsT":"bdr", "rsV":9600} - set baudrate - poslat po zmene baud rate
  */
void websocketProcess(uint8_t num, WStype_t type, uint8_t* data, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress ip = websocketServer.remoteIP(num);
    // {"rsT":"out", "rsV":{"outT":"full", "outV":"", "outM":"R"}}
    // {"rsT":"out", "rsV":{"outT":"inc", "outV":"text to add", "outM":"R"}}
    const int capacityResponseJSON = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) + ROW_LENGTH+10;
    StaticJsonDocument<capacityResponseJSON> responseJSON;
    
    responseJSON["rsT"] = "out";
    JsonObject rsV = responseJSON.createNestedObject("rsV");
    uint8_t firstMsg = 1;
    for (uint8_t i = sr.writeIndex+1; i != sr.writeIndex; i = (i+SERIAL_BUFFER_ROWS+1)%SERIAL_BUFFER_ROWS) {
      if (strlen(sr.buffer[i])) {        
        if (firstMsg) {
          rsV["outT"] = "full";
          firstMsg = 0;
        } else rsV["outT"] = "inc";
        rsV["outV"] = sr.buffer[i];
        rsV["outM"] = "R";
        strVar = "";
        serializeJson(responseJSON, strVar);        
        websocketServer.sendTXT(num,strVar);
      }
      yield();
    }
    //   {"rsT":"bdr", "rsV":9600} - set baudrate - poslat po zmene baud rate
    responseJSON["rsT"] = "bdr";
    responseJSON["rsV"] = setting->baudRate;
    strVar = "";
    serializeJson(responseJSON, strVar);
    websocketServer.sendTXT(num, strVar);
  }
  else if (type == WStype_TEXT) {  // when we receive message from a client
    //Serial.printf("[%u] get Text: %s\n", num, data);

    const int capacityRequestJSON = JSON_OBJECT_SIZE(2) + 40;
    StaticJsonDocument<capacityRequestJSON> requestJSON;

    // parse incoming JSON message
    DeserializationError err = deserializeJson(requestJSON, data);
    if (err) {
      switchToError("Error during JSON parsing");
      return;
    }
    yield();

    if (requestJSON["rqT"] == "bdr") { // set baud rate
      // ###########################
      // ## bdr
      // ###########################
      const uint32_t br = requestJSON["rqV"];
      changeBaudRate(br);
    } else if (requestJSON["rqT"] == "inp") { // send to serial
      // ###########################
      // ## inp
      // ###########################
      const char *msg = requestJSON["rqV"];
      char txt[ROW_LENGTH+1];
      strncpy(txt, msg, ROW_LENGTH);
      writeToSerial(txt);
    } else {
      websocketServer.sendTXT(num, "ERROR - Received not recognised request");
      switchToError("Error ws unknown request");
    }
  } 
}



// ==================== Web server help functions ====================
// Return content type of a file
String getContentType(String filename) { // convert the file extension to the MIME type
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

// Load file from SPIFFS and return it through the web server
bool handleFileRead(String path) { // send the right file to the client (if it exists)
  if (path.endsWith("/")) path += "index.html";          // If a folder is requested, send the index file
  String contentType = getContentType(path);             // Get the MIME type
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
    if (SPIFFS.exists(pathWithGz))                         // If there's a compressed version available
      path += ".gz";                                         // Use the compressed version
    yield();
    File file = SPIFFS.open(path, "r");                    // Open the file
    size_t sent = server.streamFile(file, contentType);    // Send it to the client
    file.close();                                          // Close the file again
    return true;
  }
  return false;                                          // If the file doesn't exist, return false
}



// ==================== MQTT API ====================
#ifdef USE_MQTT
// Returns last index of x if it is present.
// Else returns -1.
uint8_t findLastIndex(char *str, char x) {
  uint8_t len = strlen(str);
  // Traverse from right
  for (uint8_t i = len - 1; i >= 0; i--)
    if (str[i] == x) return i;
  return -1;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // payload is without ending \0
  char payloadMessage[MQTT_MAX_PAYLOAD_LENGTH];
  uint8_t payloadLength = (length > (MQTT_MAX_PAYLOAD_LENGTH-1)) ? (MQTT_MAX_PAYLOAD_LENGTH-1) : length;
  memcpy(payloadMessage, payload, payloadLength);
  payloadMessage[payloadLength] = '\0';


  uint8_t lastToken = findLastIndex(topic, '/');
  if ((lastToken == -1) || ((strlen(topic) - (lastToken+1)) > 6)) {
    mqtt_client.publish(MQTT_LAST_WILL_TOPIC, "ERROR MQTT wrong parameters for topics", false);
    switchToError("ERROR MQTT wrong parameters");
    return;
  }
  char cmdType[10];
  strncpy(cmdType, &topic[lastToken + 1], 4);

  if (strcmp(cmdType, "inp") == 0) {
    // ###########################
    // ## inp
    // ###########################
    writeToSerial(payloadMessage);

  } else if (strcmp(cmdType, "bdr") == 0) {
    // ###########################
    // ## bdr
    // ###########################
    const uint32_t br = atoi(payloadMessage);
    changeBaudRate(br);

  } else {
    mqtt_client.publish(MQTT_LAST_WILL_TOPIC, "ERROR - Unknown MQTT command, supported: inp, bdr", false);
    switchToError("Error unknown MQTT command");
  }
}

void mqttConnect() {
  mqtt_client.setServer(MQTT_SERVER, 1883);
  mqtt_client.setCallback(mqttCallback);

  uint8_t i = 0;
  while (!mqtt_client.connected()) {
    i++;
    //boolean connect(const char* id, const char* user, const char* pass, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage);
    if (!mqtt_client.connect(ESP_HOSTNAME, MQTT_USERNAME, MQTT_PASSWORD, MQTT_LAST_WILL_TOPIC, 0, 1, MQTT_LAST_WILL)) {      
      if (i <= 10) {
        delay(500);                        // Wait 500ms before retrying
      } else {
        showSetupError("Error during connecting to MQTT");
        ESP.reset();                       // If something fails, restart ESP8266
        delay(1000);
      }
    }
  }
  mqtt_client.publish(MQTT_LAST_WILL_TOPIC, "ON", false);
  mqtt_client.subscribe(MQTT_CMD_TOPIC "/#");
}
#endif



// ==================== Setup ====================
void setup() {
  Serial.begin(DEFAULT_BAUD_RATE);
  while (!Serial) delay(50);  
  Serial.println();
  Serial.println(F("Serial monitor"));
  Serial.print(F("version: "));
  Serial.println(VERSION);

  u8g2.begin(PIN_BUTTON_OK, PIN_BUTTON_RIGHT, PIN_BUTTON_LEFT);  // menu_select_pin, menu_next_pin, menu_prev_pin
  u8g2.setFont(u8g2_font_4x6_mf);
  dsp.characterWidth = u8g2.getStrWidth("0");                    // all letters have the same width

  // setum menu items - for baud rates and ascii letter menu
  char txt[MENU_ITEM_MAX_LENGTH]; 
  baudRateListCount = ARRAY_SIZE(baudRateList);
  asciiListCount = ARRAY_SIZE(asciiList);

  // initialize baud rate list/menu
  baudRateKeyboard = (TKeyboardItem *) malloc(sizeof(TKeyboardItem) * baudRateListCount);
  for (uint8_t i = 0; i < baudRateListCount; i++) {
    sprintf(baudRateKeyboard[i].txt, "%i", baudRateList[i]);
    baudRateKeyboard[i].txtWidth = dsp.characterWidth * strlen(baudRateKeyboard[i].txt);
  }

  // initialize letter list/menu
  asciiKeyboard = (TKeyboardItem *) malloc(sizeof(TKeyboardItem) * asciiListCount);
  for (uint8_t i = 0; i < asciiListCount; i++) {
    if (asciiList[i] > '\x20') {
      asciiKeyboard[i].txt[0] = asciiList[i];
      asciiKeyboard[i].txt[1] = '\0';
      asciiKeyboard[i].txtWidth = dsp.characterWidth;
    } else {
      //space - 32	20	SP (space); DEL - 8	08	BS	Backspace; CAN - 24	18	CAN	Cancel; SND - 4	04	EOT	End of Transmission
      switch (asciiList[i]) {
        case '\x20': strcpy(asciiKeyboard[i].txt, "SPACE"); break;
        case '\x08': strcpy(asciiKeyboard[i].txt, "DELETE"); break;
        case '\x18': strcpy(asciiKeyboard[i].txt, "CANCEL"); break;
        case '\x04': strcpy(asciiKeyboard[i].txt, "ENTER"); break;
        default: strcpy(asciiKeyboard[i].txt, "ERROR");
      }
      asciiKeyboard[i].txtWidth = dsp.characterWidth * strlen(asciiKeyboard[i].txt);
    }
  }
  
  // initialize global variables
  programState = PROGRAM_RECEIVING;  
  programMS = millis();
  lastActionMS = 0;
  lastMenuAction = 0;
  // display setting
  dsp.lastDisplayRefreshMS = 0;
  dsp.menuIndex = 0;
  dsp.menuStep = 1;
  // serial buffer setting
  resetSerialBuffer();

  // test if user wants to connect to wifi or be offline
  uint8_t userInput = u8g2.userInterfaceMessage("ESP8266 Serial Monitor v" VERSION, "Connect to wifi", "or work offline?", " online \n offline ");
  if (userInput == 1) {   // connect to wifi
    // Start WiFiManager, that handles connection to the WIFI network
    WiFi.hostname(ESP_HOSTNAME);
    WiFiManager wm;
    // IP parameters of WIFI network when WIfi Manager starts in AP mode - custom ip /gateway /subnet configuration    
    wm.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));  // This will set your captive portal to a specific IP should you need/want such a feature. Add the following snippet before autoConnect()
    wm.setAPCallback(startWMConfig);           // Use this if you need to do something when your device enters configuration mode on failed WiFi connection attempt. Before autoConnect()
    // set IP after logon to wifi (instead of DHCP) - custom ip /gateway /subnet configuration
    //wifiManager.setSTAStaticIPConfig(IPAddress(192,168,0,99), IPAddress(192,168,0,1), IPAddress(255,255,255,0));

    // Connect to WIFI
    // If there are no WIFI access information in the memory or the board is out of reach of saved WIFI
    // start own AP and the user must connect to this AP and sets up new WIFI connection
    if (!wm.autoConnect(ESP_HOSTNAME)) { // name of the AP WIFI, wifiManager.autoConnect("AutoConnectAP", "password")
      showSetupError("Error cant connect to Wifi");
      wifiON = 0;
    } else {
      wifiON = 1;
    }
  } else {                // offline
    wifiON = 0;
  }
  if (!wifiON) WiFi.mode(WIFI_OFF);


  // Load settings from EEPROM and set baud rate
  setting = &eeprom.setting;   // shotcut to global program settings

  EEPROM.begin(sizeof(TEeprom));
  if (EEPROM.percentUsed() >= 0) {
    EEPROM.get(0, eeprom);
    uint32_t crc = calculateCRC32( ((uint8_t*) &eeprom) + 4, sizeof( eeprom ) - 4 );
    if ( crc != eeprom.crc32 ) {
      showSetupError("Error CRC32, resetting EEPROM");
      resetEEPROM();
    }
  } else {
    showSetupError("Error EEPROM, resetting");
    resetEEPROM();
  }
  Serial.updateBaudRate(setting->baudRate);
  Serial.setTimeout(300);

  if (wifiON) {
    // reserve memory for long strings
    strVar.reserve(512);

    if (!SPIFFS.begin()) {
      showSetupError("Error SPIFFS not loaded");
      ESP.reset();                       // If something fails, restart ESP8266
      delay(1000);
    }

    // Start web server that will send the client documents from SPIFFS
    server.onNotFound([]() {                              // If the client requests any URI
      if (!handleFileRead(server.uri()))                  // send it if it exists
        server.send(404, "text/plain", "404: Not Found"); // otherwise, respond with a 404 (Not Found) error
    });
    server.begin();

    // Start websocket process
    // when someone connect to the websocket, the function websocketProcess will handle the request
    websocketServer.onEvent(websocketProcess);
    websocketServer.begin();

    #ifdef USE_MQTT
      // Connect to MQTT broker
      mqttConnect();
    #endif
  }
}


void loop(void) {
  programMS = millis();

  if (wifiON) {
    // Handle HTTP communication
    server.handleClient();
    yield();
    // Hanlde websocket communication
    websocketServer.loop();
    yield();

    #ifdef USE_MQTT
      if (!mqtt_client.connected()) {
        switchToError("Error MQTT disconnected");
        mqttConnect();
      }
      mqtt_client.loop();
      yield();
    #endif
  }

  // store timeouted incoming serial communication
  if ((strlen(sr.incomeBuffer)) && ((programMS - sr.lastMessageMS) > SERIAL_MESSAGE_TIMEOUT)) {
    addToSerialBuffer(sr.incomeBuffer, 0);
  }

  // process incoming serial communication
  if (Serial.available()) {
    uint8_t buffLen = strlen(sr.incomeBuffer);
    int i;
    char c;
    while ((i = Serial.read()) > -1) {
      c = (char) i;      
      if (c >= 32) {
        sr.incomeBuffer[buffLen] = c;
        buffLen++;
        sr.incomeBuffer[buffLen] = '\0';
      }     
      if ((buffLen == ROW_LENGTH) || ((c < 32) && buffLen)){
        addToSerialBuffer(sr.incomeBuffer, 0);
        buffLen = 0;
      }
    }
    sr.lastMessageMS = programMS;
  }

  // test if there is any user input
  uint8_t menuAction = u8g2.getMenuEvent();
  if (menuAction) {
    if (menuAction == lastMenuAction) {                    // if user pressed the same button again, we check, if he clicks fast and if so, we increase the step to traverse the menu
      if (((programMS - lastActionMS) < FAST_CLICKING_MS) && (dsp.menuStep < 8) && (menuAction != U8X8_MSG_GPIO_MENU_SELECT))  {
         dsp.menuStep *= 2;
      } else {
        dsp.menuStep = 1;
      }
    } else {
      lastMenuAction = menuAction;
      dsp.menuStep = 1;
    }    
    lastActionMS = programMS;
  }

  if (menuAction) {
    if (programState == PROGRAM_RECEIVING) {
        uint8_t i;
        switch (menuAction) {
          case U8X8_MSG_GPIO_MENU_SELECT:
            programState = PROGRAM_LETTER;
            // set the right dsp.menuIndex
            dsp.menuIndex = 0;
            break;
          case U8X8_MSG_GPIO_MENU_NEXT:
            programState = PROGRAM_BDR;
            // set the right dsp.menuIndex
            i = 0;
            while ((i < baudRateListCount) && (baudRateList[i] != setting->baudRate)) {
              i++;
            };
            dsp.menuIndex = i;
            break;
          case U8X8_MSG_GPIO_MENU_PREV:
            programState = PROGRAM_SCROLLING;
            break;
        }
    } else if (programState == PROGRAM_BDR) {
        switch (menuAction) {
          case U8X8_MSG_GPIO_MENU_NEXT: 
            dsp.menuIndex = (dsp.menuIndex+dsp.menuStep)%baudRateListCount;
            break;
          case U8X8_MSG_GPIO_MENU_PREV: 
            dsp.menuIndex = (dsp.menuIndex + baudRateListCount - dsp.menuStep)%baudRateListCount;
            break;
          case U8X8_MSG_GPIO_MENU_SELECT:
            changeBaudRate(baudRateList[dsp.menuIndex]);
            programState = PROGRAM_RECEIVING;
            break;
        }
    } else if (programState == PROGRAM_LETTER) { //  asciiList '\x20','\x8','\x18','\x4'};      // space - 32	20	SP (space); DEL - 8	08	BS	Backspace; CAN - 24	18	CAN	Cancel; SND - 4	04	EOT	End of Transmission      
      uint8_t len = strlen(sr.sendBuffer);
      switch (menuAction) {
        case U8X8_MSG_GPIO_MENU_NEXT: 
          dsp.menuIndex = (dsp.menuIndex+dsp.menuStep)%asciiListCount;
          break;
        case U8X8_MSG_GPIO_MENU_PREV: 
          dsp.menuIndex = (dsp.menuIndex + asciiListCount - dsp.menuStep)%asciiListCount;
          break;
        case U8X8_MSG_GPIO_MENU_SELECT:
          if (asciiList[dsp.menuIndex] == '\x18') {  // CANCEL
            // reset input buffer
            sr.sendBuffer[0] = '\0';
            programState = PROGRAM_RECEIVING;
          } else if (asciiList[dsp.menuIndex] == '\x4') {  // SEND
            writeToSerial(sr.sendBuffer);
            programState = PROGRAM_RECEIVING;
          } else if (asciiList[dsp.menuIndex] == '\x8') {  // DELETE
            if (len) sr.sendBuffer[len-1] = '\0';
          } else {
            if (len < (ROW_LENGTH+1-2-1)) {                  // buffer is not full
              sr.sendBuffer[len] = asciiList[dsp.menuIndex];
              sr.sendBuffer[len+1] = '\0';
            }
          }
      }
    } else if (programState == PROGRAM_SCROLLING) {
      uint8_t newReadIndex;
      uint8_t readIndex = sr.readIndex + SERIAL_BUFFER_ROWS;
      uint8_t writeIndex = sr.writeIndex + SERIAL_BUFFER_ROWS;
      switch (menuAction) {
        case U8X8_MSG_GPIO_MENU_SELECT:
          sr.readIndex = (sr.writeIndex + SERIAL_BUFFER_ROWS - 1)%SERIAL_BUFFER_ROWS;
          programState = PROGRAM_RECEIVING;
          break;
        case U8X8_MSG_GPIO_MENU_NEXT: 
          // one page = 8 rows
          
          newReadIndex = readIndex + 7;
          if (((readIndex < writeIndex) && (newReadIndex >= writeIndex)) || ((readIndex > writeIndex) && (newReadIndex > (writeIndex+SERIAL_BUFFER_ROWS)))) sr.readIndex = (sr.writeIndex + SERIAL_BUFFER_ROWS - 1)%SERIAL_BUFFER_ROWS;
          else sr.readIndex = newReadIndex%SERIAL_BUFFER_ROWS;  // readIndex is right at the end
          break;
        case U8X8_MSG_GPIO_MENU_PREV: 
          // one page = 8 rows
          newReadIndex = readIndex - 7;
          if ((readIndex >= (writeIndex+7))&&(newReadIndex <= (writeIndex+7))) sr.readIndex = (sr.writeIndex+7)%SERIAL_BUFFER_ROWS;
          else if (strlen(sr.buffer[newReadIndex%SERIAL_BUFFER_ROWS])) sr.readIndex = newReadIndex%SERIAL_BUFFER_ROWS;  // readIndex is right at the end
          break;
      }
    }
  }
  
  if ((programState == PROGRAM_ERROR) && ((programMS - errorEnterMS) > SHOW_ERROR_MS)) {
    programState = beforeErrorState;
    errStr[0] = '\0';
  }

  processDisplay(programMS);
}