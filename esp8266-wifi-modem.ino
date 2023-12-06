#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SPI.h>
#include <U8x8lib.h>
#include <SoftwareSerial.h>

WiFiClient wifi_client;
HTTPClient http_client;

#define CONSOLE Serial
#define CONSOLE_BAUD_RATE 115200

#define APPLE_TX_PIN 4  // D2(GPIO-04)
#define APPLE_RX_PIN 5  // D1(GPIO-05)
#define APPLE_BAUD_RATE 4800
SoftwareSerial mySerial(APPLE_RX_PIN, APPLE_TX_PIN);
#define APPLE mySerial

enum Mode {
  UNKNOWN_MODE,
  COMMAND_MODE,
  DATA_MODE,
};

int mode = UNKNOWN_MODE;

#define ESCAPE_SEQ_MAX 3
const char ESCAPE_SEQ[ESCAPE_SEQ_MAX] = { '+', '+', '+' };
int escape_seq_pos = 0;

#define ECHO_MIXED_HEX_PREFIX "\\x"
#define ECHO_MIXED_HEX_SUFFIX ""

enum Echo {
  ECHO_MIXED,
  ECHO_TEXT,
  ECHO_BINARY,
};

int echo = ECHO_MIXED;

uint32 inbound_count = 0;
uint32 outbound_count = 0;

uint32 start_time = 0;
uint32 last_time = 0;
uint32 last_duration = 0;

void setup() {
  CONSOLE.begin(CONSOLE_BAUD_RATE);

  pinMode(APPLE_TX_PIN, OUTPUT);
  pinMode(APPLE_RX_PIN, INPUT);
  APPLE.begin(APPLE_BAUD_RATE);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  init_display();

  while (!APPLE.isListening()) {
    CONSOLE.print(".");
    delay(500);
  }

  start_time = millis();
  set_command_mode();
}

void loop() {
  uint32 now = millis();
  if (last_duration) {
    last_duration = now - last_time;
  }
  last_time = now;

  switch (mode) {
    case COMMAND_MODE:
      handle_command_mode(CONSOLE);
      handle_command_mode(APPLE);
      break;
    case DATA_MODE:
      handle_data_mode();
      break;
    default:
      // unknown mode!
      break;
  }
}

void handle_command_mode(Stream &stream) {
  if (stream.available()) {
    String s = read_line(stream);
    if (s.length() == 0) {
      return;
    }
    if (s == "ping") {
      stream.println("pong!");
    } else if (s == "scan") {
      handle_scan_command(stream);
    } else if (s == "connect") {
      // TODO: parse args
      handle_connect_command(stream, "준우네", "1234567890");
    } else if (s == "disconnect") {
      handle_disconnect_command(stream);
    } else if (s == "open") {
      // TODO: parse args
      handle_open_command(stream, "example.com", 80);
    } else if (s == "close") {
      handle_close_command(stream);
    } else if (s == "data") {
      set_data_mode();
    } else if (s == "http") {
      // TODO: parse args
      handle_http_command(stream, "http://example.com");
    } else if (s == "time") {
      handle_time_command(stream);
    } else if (s == "mixed") {
      echo = ECHO_MIXED;
    } else if (s == "text") {
      echo = ECHO_TEXT;
    } else if (s == "binary") {
      echo = ECHO_BINARY;
    } else if (s == "status") {
      handle_status_command(stream);
    } else if (s == "help") {
      handle_help_command(stream);
    } else {
      stream.print("invalid command:");
      stream.print(s);
      stream.println();
    }
  }
}

void handle_data_mode() {
  // console(nodemcu) --> wifi
  while (CONSOLE.available()) {
    int c = CONSOLE.read();
    if (handle_escape_seq(CONSOLE, c)) { return; }
    wifi_client.write(c);
    outbound_count++;
    handle_echo(c);
  }

  // if (WiFi.status() != WL_CONNECTED) {
  //   CONSOLE.println("data mode but not connected!");
  //   delay(1000);
  //   // TODO: try to resume?
  //   return;
  // }

  // if (!wifi_client.connected()) {
  //   CONSOLE.println("data mode but not opened!");
  //   delay(1000);
  //   // TODO: try to resume?
  //   return;
  // }

  // rs232(apple) -> wifi
  while (APPLE.available()) {
    int c = APPLE.read();
    if (handle_escape_seq(APPLE, c)) { return; }
    wifi_client.write(c);
    outbound_count++;
    handle_echo(c);
  }

  // wifi --> rs232(apple)
  while (wifi_client.available()) {
    int c = wifi_client.read();
    APPLE.write(c);
    inbound_count++;
    handle_echo(c);
  }

  display_status(false);
}

//
// data mode
//

bool handle_escape_seq(Stream& stream, char c) {
  if (c == ESCAPE_SEQ[escape_seq_pos]) {
    if (++escape_seq_pos == ESCAPE_SEQ_MAX) {  // triple consequence '+'
      stream.println("escape sequence detected!");
      set_command_mode();
      escape_seq_pos = 0;
      return true;
    }
  } else {
    escape_seq_pos = 0;
  }
  return false;
}

void handle_echo(char c) {
  switch (echo) {
    case ECHO_MIXED:
      print_mixed(c);
      break;
    case ECHO_TEXT:
      print_text(c);
      break;
    case ECHO_BINARY:
      print_binary(c);
      break;
  }
}

//
// command mode
//

void handle_scan_command(Stream &stream) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  stream.println();
  stream.println("Scan WIFI networks...");

  String ssid;
  int32_t rssi;
  uint8_t encryptionType;
  uint8_t *bssid;
  int32_t channel;
  bool hidden;
  int scanResult = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);

  if (scanResult == 0) {
    stream.println(F("No networks found"));
  } else if (scanResult > 0) {
    stream.printf(PSTR("%d networks found:\n"), scanResult);

    // Print unsorted scan results
    for (int8_t i = 0; i < scanResult; i++) {
      WiFi.getNetworkInfo(i, ssid, encryptionType, rssi, bssid, channel, hidden);

      // get extra info
      const bss_info *bssInfo = WiFi.getScanInfoByIndex(i);
      String phyMode;
      const char *wps = "";
      if (bssInfo) {
        phyMode.reserve(12);
        phyMode = F("802.11");
        String slash;
        if (bssInfo->phy_11b) {
          phyMode += 'b';
          slash = '/';
        }
        if (bssInfo->phy_11g) {
          phyMode += slash + 'g';
          slash = '/';
        }
        if (bssInfo->phy_11n) {
          phyMode += slash + 'n';
        }
        if (bssInfo->wps) {
          wps = PSTR("WPS");
        }
      }
      stream.printf(PSTR("  %02d: [CH %02d] [%02X:%02X:%02X:%02X:%02X:%02X] %ddBm %c %c %-11s %3S %s\n"), i, channel, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], rssi, (encryptionType == ENC_TYPE_NONE) ? ' ' : '*', hidden ? 'H' : 'V', phyMode.c_str(), wps, ssid.c_str());
      yield();
    }
  } else {
    stream.printf(PSTR("WiFi scan error %d"), scanResult);
  }

  delay(5000);
}

void handle_connect_command(Stream &stream, const char *ssid, const char *password) {
  stream.println();
  stream.print(F("WiFi connecting... ssid="));
  stream.print(ssid);
  stream.println();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    stream.print(".");
  }

  stream.println();
  stream.println(F("WiFi connected! ip="));
  stream.print(WiFi.localIP());
  stream.println();

  delay(1000);
}

void handle_disconnect_command(Stream &stream) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  stream.println();
  stream.println(F("WiFi disconnected!"));

  delay(1000);
}

void handle_open_command(Stream &stream, const char *host, int port) {
  stream.print("opening...: host=");
  stream.print(host);
  stream.print(",port=");
  stream.print(port);
  stream.println();

  if (!wifi_client.connect(host, port)) {
    stream.println("open failed!");
    delay(5000);
    return;
  }

  stream.print("open ok: remoteIP=");
  stream.print(wifi_client.remoteIP().toString().c_str());
  stream.print(",port=");
  stream.print(wifi_client.remotePort());
  stream.print(",connected=");
  stream.print(wifi_client.connected());
  stream.println();

#if 0
  //--------------
  wifi_client.print("GET / HTTP/1.0\r\nHost:example.com\r\nConnection:close\r\n\r\n");
  stream.println("receiving from remote server");
#define TCP_READ_TIMEOUT (10 * 1000)
  uint32 timeout = millis();
  do {
    while (!wifi_client.available()) {
      if (millis() - timeout > TCP_READ_TIMEOUT) {
        stream.println("tcp read timeout !");
        wifi_client.stop();
        return;
      }
    }
    while (wifi_client.available()) {
      char c = wifi_client.read();
      stream.print(c);
    }
  } while (wifi_client.connected());
  wifi_client.stop();
  stream.println("------------------------------");
  stream.println("tcp connection closed!");
  stream.println("------------------------------");
  stream.println();
  //--------------
#endif

  set_data_mode();
}

void handle_close_command(Stream &stream) {
  if (!wifi_client.connected()) {
    stream.println("not yet opened!");
    return;
  }

  wifi_client.stop();
  stream.println("closed!");

  set_command_mode();
}

void handle_http_command(Stream &stream, const char *url) {
  stream.print("http: url=");
  stream.print(url);
  stream.println();
  if (http_client.begin(wifi_client, url)) {
    int httpCode = http_client.GET();
    stream.print("http status=");
    stream.print(httpCode);
    stream.println();
    String payload = http_client.getString();
    stream.println(payload);
    stream.println();
    http_client.end();
  } else {
    stream.println("http error!");
  }
  delay(1000);
}

void handle_time_command(Stream& stream) {
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  stream.print("Waiting for NTP time sync: ");
  static time_t now;
  time(&now);
  while (now < 8 * 3600 * 2) {
    delay(1000);
    stream.print(".");
    static time_t now;
    time(&now);
  }
  static char time_buf[sizeof "2017-06-22T10:20:30Z"];
  strftime(time_buf, sizeof time_buf, "%FT%TZ", gmtime(&now));
  stream.println();
  stream.print("time: ");
  stream.println(time_buf);
}

void handle_status_command(Stream &stream) {
  stream.println("------------------------------");
  stream.println("status");
  stream.println("------------------------------");
  stream.print("ssid:");
  stream.print(WiFi.SSID().c_str());
  stream.println();
  stream.print("ip:");
  stream.print(WiFi.localIP().toString().c_str());
  stream.println();
  stream.print("inbound:");
  stream.print(inbound_count);
  stream.println();
  stream.print("outbound:");
  stream.print(outbound_count);
  stream.println();
  stream.print("uptime:");
  stream.print((uint32)((last_time - start_time) / 1000));
  stream.println();

  display_status(true);
}

void handle_help_command(Stream &stream) {
  stream.println("------------------------------");
  stream.println("help");
  stream.println("------------------------------");
  stream.println("connect <ssid> <pwd>");
  stream.println("disconnect");
  stream.println("open <host> <port>");
  stream.println("close");
  stream.println("data");
  stream.println("http <url>");
  stream.println("time");
  stream.println("status");
  stream.println("help");
}

//
//
//

void set_data_mode() {
  if (mode == DATA_MODE) {
    CONSOLE.println("already data mode!");
    return;
  }

  // FIXME: unexpected behaviour of WiFiClient.connected()
  // if (!wifi_client.connected()) {
  //   CONSOLE.println("not yet connected!");
  //   return;
  // }

  mode = DATA_MODE;

  CONSOLE.println();
  CONSOLE.println("==============================");
  CONSOLE.println("data mode");
  CONSOLE.println("==============================");
  CONSOLE.println();

  display_status(true);
}

void set_command_mode() {
  if (mode == COMMAND_MODE) {
    CONSOLE.println("already command mode!");
    return;
  }

  // FIXME: unexpected behaviour of WiFiClient.connected()
  // if (wifi_client.connected()) {
  //   CONSOLE.println("still connected! use 'data' command to return to data mode ");
  // }

  mode = COMMAND_MODE;

  CONSOLE.println();
  CONSOLE.println("==============================");
  CONSOLE.println("command mode");
  CONSOLE.println("==============================");
  CONSOLE.println();

  display_status(true);
}

//
// lcd
//

#define I2C_CLOCK_PIN 14
#define I2C_DATA_PIN 12
U8X8_SSD1306_128X64_NONAME_SW_I2C u8x8(I2C_CLOCK_PIN, I2C_DATA_PIN, U8X8_PIN_NONE);

#define DISPLAY_STATUS_INTERVAL (1 * 1000)
uint32 last_display_status_time = 0;

void init_display() {
  u8x8.begin();
  u8x8.setPowerSave(0);
}

void display_status(bool refresh) {
  if (refresh) {
    u8x8.clear();
    u8x8.setFont(u8x8_font_amstrad_cpc_extended_r);
    u8x8.setInverseFont(1);
    u8x8.drawString(0, 0, "ESP8266WiFiModem");
    u8x8.setInverseFont(0);
    last_display_status_time = 0;
  }
  if (last_time - last_display_status_time < DISPLAY_STATUS_INTERVAL) return;
  switch (mode) {
    case COMMAND_MODE:
      display_command_mode_status(refresh);
      break;
    case DATA_MODE:
      display_data_mode_status(refresh);
      break;
    default:
      display_welcome(refresh);
      break;
  }
  display_uptime(refresh);
  last_display_status_time = last_time;
}

void display_welcome(bool refresh) {
  if (refresh) {
    u8x8.setInverseFont(0);
  } else {
    u8x8.setInverseFont(1);
  }
  u8x8.drawString(0, 1, "Ready!");
}

void display_uptime(bool refresh) {
  if (refresh) {
    u8x8.drawString(0, 6, "uptime:");
  }
  u8x8.clearLine(7);
  u8x8.drawString(0, 7, String((last_time - start_time) / 1000).c_str());
}

void display_data_mode_status(bool refresh) {
  if (refresh) {
    u8x8.setInverseFont(1);
    u8x8.drawString(0, 1, "[DATA MODE]");
    u8x8.setInverseFont(0);
    u8x8.drawString(0, 2, "inbound:");
    u8x8.drawString(0, 4, "outbound:");
  }
  u8x8.clearLine(3);
  u8x8.drawString(0, 3, String(inbound_count).c_str());
  u8x8.clearLine(5);
  u8x8.drawString(0, 5, String(outbound_count).c_str());
}

void display_command_mode_status(bool refresh) {
  if (refresh) {
    u8x8.drawString(0, 1, "[COMMAND MODE]");
    u8x8.setInverseFont(0);
    u8x8.drawString(0, 2, "ssid:");
    u8x8.drawString(0, 4, "ip:");
  }
  u8x8.clearLine(3);
  u8x8.clearLine(5);
  if (WiFi.status() == WL_CONNECTED) {
    u8x8.drawString(0, 3, WiFi.SSID().c_str());
    u8x8.drawString(0, 5, WiFi.localIP().toString().c_str());
  }
}

//
// utils
//

void print_mixed(char c) {
  if (c >= 0x20 && c < 0x80) {
    CONSOLE.print(c);
  } else {
    CONSOLE.print(ECHO_MIXED_HEX_PREFIX);
    CONSOLE.print(c, HEX);
    CONSOLE.print(ECHO_MIXED_HEX_SUFFIX);
  }
  if (c == '\n') {
    CONSOLE.println();
  }
}

void print_text(char c) {
  CONSOLE.print(c);
}

void print_binary(char c) {
  static int hex_cols = 0;
  if (c < 0x10) {
    CONSOLE.print("0");
  }
  CONSOLE.print(c, HEX);
  if (++hex_cols == 16) {
    hex_cols = 0;
    CONSOLE.print('\n');
  } else {
    CONSOLE.print(' ');
  }
}

String read_line(Stream &stream) {
  static char buf[100];
  int len = stream.readBytesUntil('\n', buf, sizeof(buf));
  if (len > 0 && buf[len - 1] == '\r') {
    len--;
  }
  buf[len] = 0;
  return String(buf);
}
