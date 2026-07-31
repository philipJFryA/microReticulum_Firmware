# 1 "/tmp/tmp2kmdeb0u"
#include <Arduino.h>
# 1 "/config/Desktop/microReticulum_Firmware/RNode_Firmware.ino"
# 17 "/config/Desktop/microReticulum_Firmware/RNode_Firmware.ino"
#ifdef HAS_RNS
#include <microReticulum.h>
#include "Provisioning.h"
#if defined(LORA_TRANSPORT)
#include "LoRaInterface.h"
#endif
#if defined(UDP_TRANSPORT)
#include "UDPInterface.h"
#endif
#ifdef URTN_STATS_PAGES
#include "Pages.h"
#endif
#endif
#ifdef HAS_GPIO
#include "GPIO.h"
#endif
#ifdef HAS_BME
#include "BME680.h"
#endif

#include <Arduino.h>
#include <SPI.h>
#include "Utilities.h"
#include "DeviceUID.h"
#include "Platform.h"
#include "WebSocketConsole.h"

#if BOARD_MODEL == BOARD_TDECK
    #include "ui/UISystem.h"
  #endif

#if MODEM == MODEM_RUNTIME
#include "native/LoRaFactory.h"
#include "native/PinMap.h"
#include "native/config.h"
#endif


#if HAS_SDCARD
#include <SD.h>
SPIClass SDSPI(HSPI);
#endif

#if MCU_VARIANT == MCU_ESP32
  #include <esp_task_wdt.h>
#endif


#define WDT_TIMEOUT 60

FIFOBuffer serialFIFO;
uint8_t serialBuffer[CONFIG_UART_BUFFER_SIZE+1];




extern "C" void serial_fifo_push(uint8_t byte) {
  if (!fifo_isfull(&serialFIFO)) fifo_push(&serialFIFO, byte);
}

FIFOBuffer16 packet_starts;
uint16_t packet_starts_buf[CONFIG_QUEUE_MAX_LENGTH+1];

FIFOBuffer16 packet_lengths;
uint16_t packet_lengths_buf[CONFIG_QUEUE_MAX_LENGTH+1];

uint8_t packet_queue[CONFIG_QUEUE_SIZE];

volatile uint8_t queue_height = 0;
volatile uint16_t queued_bytes = 0;
volatile uint16_t queue_cursor = 0;
volatile uint16_t current_packet_start = 0;
volatile bool serial_buffering = false;
#if HAS_BLUETOOTH || HAS_BLE == true
  bool bt_init_ran = false;
#endif

#if PLATFORM == PLATFORM_NATIVE
bool kiss_framed_logs = false;
#else
bool kiss_framed_logs = true;
#endif
bool nomadnet_enabled = true;
RNS::Destination nomadnet_destination = {RNS::Type::NONE};
char nomadnet_name[64];

#if HAS_CONSOLE
  #include "Console.h"
#endif

#if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52 || PLATFORM == PLATFORM_NATIVE
  #define MODEM_QUEUE_SIZE 8
  typedef struct {
          size_t len;
          int rssi;
          int snr_raw;
          uint8_t data[];
  } modem_packet_t;
  static xQueueHandle modem_packet_queue = NULL;
#endif

char sbuf[128];

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
  bool packet_ready = false;
#endif

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
void update_csma_parameters();
#endif


void serial_interrupt_init();
void validate_status();
void update_radio_lock();
void transmit(uint16_t size);
void update_airtime();
void update_modem_status();
void buffer_serial();
void serial_poll();
void work_while_waiting();

#ifdef HAS_RNS
# 218 "/config/Desktop/microReticulum_Firmware/RNode_Firmware.ino"
RNS::Reticulum reticulum(RNS::Type::NONE);
RNS::Interface lora_interface(RNS::Type::NONE);
#if defined(UDP_TRANSPORT)
RNS::Interface udp_interface(RNS::Type::NONE);
#endif
#if defined(RNS_USE_FS)

  #if MCU_VARIANT == MCU_ESP32
    #if defined(USTORE_USE_SD)
      #include <microStore/Adapters/SDFileSystem.h>
      microStore::FileSystem filesystem{microStore::Adapters::SDFileSystem(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS)};
    #else




      #include <microStore/Adapters/PosixFileSystem.h>
      microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
    #endif
  #elif MCU_VARIANT == MCU_NRF52
    #include <microStore/Adapters/InternalFSFileSystem.h>
    #include <microStore/Adapters/FlashFSFileSystem.h>
    microStore::FileSystem filesystem{microStore::Adapters::InternalFSFileSystem()};
  #else
    #include <microStore/Adapters/PosixFileSystem.h>
    microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
  #endif
  #else
    #include <microStore/Adapters/NoopFileSystem.h>
    microStore::FileSystem filesystem{microStore::Adapters::NoopFileSystem()};
  #endif
#endif

void on_log(const char* msg, RNS::LogLevel level) {
  if (kiss_framed_logs) {


    char line[256];
    int n = snprintf(line, sizeof(line), "%s [%s] %s",
                     RNS::getTimeString(),
                     RNS::getLevelName(level),
                     msg);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    kiss_indicate_log(line, (size_t)n);
  }
  else {

    Serial.print(RNS::getTimeString());
    Serial.print(" [");
    Serial.print(RNS::getLevelName(level));
    Serial.print("] ");
    Serial.println(msg);
    Serial.flush();
  }

#ifdef HAS_SDCARD
 File file = SD.open("/logfile.txt", FILE_APPEND);
 if (file) {
    file.write((uint8_t*)msg, strlen(msg));
    file.close();
  }
#endif
}


void on_receive_packet(const RNS::Bytes& raw, const RNS::Interface& interface) {
#ifdef HAS_SDCARD
  TRACE("Logging receive packet to SD");
  String line = RNS::getTimeString() + String(" recv: ") + String(raw.toHex().c_str()) + "\n";
 File file = SD.open("./tracefile.txt", FILE_APPEND);
 if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
 RNS::Packet packet(raw);
 if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" recv: ") + String(packet.dumpString().c_str()) + "\n";
    File file = SD.open("./tracedetails.txt", FILE_APPEND);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
 }
#endif
#if PLATFORM == PLATFORM_NATIVE
  String line = RNS::getTimeString() + String(" RECV: ") + String(raw.toHex().c_str()) + "\n";
 microStore::File file = filesystem.open("./tracefile.txt", microStore::File::ModeAppend);
 if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
 RNS::Packet packet(raw);
 if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" RECV: ") + String(packet.dumpString().c_str()) + "\n";
   microStore::File file = filesystem.open("./tracedetails.txt", microStore::File::ModeAppend);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
 }
#endif
}


void on_transmit_packet(const RNS::Bytes& raw, const RNS::Interface& interface) {
#ifdef HAS_SDCARD
  TRACE("Logging transmit packet to SD");
  String line = RNS::getTimeString() + String(" send: ") + String(raw.toHex().c_str()) + "\n";
 File file = SD.open("/tracefile.txt", FILE_APPEND);
 if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
 RNS::Packet packet(raw);
 if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" send: ") + String(packet.dumpString().c_str()) + "\n";
    File file = SD.open("/tracedetails.txt", FILE_APPEND);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
 }
#endif
#if PLATFORM == PLATFORM_NATIVE
  String line = RNS::getTimeString() + String(" SEND: ") + String(raw.toHex().c_str()) + "\n";
 microStore::File file = filesystem.open("./tracefile.txt", microStore::File::ModeAppend);
 if (file) {
    file.write((uint8_t*)line.c_str(), line.length());
    file.close();
  }
 RNS::Packet packet(raw);
 if (packet.unpack()) {
    String line = RNS::getTimeString() + String(" SEND: ") + String(packet.dumpString().c_str()) + "\n";
   microStore::File file = filesystem.open("./tracedetails.txt", microStore::File::ModeAppend);
    if (file) {
      file.write((uint8_t*)line.c_str(), line.length());
      file.close();
    }
 }
#endif
}


#if MCU_VARIANT == MCU_ESP32

#include <esp_vfs.h>
#include <sys/errno.h>
static ssize_t kiss_vfs_write(int fd, const void* data, size_t size);
static int kiss_vfs_open(const char* path, int flags, int mode);
static int kiss_vfs_close(int fd);
static int kiss_vfs_fstat(int fd, struct stat* st);
static void install_kiss_stdout(void);
static void install_kiss_stdout(void);
static void install_kiss_stdout(void);
void setup();
void lora_receive();
inline void kiss_write_packet();
inline void getPacketData(uint16_t len);
void ISR_VECT receive_callback(int packet_size);
bool startRadio();
void stopRadio();
bool queue_full();
void flush_queue(void);
void pop_queue();
void add_airtime(uint16_t written);
void serial_callback(uint8_t sbyte);
bool medium_free();
void update_noise_floor();
void check_modem_status();
void tx_queue_handler();
void loop();
void sleep_now();
void button_event(uint8_t event, unsigned long duration);
#line 367 "/config/Desktop/microReticulum_Firmware/RNode_Firmware.ino"
static ssize_t kiss_vfs_write(int fd, const void* data, size_t size) {
  if (kiss_framed_logs) {
    kiss_indicate_log((const char*)data, size);
  } else {
    Serial.write((const uint8_t*)data, size);
    Serial.flush();
  }
  return (ssize_t)size;
}

static int kiss_vfs_open(const char* path, int flags, int mode) { return 0; }
static int kiss_vfs_close(int fd) { return 0; }
static int kiss_vfs_fstat(int fd, struct stat* st) {
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFCHR;
  return 0;
}

static void install_kiss_stdout(void) {
  static const esp_vfs_t kiss_vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .write = &kiss_vfs_write,
    .open = &kiss_vfs_open,
    .close = &kiss_vfs_close,
    .fstat = &kiss_vfs_fstat,
  };
  if (esp_vfs_register("/dev/kiss", &kiss_vfs, NULL) != ESP_OK) return;
  FILE* f = fopen("/dev/kiss/0", "w");
  if (!f) return;
  setvbuf(f, NULL, _IONBF, 0);
  stdout = f;

  stderr = f;
  esp_log_set_vprintf(&vprintf);
}

#elif MCU_VARIANT == MCU_NRF52







extern "C" int __wrap__write(int file, const void *ptr, size_t len) {
  (void)file;
  if (kiss_framed_logs) {
    kiss_indicate_log((const char*)ptr, len);
    return (int)len;
  }
  int n = Serial.write((const uint8_t*)ptr, len);
  Serial.flush();
  return n;
}

static void install_kiss_stdout(void) {}

#else

extern "C" int _write(int file, char *ptr, int len) {
  size_t wrote = 0;
  if (kiss_framed_logs) {
    kiss_indicate_log(ptr, len);
    wrote = len;
  }
  else {
    wrote = Serial.write(ptr, len);
    Serial.flush();
  }
  return wrote;
}

static void install_kiss_stdout(void) {}

#endif

#if defined(RNS_USE_FS)
void dump_filesystem(const char* basepath, uint8_t level = 0, uint8_t max_level = 0) {
  if (max_level > 0 && level > max_level) return;
  char prefix[17] = "";
  for (uint8_t index = 0; index < level && index < 8; index++) {
    prefix[index*2] = ' ';
    prefix[index*2+1] = ' ';
    prefix[index*2+2] = '\0';
  }
  filesystem.listDirectory(basepath, [&](const char* name) -> void {



    if (name[0] == '.') return;
    char fullpath[96];
    const bool root = (basepath[0] == '/' && basepath[1] == '\0');
    if (root) snprintf(fullpath, sizeof(fullpath), "/%s", name);
    else snprintf(fullpath, sizeof(fullpath), "%s/%s", basepath, name);
    if (filesystem.isDirectory(fullpath)) {
      TRACEF("%s%s:", prefix, name);
      dump_filesystem(fullpath, level + 1, max_level);
    }
    else {
      TRACEF("%s%s", prefix, name);
    }
  });
}
#endif

void setup() {


  memset(serialBuffer, 0, sizeof(serialBuffer));
  fifo_init(&serialFIFO, serialBuffer, CONFIG_UART_BUFFER_SIZE);

  Serial.begin(serial_baudrate);


  install_kiss_stdout();


  while (!Serial) {
    if (millis() > 2000) {
      break;
    }
    delay(10);
  }


  #if BOARD_MODEL != BOARD_HELTEC_TRACKER_V2
    delay(2000);
  #endif

#ifdef HAS_RNS
  printf("Total SRAM:  %7u bytes\n", RNS::Utilities::Memory::heap_size());
  printf("Free SRAM:   %7u bytes\n", RNS::Utilities::Memory::heap_available());
#endif
#if defined(ESP32)
 printf("Total PSRAM: %7u bytes\n", ESP.getPsramSize());
#endif


  device_uid_init();
  INFOF("Device UID:  %s", device_uid_str);

#ifdef HAS_GPIO
  GPIO::init();
  INFO("GPIO control initialized");
#endif

#ifdef HAS_BME
  BME680::init();
#endif


  #if MCU_VARIANT == MCU_ESP32
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      esp_task_wdt_config_t wdt_config = {
          .timeout_ms = WDT_TIMEOUT * 1000,
          .idle_core_mask = 0,
          .trigger_panic = true,
      };



      if (esp_task_wdt_reconfigure(&wdt_config) == ESP_ERR_INVALID_STATE) {
          esp_task_wdt_init(&wdt_config);
      }
    #else
      esp_task_wdt_init(WDT_TIMEOUT, true);
    #endif
    esp_task_wdt_add(NULL);
  #elif MCU_VARIANT == MCU_NRF52
    NRF_WDT->CONFIG = 0x01;
    NRF_WDT->CRV = WDT_TIMEOUT * 32768 + 1;
    NRF_WDT->RREN = 0x01;
    NRF_WDT->TASKS_START = 1;
  #endif

  #if MCU_VARIANT == MCU_ESP32
    boot_seq();
    EEPROM.begin(EEPROM_SIZE);
    Serial.setRxBufferSize(CONFIG_UART_BUFFER_SIZE);

    #if BOARD_MODEL == BOARD_TDECK
      pinMode(pin_poweron, OUTPUT);
      digitalWrite(pin_poweron, HIGH);

      pinMode(SD_CS, OUTPUT);
      pinMode(DISPLAY_CS, OUTPUT);
      digitalWrite(SD_CS, HIGH);
      digitalWrite(DISPLAY_CS, HIGH);

      pinMode(DISPLAY_BL_PIN, OUTPUT);

      #if BOARD_MODEL == BOARD_TDECK
        td_ui::begin();
      #endif
    #endif
  #endif

  #if MCU_VARIANT == MCU_NRF52
    #if BOARD_MODEL == BOARD_TECHO
      delay(200);
      pinMode(PIN_VEXT_EN, OUTPUT);
      digitalWrite(PIN_VEXT_EN, HIGH);
      pinMode(pin_btn_usr1, INPUT_PULLUP);
      pinMode(pin_btn_touch, INPUT_PULLUP);
      pinMode(PIN_LED_RED, OUTPUT);
      pinMode(PIN_LED_GREEN, OUTPUT);
      pinMode(PIN_LED_BLUE, OUTPUT);
      delay(200);
    #endif

    if (!eeprom_begin()) { Serial.write("EEPROM initialisation failed.\r\n"); }
  #endif


  #if MCU_VARIANT == MCU_ESP32


    unsigned long seed_val = (unsigned long)esp_random();
  #elif MCU_VARIANT == MCU_NRF52


    unsigned long seed_val = get_rng_seed();
  #else
# 598 "/config/Desktop/microReticulum_Firmware/RNode_Firmware.ino"
    unsigned long seed_val = analogRead(0);
  #endif
  randomSeed(seed_val);

  #if HAS_NP
    led_init();
  #endif

  #if MCU_VARIANT == MCU_NRF52 && HAS_NP == true
    boot_seq();
  #endif

  #if BOARD_MODEL != BOARD_RAK4631 && BOARD_MODEL != BOARD_RAK3401 && BOARD_MODEL != BOARD_HELTEC_T114 && BOARD_MODEL != BOARD_TECHO && BOARD_MODEL != BOARD_T3S3 && BOARD_MODEL != BOARD_TBEAM_S_V1 && BOARD_MODEL != BOARD_HELTEC32_V4 && BOARD_MODEL != BOARD_HELTEC_TRACKER_V2




    while (!Serial);
  #endif

  serial_interrupt_init();


  #if HAS_INPUT
    input_init();
  #endif

  #if HAS_NP == false


    if (pin_led_rx >= 0) pinMode(pin_led_rx, OUTPUT);
    if (pin_led_tx >= 0) pinMode(pin_led_tx, OUTPUT);
  #endif

  #if HAS_TCXO == true
    if (pin_tcxo_enable != -1) {
        pinMode(pin_tcxo_enable, OUTPUT);
        digitalWrite(pin_tcxo_enable, HIGH);
    }
  #endif


  memset(pbuf, 0, sizeof(pbuf));
  memset(cmdbuf, 0, sizeof(cmdbuf));

  memset(packet_queue, 0, sizeof(packet_queue));

  memset(packet_starts_buf, 0, sizeof(packet_starts_buf));
  fifo16_init(&packet_starts, packet_starts_buf, CONFIG_QUEUE_MAX_LENGTH);

  memset(packet_lengths_buf, 0, sizeof(packet_starts_buf));
  fifo16_init(&packet_lengths, packet_lengths_buf, CONFIG_QUEUE_MAX_LENGTH);

  #if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52 || PLATFORM == PLATFORM_NATIVE
    modem_packet_queue = xQueueCreate(MODEM_QUEUE_SIZE, sizeof(modem_packet_t*));
  #endif





  #if defined(LORA_TRANSPORT)


  #if MODEM == MODEM_RUNTIME



  LoRa = native_lora::create_radio(current_modem);



  if (LoRa != nullptr && current_modem == SX1262) {
    float v = native_config::g_config.dio3_tcxo_voltage;
    if (v > 0.0f) {


      uint8_t mode;
      if (v >= 3.15f) mode = 0x07;
      else if (v >= 2.30f) mode = 0x06;
      else if (v >= 2.00f) mode = 0x03;
      else if (v >= 1.75f) mode = 0x02;
      else if (v >= 1.65f) mode = 0x01;
      else mode = 0x00;
      LoRa->setTcxoVoltage(mode);
    }
    LoRa->setDio2AsRfSwitch(native_config::g_config.dio2_as_rf_switch);
  }
  #elif MODEM == SX1276 || MODEM == SX1278
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy);
  #elif MODEM == SX1262
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy, pin_rxen);
  #elif MODEM == SX1280
  LoRa->setPins(pin_cs, pin_reset, pin_dio, pin_busy, pin_rxen, pin_txen);
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    init_channel_stats();

    #if BOARD_MODEL == BOARD_T3S3
      #if MODEM == SX1280
        delay(300);
        LoRa->reset();
        delay(100);
      #endif
    #endif

    #if BOARD_MODEL == BOARD_XIAO_S3

      delay(300);
      LoRa->reset();
      delay(100);
    #endif



    #if MCU_VARIANT == MCU_NATIVE






      native_pinmap::assert_radio_enable_pins();






      delay(10);
      LoRa->reset();
      delay(10);
    #endif
    if (LoRa->preInit()) {
      modem_installed = true;

      #if HAS_INPUT

      #else
        uint32_t lfr = LoRa->getFrequency();
        if (lfr == 0) {

        } else if (lfr == M_FRQ_R) {

          #if HAS_CONSOLE
            if (rtc_get_reset_reason(0) == POWERON_RESET) {
              console_active = true;
            }
          #endif
        } else {

        }
        LoRa->setFrequency(M_FRQ_S);
      #endif

    } else {
      modem_installed = false;
      #if MCU_VARIANT == MCU_NATIVE
        native_pinmap::deassert_radio_enable_pins();
      #endif
    }
  #else


    modem_installed = true;
  #endif
  #endif

  #if HAS_DISPLAY
    #if HAS_EEPROM
    if (EEPROM.read(eeprom_addr(ADDR_CONF_DSET)) != CONF_OK_BYTE) {
    #elif MCU_VARIANT == MCU_NRF52
    if (eeprom_read(eeprom_addr(ADDR_CONF_DSET)) != CONF_OK_BYTE) {
    #endif
      eeprom_update(eeprom_addr(ADDR_CONF_DSET), CONF_OK_BYTE);
      #if BOARD_MODEL == BOARD_TECHO
        eeprom_update(eeprom_addr(ADDR_CONF_DINT), 0x03);
      #else
        eeprom_update(eeprom_addr(ADDR_CONF_DINT), 0xFF);
      #endif
    }
    #if BOARD_MODEL == BOARD_TECHO
      display_add_callback(work_while_waiting);
    #endif

    display_unblank();
    disp_ready = display_init();
    update_display();
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    #if HAS_PMU == true
      pmu_ready = init_pmu();
    #endif





    #if BOARD_MODEL == BOARD_HELTEC_TRACKER_V2 && HAS_EEPROM
      if (EEPROM.read(eeprom_addr(ADDR_CONF_BT)) == 0xFF) {
        eeprom_update(eeprom_addr(ADDR_CONF_BT), BT_ENABLE_BYTE);
      }
      if (EEPROM.read(eeprom_addr(ADDR_CONF_WIFI)) == 0xFF) {
        eeprom_update(eeprom_addr(ADDR_CONF_WIFI), WR_WIFI_OFF);
      }
    #endif

    #if HAS_BLUETOOTH || HAS_BLE == true
      bt_init();
      bt_init_ran = true;
    #endif

    if (console_active) {
      #if HAS_CONSOLE
        console_start();
      #else
        kiss_indicate_reset();
      #endif
    } else {
      #if HAS_WIFI
        wifi_mode = EEPROM.read(eeprom_addr(ADDR_CONF_WIFI));
        if (wifi_mode == WR_WIFI_STA || wifi_mode == WR_WIFI_AP) { wifi_remote_init(); }
      #endif
      kiss_indicate_reset();
    }
  #endif

  #if defined(ENABLE_WEBSOCKETS) && __has_include(<WiFi.h>)




    ws_console::init(81);
  #endif

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    #if MODEM == MODEM_RUNTIME

      if (current_modem == SX1280) {
        avoid_interference = false;
      } else {
        #if HAS_EEPROM
          uint8_t ia_conf = EEPROM.read(eeprom_addr(ADDR_CONF_DIA));
          if (ia_conf == 0x00) { avoid_interference = true; }
          else { avoid_interference = false; }
        #else
          avoid_interference = false;
        #endif
      }
    #elif MODEM == SX1280
      avoid_interference = false;
    #else
      #if HAS_EEPROM
        uint8_t ia_conf = EEPROM.read(eeprom_addr(ADDR_CONF_DIA));
        if (ia_conf == 0x00) { avoid_interference = true; }
        else { avoid_interference = false; }
      #elif MCU_VARIANT == MCU_NRF52
        uint8_t ia_conf = eeprom_read(eeprom_addr(ADDR_CONF_DIA));
        if (ia_conf == 0x00) { avoid_interference = true; }
        else { avoid_interference = false; }
      #endif
    #endif
  #endif


#ifdef DISABLE_IA
  avoid_interference = false;
#endif


  validate_status();

  #if defined(LORA_TRANSPORT)
  if (op_mode != MODE_TNC) LoRa->setFrequency(0);
  #endif


#ifdef HAS_SDCARD
  pinMode(SDCARD_MISO, INPUT_PULLUP);
  SDSPI.begin(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS);
  if (!SD.begin(SDCARD_CS, SDSPI)) {
      printf("setupSDCard FAIL\n");
  } else {
      uint32_t cardSize = SD.cardSize() / (1024 * 1024);
      printf("setupSDCard PASS . SIZE = %u GB\n", cardSize / 1024.0);
      SD.remove("/logfile");
      SD.remove("/logfile.txt");
      SD.remove("/tracefile");
      SD.remove("/tracedetails");
      SD.remove("/tracefile.txt");
      SD.remove("/tracedetails.txt");
      printf("DIR: /\n");
      File root = SD.open("/");
      File file = root.openNextFile();
      while(file){
          printf("  FILE: %s\n", file.name());
          file = root.openNextFile();
      }
  }
  delay(3000);
#endif

#ifdef HAS_RNS



  RNS::Transport::path_table_maxsize(URTN_PATH_TABLE_MAX_RECS);
  RNS::Transport::announce_table_maxsize(50);
  RNS::Transport::hashlist_maxsize(50);
  RNS::Identity::known_destinations_maxsize(50);
  RNS::Transport::max_pr_tags(50);
  RNS::Reticulum::clean_interval(60*15);

  RNS::Reticulum::persist_interval(60*60);




  RNS::set_log_callback(&on_log);
  RNS::Transport::set_receive_packet_callback(on_receive_packet);
  RNS::Transport::set_transmit_packet_callback(on_transmit_packet);

  try {

    HEAD("Initializing filesystem...", RNS::LOG_TRACE);
#if BOARD_MODEL == BOARD_RAK4631 || BOARD_MODEL == BOARD_RAK3401
    bool init_success = false;

    {
      TRACE("Looking for RAK15001 flash...");
      static const SPIFlash_Device_t device = RAK15001;


      filesystem = microStore::Adapters::FlashFSFileSystem(&device, SS);
      if (filesystem.init()) {
        TRACE("Initialized RAK15001 flash");
        init_success = true;

        RNS::Transport::path_table_maxsize(500);
        RNS::Transport::path_store_segment_size(24576);
        RNS::Transport::path_store_segment_count(8);
      }
    }

    if (!init_success) {
      TRACE("Looking for W25Q128 flash...");
      static const SPIFlash_Device_t device = W25Q128;


      filesystem = microStore::Adapters::FlashFSFileSystem(&device, WB_IO1);
      if (filesystem.init()) {
        TRACE("Initialized W25Q128 flash");
        init_success = true;

        RNS::Transport::path_table_maxsize(500);
        RNS::Transport::path_store_segment_size(24576);
        RNS::Transport::path_store_segment_count(8);
      }
    }

    if (!init_success) {
      TRACE("Using internal flash...");
      filesystem = microStore::Adapters::InternalFSFileSystem();
      if (!filesystem.init()) WARNING("Failed to initialize filesystem!");
      else TRACE("Initialized internal flash");
    }
#else
    if (!filesystem.init()) WARNING("Failed to initialize filesystem!");
#endif


    filesystem.remove("./destination_table");
    filesystem.remove("./path_store_index.dat");
    filesystem.remove("./path_store_0.dat");
    filesystem.remove("./path_store_1.dat");
    filesystem.remove("./path_store_2.dat");
    filesystem.remove("./path_store_3.dat");
    filesystem.remove("./path_store_4.dat");
    filesystem.remove("./path_store_5.dat");
    filesystem.remove("./path_store_6.dat");
    filesystem.remove("./path_store_7.dat");
    if (filesystem.isDirectory("./cache")) {
      filesystem.listDirectory("./cache", [&](const char* name) -> void {
        char rmpath[64];
        snprintf(rmpath, sizeof(rmpath), "./cache/%s", name);
        if (filesystem.isDirectory(rmpath)) filesystem.rmdir(rmpath);
        else filesystem.remove(rmpath);
      });
      filesystem.rmdir("./cache");
    }

#if PLATFORM != PLATFORM_NATIVE

    if (filesystem.storageAvailable() < 1024) {
      WARNING("FileSystem is full, clearing space...");

      filesystem.remove("/path_store/index.dat");

      filesystem.remove("/path_store/seg0.dat");
      filesystem.remove("/path_store/seg1.dat");
      filesystem.remove("/path_store/seg2.dat");
      filesystem.remove("/path_store/seg3.dat");
      filesystem.remove("/path_store/seg4.dat");
      filesystem.remove("/path_store/seg5.dat");
      filesystem.remove("/path_store/seg6.dat");
      filesystem.remove("/path_store/seg7.dat");
    }
#endif

    TRACE("Registering filesystem...");
    RNS::Utilities::OS::register_filesystem(filesystem);

#if defined(RNS_USE_FS)
#if 0
    filesystem.format();
#endif
#if 1
    TRACE("Listing filesystem...");
    dump_filesystem("./", 1, 2);
#endif
#endif



    if (true) {

#if defined(LORA_TRANSPORT)
      lora_interface = new LoRaInterface();

      lora_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
#endif
#if HAS_WIFI && defined(UDP_TRANSPORT)
      if (wifi_mode != WR_WIFI_OFF) {
        udp_interface = new UDPInterface();

        udp_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
      }
#endif


      reticulum.transport_enabled(true);

      reticulum.probe_destination_enabled(true);

      reticulum.remote_management_enabled(true);

#ifdef URTN_STATS_PAGES

      snprintf(nomadnet_name, sizeof(nomadnet_name), "microReticulum Node [%s]", device_uid_str);
#endif

#ifdef HAS_PROVISIONING







      HEAD("Initializing Provisioning subsystem...", RNS::LOG_TRACE);
      init_provisioning();
      auto& prov = RNS::Provisioning::Provisioner::instance();
#endif



      HEAD("Starting RNS...\r\n", RNS::LOG_VERBOSE);
#if defined(RNS_MEM_LOG)
      RNS::loglevel(RNS::LOG_MEM);
#else
      RNS::loglevel(RNS::LOG_TRACE);
#endif

#if defined(LORA_TRANSPORT)
      HEAD("Registering LoRA Interface...", RNS::LOG_TRACE);
      RNS::Transport::register_interface(lora_interface);
      TRACEF("LoRaInterface hash: %s", lora_interface.get_hash().toHex().c_str());
#endif
#if HAS_WIFI && defined(UDP_TRANSPORT)
      if (wifi_mode != WR_WIFI_OFF) {
        HEAD("Registering UDP Interface...", RNS::LOG_TRACE);
        RNS::Transport::register_interface(udp_interface);
        TRACEF("UDPInterface hash: %s", udp_interface.get_hash().toHex().c_str());
      }
#endif

      HEAD("Creating Reticulum instance...", RNS::LOG_TRACE);
      reticulum = RNS::Reticulum();

printf("[init] hw_ready: %u\n", hw_ready);
printf("[init] op_mode: %U\n", op_mode);
      if (op_mode != MODE_TNC) {
        INFO("Not in TNC mode, transport will be disabled");
        reticulum.transport_enabled(false);
      }
      reticulum.start();



      RNS::Utilities::OS::set_loop_callback(&loop);


#if 0
      RNS::Identity identity = {RNS::Type::NONE};
      std::string local_identity_path = RNS::Reticulum::_storagepath + "/local_identity";
      if (RNS::Utilities::OS::file_exists(local_identity_path.c_str())) {
        identity = RNS::Identity::from_file(local_identity_path.c_str());
      }
      if (!identity) {
        RNS::verbose("No valid local identity in storage, creating...");
        identity = RNS::Identity();
        identity.to_file(local_identity_path.c_str());
      }
      else {
        RNS::verbose("Loaded local identity from storage");
      }
      RNS::Destination destination(identity, RNS::Type::Destination::IN, RNS::Type::Destination::SINGLE, "rnstransport", "local");
#endif


#ifdef URTN_STATS_PAGES
      if (nomadnet_enabled) {



        nomadnet_destination = RNS::Destination(
          RNS::Transport::identity(),
          RNS::Type::Destination::IN,
          RNS::Type::Destination::SINGLE,
          "nomadnetwork",
          "node"
        );





        nomadnet_destination.register_request_handler("/page/index.mu", serve_page, RNS::Type::Destination::ALLOW_ALL);
        nomadnet_destination.register_request_handler("/page/stack.mu", serve_page, RNS::Type::Destination::ALLOW_LIST, RNS::Transport::remote_management_allowed());
        nomadnet_destination.register_request_handler("/page/device.mu", serve_page, RNS::Type::Destination::ALLOW_LIST, RNS::Transport::remote_management_allowed());
#ifdef HAS_BME
        if (BME680::bme_installed) {
          nomadnet_destination.register_request_handler("/page/telemetry.mu", serve_page, RNS::Type::Destination::ALLOW_ALL);
        }
#endif





        {
          NOTICEF("Announcing NomadNet site \"%s\" at destination <%s>", nomadnet_name, nomadnet_destination.hash().toHex().c_str());
          nomadnet_destination.announce(nomadnet_name);
        }
      }
#endif

      HEAD("RNS is READY!", RNS::LOG_TRACE);
      if (op_mode == MODE_TNC) {
        HEAD("RNS transport mode is ENABLED", RNS::LOG_TRACE);
        TRACEF("Frequency: %d Hz", lora_freq);
        TRACEF("Bandwidth: %d Hz", lora_bw);
        TRACEF("Spreading Factor: %d", lora_sf);
        TRACEF("Coding Rate: %d", lora_cr);
        TRACEF("TX Power: %d dBm", lora_txp);
        HEAD("RNS Transport is READY!", RNS::LOG_TRACE);
      }
      else {
        HEAD("RNS transport mode is DISABLED", RNS::LOG_INFO);
        HEAD("Configure TNC mode with radio configuration to enable RNS transport", RNS::LOG_INFO);
      }
    }
    else {
      HEAD("RNS is inoperable because hardware is not ready!", RNS::LOG_ERROR);
      HEAD("Check firmware signature and eeprom provisioning", RNS::LOG_ERROR);


    }
  }
  catch (const std::bad_alloc&) {
    ERROR("RNS startup failed: bad_alloc - out of memory");
  }
  catch (std::exception& e) {
    ERRORF("RNS startup failed: %s", e.what());
  }
#endif
}

void lora_receive() {
  if (!implicit) {
    LoRa->receive();
  } else {
    LoRa->receive(implicit_l);
  }
}

inline void kiss_write_packet() {

#if defined(HAS_RNS) && defined(LORA_TRANSPORT)
  if (host_write_len > 0) {
    printf("[radio] Received %d byte packet", host_write_len);

    RNS::Bytes data(pbuf, host_write_len);
    lora_interface.r_stat_rssi(last_rssi);
    lora_interface.r_stat_snr(((int8_t)last_snr_raw) / 4.0f);
    lora_interface.r_stat_q(get_quality());
    lora_interface.handle_incoming(data);
  }
#endif

  serial_write(FEND);
  serial_write(CMD_DATA);

  for (uint16_t i = 0; i < host_write_len; i++) {
    #if MCU_VARIANT == MCU_NRF52
      portENTER_CRITICAL();
      uint8_t byte = pbuf[i];
      portEXIT_CRITICAL();
    #else
      uint8_t byte = pbuf[i];
    #endif

    if (byte == FEND) { serial_write(FESC); byte = TFEND; }
    if (byte == FESC) { serial_write(FESC); byte = TFESC; }
    serial_write(byte);
  }

  serial_write(FEND);
  host_write_len = 0;

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    packet_ready = false;
  #endif

  #if MCU_VARIANT == MCU_ESP32
    #if HAS_BLE
      bt_flush();
    #endif
  #endif
}

inline void getPacketData(uint16_t len) {
  #if MCU_VARIANT != MCU_NRF52
    while (len-- && read_len < MTU) {
      pbuf[read_len++] = LoRa->read();
    }
  #else
    BaseType_t int_mask = taskENTER_CRITICAL_FROM_ISR();
    while (len-- && read_len < MTU) {
      pbuf[read_len++] = LoRa->read();
    }
    taskEXIT_CRITICAL_FROM_ISR(int_mask);
  #endif
}

void ISR_VECT receive_callback(int packet_size) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52
    BaseType_t int_mask;
  #endif

  bool ready = false;
  if (!promisc) {





    uint8_t header = LoRa->read(); packet_size--;
    uint8_t sequence = packetSequence(header);

    if (isSplitPacket(header) && seq == SEQ_UNSET) {



      #if MCU_VARIANT == MCU_NRF52
        int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
      #else
        read_len = 0;
      #endif

      seq = sequence;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);

    } else if (isSplitPacket(header) && seq == sequence) {



      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = (last_rssi+LoRa->packetRssi())/2;
        last_snr_raw = (last_snr_raw+LoRa->packetSnrRaw())/2;
      #endif

      getPacketData(packet_size);
      seq = SEQ_UNSET;
      ready = true;

    } else if (isSplitPacket(header) && seq != sequence) {




      #if MCU_VARIANT == MCU_NRF52
        int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
      #else
        read_len = 0;
      #endif
      seq = sequence;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);

    } else if (!isSplitPacket(header)) {




      if (seq != SEQ_UNSET) {


        #if MCU_VARIANT == MCU_NRF52
          int_mask = taskENTER_CRITICAL_FROM_ISR(); read_len = 0; taskEXIT_CRITICAL_FROM_ISR(int_mask);
        #else
          read_len = 0;
        #endif
        seq = SEQ_UNSET;
      }

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
      #endif

      getPacketData(packet_size);
      ready = true;
    }
  } else {


    read_len = 0;

    #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
      last_rssi = LoRa->packetRssi();
      last_snr_raw = LoRa->packetSnrRaw();
      getPacketData(packet_size);



      kiss_indicate_stat_rssi();
      kiss_indicate_stat_snr();


      kiss_write_packet();

    #else
      getPacketData(packet_size);
      packet_ready = true;
    #endif
  }

  if (ready) {
    #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE


      kiss_indicate_stat_rssi();
      kiss_indicate_stat_snr();


      host_write_len = read_len;
      kiss_write_packet(); read_len = 0;

    #else


      modem_packet_t *modem_packet = (modem_packet_t*)malloc(sizeof(modem_packet_t) + read_len);
      if(!modem_packet) { memory_low = true; return; }


      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NATIVE
        modem_packet->snr_raw = LoRa->packetSnrRaw();
        modem_packet->rssi = LoRa->packetRssi(modem_packet->snr_raw);
      #endif




      modem_packet->len = read_len;
      memcpy(modem_packet->data, pbuf, read_len); read_len = 0;
      if (!modem_packet_queue || xQueueSendFromISR(modem_packet_queue, &modem_packet, NULL) != pdPASS) {
          free(modem_packet);
      }
    #endif
  }
}

bool startRadio() {
  update_radio_lock();
  if (!radio_online && !console_active) {
    if (!radio_locked && hw_ready) {
      #if MCU_VARIANT == MCU_NATIVE



        native_pinmap::assert_radio_enable_pins();
      #endif
      if (!LoRa->begin(lora_freq)) {



        #if MCU_VARIANT == MCU_NATIVE
          native_pinmap::deassert_radio_enable_pins();
        #endif
        radio_error = true;
        kiss_indicate_error(ERROR_INITRADIO);
        led_indicate_error(0);
        return false;
      } else {
        radio_online = true;

        init_channel_stats();

        setTXPower();
        setBandwidth();
        setSpreadingFactor();
        setCodingRate();
        getFrequency();

        LoRa->enableCrc();
        LoRa->onReceive(receive_callback);
        lora_receive();



        kiss_indicate_radiostate();
        led_indicate_info(3);
        return true;
      }

    } else {



      radio_online = false;
      kiss_indicate_radiostate();
      led_indicate_warning(3);
      return false;
    }
  } else {


    kiss_indicate_radiostate();
    return true;
  }
}

void stopRadio() {






  #if defined(LORA_TRANSPORT)
  if (radio_online) LoRa->end();
  #endif
  radio_online = false;
  #if MCU_VARIANT == MCU_NATIVE


    native_pinmap::deassert_radio_enable_pins();
  #endif
}

void update_radio_lock() {
  if (lora_freq != 0 && lora_bw != 0 && lora_txp != 0xFF && lora_sf != 0) {
    radio_locked = false;
  } else {
    radio_locked = true;
  }
}

bool queue_full() { return (queue_height >= CONFIG_QUEUE_MAX_LENGTH || queued_bytes >= CONFIG_QUEUE_SIZE); }

volatile bool queue_flushing = false;
void flush_queue(void) {
  if (!queue_flushing) {
    queue_flushing = true;
    led_tx_on();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    while (!fifo16_isempty(&packet_starts)) {
    #else
    while (!fifo16_isempty_locked(&packet_starts)) {
    #endif

      uint16_t start = fifo16_pop(&packet_starts);
      uint16_t length = fifo16_pop(&packet_lengths);

      if (length >= MIN_L && length <= MTU) {
        for (uint16_t i = 0; i < length; i++) {
          uint16_t pos = (start+i)%CONFIG_QUEUE_SIZE;
          tbuf[i] = packet_queue[pos];
        }

        transmit(length);
      }
    }

    lora_receive(); led_tx_off();
  }

  queue_height = 0;
  queued_bytes = 0;

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    update_airtime();
  #endif

  queue_flushing = false;

  #if HAS_DISPLAY
    display_tx = true;
  #endif
}

void pop_queue() {
  if (!queue_flushing) {
    queue_flushing = true; led_tx_on();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    if (!fifo16_isempty(&packet_starts)) {
    #else
    if (!fifo16_isempty_locked(&packet_starts)) {
    #endif

      uint16_t start = fifo16_pop(&packet_starts);
      uint16_t length = fifo16_pop(&packet_lengths);
      if (length >= MIN_L && length <= MTU) {
        for (uint16_t i = 0; i < length; i++) {
          uint16_t pos = (start+i)%CONFIG_QUEUE_SIZE;
          tbuf[i] = packet_queue[pos];
        }

        transmit(length);
      }
      queue_height -= 1;
      queued_bytes -= length;
    }

    lora_receive(); led_tx_off();
  }

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    update_airtime();
  #endif

  queue_flushing = false;

  #if HAS_DISPLAY
    display_tx = true;
  #endif
}

void add_airtime(uint16_t written) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    float lora_symbols = 0;
    float packet_cost_ms = 0.0;
    int ldr_opt = 0; if (lora_low_datarate) ldr_opt = 1;

    #if MODEM == MODEM_RUNTIME
      if (current_modem == SX1276 || current_modem == SX1278) {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /= 4*(lora_sf-2*ldr_opt);
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 0.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;
      } else {
        if (lora_sf < 7) {
          lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + PHY_HEADER_LORA_SYMBOLS);
          lora_symbols /= 4*lora_sf;
          lora_symbols *= lora_cr;
          lora_symbols += lora_preamble_symbols + 2.25 + 8;
          packet_cost_ms += lora_symbols * lora_symbol_time_ms;
        } else {
          lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
          lora_symbols /= 4*(lora_sf-2*ldr_opt);
          lora_symbols *= lora_cr;
          lora_symbols += lora_preamble_symbols + 0.25 + 8;
          packet_cost_ms += lora_symbols * lora_symbol_time_ms;
        }
      }
    #elif MODEM == SX1276 || MODEM == SX1278
      lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
      lora_symbols /= 4*(lora_sf-2*ldr_opt);
      lora_symbols *= lora_cr;
      lora_symbols += lora_preamble_symbols + 0.25 + 8;
      packet_cost_ms += lora_symbols * lora_symbol_time_ms;

    #elif MODEM == SX1262 || MODEM == SX1280
      if (lora_sf < 7) {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /= 4*lora_sf;
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 2.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;

      } else {
        lora_symbols += (8*written + PHY_CRC_LORA_BITS - 4*lora_sf + 8 + PHY_HEADER_LORA_SYMBOLS);
        lora_symbols /= 4*(lora_sf-2*ldr_opt);
        lora_symbols *= lora_cr;
        lora_symbols += lora_preamble_symbols + 0.25 + 8;
        packet_cost_ms += lora_symbols * lora_symbol_time_ms;
      }

    #endif

    uint16_t cb = current_airtime_bin();
    uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
    airtime_bins[cb] += packet_cost_ms;
    airtime_bins[nb] = 0;

  #endif
}

void update_airtime() {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    uint16_t cb = current_airtime_bin();
    uint16_t pb = cb-1; if (cb-1 < 0) { pb = AIRTIME_BINS-1; }
    uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
    airtime_bins[nb] = 0; airtime = (float)(airtime_bins[cb]+airtime_bins[pb])/(2.0*AIRTIME_BINLEN_MS);

    uint32_t longterm_airtime_sum = 0;
    for (uint16_t bin = 0; bin < AIRTIME_BINS; bin++) { longterm_airtime_sum += airtime_bins[bin]; }
    longterm_airtime = (float)longterm_airtime_sum/(float)AIRTIME_LONGTERM_MS;

    float longterm_channel_util_sum = 0.0;
    for (uint16_t bin = 0; bin < AIRTIME_BINS; bin++) { longterm_channel_util_sum += longterm_bins[bin]; }
    longterm_channel_util = (float)longterm_channel_util_sum/(float)AIRTIME_BINS;

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
      update_csma_parameters();
    #endif

    kiss_indicate_channel_stats();
  #endif
}

void transmit(uint16_t size) {
  if (radio_online) {
    if (!promisc) {
      uint16_t written = 0;
      uint8_t header = random(256) & 0xF0;
      if (size > SINGLE_MTU - HEADER_L) { header = header | FLAG_SPLIT; }

      LoRa->beginPacket();
      LoRa->write(header); written++;

      for (uint16_t i=0; i < size; i++) {
        LoRa->write(tbuf[i]); written++;

        if (written == 255 && isSplitPacket(header)) {
          if (!LoRa->endPacket()) {
            kiss_indicate_error(ERROR_MODEM_TIMEOUT);
            kiss_indicate_error(ERROR_TXFAILED);
            led_indicate_error(5);
            #if MCU_VARIANT == MCU_NATIVE
              if (native_config::g_config.reboot_on_tx_failure) { hard_reset(); }
              else { LoRa->receive(); return; }
            #else
              hard_reset();
            #endif
          }

          add_airtime(written);
          LoRa->beginPacket();
          LoRa->write(header);
          written = 1;
          printf("[radio] Sent %d byte packet (split)", written);
        }
      }

      if (!LoRa->endPacket()) {
        kiss_indicate_error(ERROR_MODEM_TIMEOUT);
        kiss_indicate_error(ERROR_TXFAILED);
        led_indicate_error(5);
        #if MCU_VARIANT == MCU_NATIVE
          if (native_config::g_config.reboot_on_tx_failure) { hard_reset(); }
          else { LoRa->receive(); return; }
        #else
          hard_reset();
        #endif
      }

      add_airtime(written);
      printf("[radio] Sent %d byte packet", written);

    } else {
      led_tx_on(); uint16_t written = 0;
      if (size > SINGLE_MTU) { size = SINGLE_MTU; }
      if (!implicit) { LoRa->beginPacket(); }
      else { LoRa->beginPacket(size); }
      for (uint16_t i=0; i < size; i++) { LoRa->write(tbuf[i]); written++; }
      LoRa->endPacket(); add_airtime(written);
      printf("[radio] Sent %d byte packet", written);
    }

  } else { kiss_indicate_error(ERROR_TXFAILED); led_indicate_error(5); }
}

void serial_callback(uint8_t sbyte) {
  if (IN_FRAME && sbyte == FEND && command == CMD_DATA) {
    IN_FRAME = false;

    if (!fifo16_isfull(&packet_starts) && queued_bytes < CONFIG_QUEUE_SIZE) {
        uint16_t s = current_packet_start;
        int16_t e = queue_cursor-1; if (e == -1) e = CONFIG_QUEUE_SIZE-1;
        uint16_t l;

        if (s != e) { l = (s < e) ? e - s + 1 : CONFIG_QUEUE_SIZE - s + e + 1; }
        else { l = 1; }

        if (l >= MIN_L) {
            queue_height++;
            fifo16_push(&packet_starts, s);
            fifo16_push(&packet_lengths, l);
            current_packet_start = queue_cursor;
        }
    }

#ifdef HAS_PROVISIONING
  } else if (IN_FRAME && sbyte == FEND && command == CMD_PROVISION_REQ) {
    IN_FRAME = false;
    on_provision_request(provision_rx_buf);
    provision_rx_buf.clear();
#endif
  } else if (sbyte == FEND) {
    IN_FRAME = true;
    command = CMD_UNKNOWN;
    frame_len = 0;
  } else if (IN_FRAME && frame_len < MTU) {

    if (frame_len == 0 && command == CMD_UNKNOWN) {
        command = sbyte;
    } else if (command == CMD_DATA) {
        if (bt_state != BT_STATE_CONNECTED) {
          cable_state = CABLE_STATE_CONNECTED;
        }
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (queue_height < CONFIG_QUEUE_MAX_LENGTH && queued_bytes < CONFIG_QUEUE_SIZE) {
              queued_bytes++;
              packet_queue[queue_cursor++] = sbyte;
              if (queue_cursor == CONFIG_QUEUE_SIZE) queue_cursor = 0;
            }
        }
#ifdef HAS_PROVISIONING
    } else if (command == CMD_PROVISION_REQ) {
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (provision_rx_buf.size() < PROVISION_RX_BUF_MAX) {
                provision_rx_buf.append(sbyte);
            }
        }
#endif
    } else if (command == CMD_FREQUENCY) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) {
          uint32_t freq = (uint32_t)cmdbuf[0] << 24 | (uint32_t)cmdbuf[1] << 16 | (uint32_t)cmdbuf[2] << 8 | (uint32_t)cmdbuf[3];

          if (freq == 0) {
            kiss_indicate_frequency();
          } else {
            lora_freq = freq;
            if (op_mode == MODE_HOST) setFrequency();
            kiss_indicate_frequency();
          }
        }
    } else if (command == CMD_BANDWIDTH) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) {
          uint32_t bw = (uint32_t)cmdbuf[0] << 24 | (uint32_t)cmdbuf[1] << 16 | (uint32_t)cmdbuf[2] << 8 | (uint32_t)cmdbuf[3];

          if (bw == 0) {
            kiss_indicate_bandwidth();
          } else {
            lora_bw = bw;
            if (op_mode == MODE_HOST) setBandwidth();
            kiss_indicate_bandwidth();
          }
        }
    } else if (command == CMD_TXPOWER) {
      if (sbyte == 0xFF) {
        kiss_indicate_txpower();
      } else {
        int txp = sbyte;
        #if MODEM == MODEM_RUNTIME
          if (current_modem == SX1262) {
            if (txp > 22) txp = 22;
          } else if (current_modem == SX1280) {
            if (txp > 13) txp = 13;
          } else {
            if (txp > 17) txp = 17;
          }
        #elif MODEM == SX1262
          #if HAS_LORA_PA
            if (txp > PA_MAX_OUTPUT) txp = PA_MAX_OUTPUT;
          #else
            if (txp > 22) txp = 22;
          #endif
        #elif MODEM == SX1280
          #if HAS_PA
            if (txp > 20) txp = 20;
          #else
            if (txp > 13) txp = 13;
          #endif
        #else
          if (txp > 17) txp = 17;
        #endif

        lora_txp = txp;
        if (op_mode == MODE_HOST) setTXPower();
        kiss_indicate_txpower();
      }
    } else if (command == CMD_SF) {
      if (sbyte == 0xFF) {
        kiss_indicate_spreadingfactor();
      } else {
        int sf = sbyte;
        if (sf < 5) sf = 5;
        if (sf > 12) sf = 12;

        lora_sf = sf;
        if (op_mode == MODE_HOST) setSpreadingFactor();
        kiss_indicate_spreadingfactor();
      }
    } else if (command == CMD_CR) {
      if (sbyte == 0xFF) {
        kiss_indicate_codingrate();
      } else {
        int cr = sbyte;
        if (cr < 5) cr = 5;
        if (cr > 8) cr = 8;

        lora_cr = cr;
        if (op_mode == MODE_HOST) setCodingRate();
        kiss_indicate_codingrate();
      }
    } else if (command == CMD_IMPLICIT) {
      set_implicit_length(sbyte);
      kiss_indicate_implicit_length();
    } else if (command == CMD_LEAVE) {
      if (sbyte == 0xFF) {
        display_unblank();
        cable_state = CABLE_STATE_DISCONNECTED;
        current_rssi = -292;
        last_rssi = -292;
        last_rssi_raw = 0x00;
        last_snr_raw = 0x80;
      }
    } else if (command == CMD_RADIO_STATE) {
      if (bt_state != BT_STATE_CONNECTED) {
        cable_state = CABLE_STATE_CONNECTED;
        display_unblank();
      }
      if (sbyte == 0xFF) {
        kiss_indicate_radiostate();
      } else if (sbyte == 0x00) {
        stopRadio();
        kiss_indicate_radiostate();
      } else if (sbyte == 0x01) {
        startRadio();
        kiss_indicate_radiostate();
      }
    } else if (command == CMD_ST_ALOCK) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          uint16_t at = (uint16_t)cmdbuf[0] << 8 | (uint16_t)cmdbuf[1];

          if (at == 0) {
            st_airtime_limit = 0.0;
          } else {
            st_airtime_limit = (float)at/(100.0*100.0);
            if (st_airtime_limit >= 1.0) { st_airtime_limit = 0.0; }
          }
          kiss_indicate_st_alock();
        }
    } else if (command == CMD_LT_ALOCK) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          uint16_t at = (uint16_t)cmdbuf[0] << 8 | (uint16_t)cmdbuf[1];

          if (at == 0) {
            lt_airtime_limit = 0.0;
          } else {
            lt_airtime_limit = (float)at/(100.0*100.0);
            if (lt_airtime_limit >= 1.0) { lt_airtime_limit = 0.0; }
          }
          kiss_indicate_lt_alock();
        }
    } else if (command == CMD_STAT_RX) {
      kiss_indicate_stat_rx();
    } else if (command == CMD_STAT_TX) {
      kiss_indicate_stat_tx();
    } else if (command == CMD_STAT_RSSI) {
      kiss_indicate_stat_rssi();
    } else if (command == CMD_RADIO_LOCK) {
      update_radio_lock();
      kiss_indicate_radio_lock();
    } else if (command == CMD_BLINK) {
      led_indicate_info(sbyte);
    } else if (command == CMD_RANDOM) {
      kiss_indicate_random(getRandom());
    } else if (command == CMD_DETECT) {
      if (sbyte == DETECT_REQ) {
        if (bt_state != BT_STATE_CONNECTED) cable_state = CABLE_STATE_CONNECTED;
        kiss_indicate_detect();
      }
    } else if (command == CMD_PROMISC) {
      if (sbyte == 0x01) {
        promisc_enable();
      } else if (sbyte == 0x00) {
        promisc_disable();
      }
      kiss_indicate_promisc();
    } else if (command == CMD_READY) {
      if (!queue_full()) {
        kiss_indicate_ready();
      } else {
        kiss_indicate_not_ready();
      }
    } else if (command == CMD_UNLOCK_ROM) {
      if (sbyte == ROM_UNLOCK_BYTE) {
        unlock_rom();
      }
    } else if (command == CMD_RESET) {
      if (sbyte == CMD_RESET_BYTE) {
        hard_reset();
      }
    } else if (command == CMD_ROM_READ) {
      kiss_dump_eeprom();
    } else if (command == CMD_CFG_READ) {
      kiss_dump_config();
    } else if (command == CMD_ROM_WRITE) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 2) {
          eeprom_write(cmdbuf[0], cmdbuf[1]);
        }
    } else if (command == CMD_FW_VERSION) {
      kiss_indicate_version();
    } else if (command == CMD_PLATFORM) {
      kiss_indicate_platform();
    } else if (command == CMD_MCU) {
      kiss_indicate_mcu();
    } else if (command == CMD_BOARD) {
      kiss_indicate_board();
    } else if (command == CMD_CONF_SAVE) {
      eeprom_conf_save();
    } else if (command == CMD_CONF_DELETE) {
      eeprom_conf_delete();
    } else if (command == CMD_FB_EXT) {
      #if HAS_DISPLAY == true
        if (sbyte == 0xFF) {
          kiss_indicate_fbstate();
        } else if (sbyte == 0x00) {
          ext_fb_disable();
          kiss_indicate_fbstate();
        } else if (sbyte == 0x01) {
          ext_fb_enable();
          kiss_indicate_fbstate();
        }
      #endif
    } else if (command == CMD_FB_WRITE) {
      if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }
        #if HAS_DISPLAY
          if (frame_len == 9) {
            uint8_t line = cmdbuf[0];
            if (line > 63) line = 63;
            int fb_o = line*8;
            memcpy(fb+fb_o, cmdbuf+1, 8);
          }
        #endif
    } else if (command == CMD_FB_READ) {
      if (sbyte != 0x00) { kiss_indicate_fb(); }
    } else if (command == CMD_DISP_READ) {
      if (sbyte != 0x00) { kiss_indicate_disp(); }
    } else if (command == CMD_DEV_HASH) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte != 0x00) {
          kiss_indicate_device_hash();
        }
      #endif
    } else if (command == CMD_DEV_SIG) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte == FESC) {
              ESCAPE = true;
          } else {
              if (ESCAPE) {
                  if (sbyte == TFEND) sbyte = FEND;
                  if (sbyte == TFESC) sbyte = FESC;
                  ESCAPE = false;
              }
              if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
          }

          if (frame_len == DEV_SIG_LEN) {
            memcpy(dev_sig, cmdbuf, DEV_SIG_LEN);
            device_save_signature();
          }
      #endif
    } else if (command == CMD_FW_UPD) {
      if (sbyte == 0x01) {
        firmware_update_mode = true;
      } else {
        firmware_update_mode = false;
      }
    } else if (command == CMD_HASHES) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte == 0x01) {
          kiss_indicate_target_fw_hash();
        } else if (sbyte == 0x02) {
          kiss_indicate_fw_hash();
        } else if (sbyte == 0x03) {
          kiss_indicate_bootloader_hash();
        } else if (sbyte == 0x04) {
          kiss_indicate_partition_table_hash();
        }
      #endif
    } else if (command == CMD_FW_HASH) {
      #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
        if (sbyte == FESC) {
              ESCAPE = true;
          } else {
              if (ESCAPE) {
                  if (sbyte == TFEND) sbyte = FEND;
                  if (sbyte == TFESC) sbyte = FESC;
                  ESCAPE = false;
              }
              if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
          }

          if (frame_len == DEV_HASH_LEN) {
            memcpy(dev_firmware_hash_target, cmdbuf, DEV_HASH_LEN);
            device_save_firmware_hash();
          }
      #endif
    } else if (command == CMD_WIFI_CHN) {
      #if HAS_WIFI
        if (sbyte > 0 && sbyte < 14) { eeprom_update(eeprom_addr(ADDR_CONF_WCHN), sbyte); }
      #endif
    } else if (command == CMD_WIFI_MODE) {
      #if HAS_WIFI
        if (sbyte == WR_WIFI_OFF || sbyte == WR_WIFI_STA || sbyte == WR_WIFI_AP) {
          wr_conf_save(sbyte);
          wifi_mode = sbyte;
          wifi_remote_init();
        }
      #endif
    } else if (command == CMD_WIFI_SSID) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (sbyte == 0x00) {
          for (uint8_t i = 0; i<33; i++) {
            if (i<frame_len && i<32) { eeprom_update(config_addr(ADDR_CONF_SSID+i), cmdbuf[i]); }
            else { eeprom_update(config_addr(ADDR_CONF_SSID+i), 0x00); }
          }
        }
      #endif
    } else if (command == CMD_WIFI_PSK) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (sbyte == 0x00) {
          for (uint8_t i = 0; i<33; i++) {
            if (i<frame_len && i<32) { eeprom_update(config_addr(ADDR_CONF_PSK+i), cmdbuf[i]); }
            else { eeprom_update(config_addr(ADDR_CONF_PSK+i), 0x00); }
          }
        }
      #endif
    } else if (command == CMD_WIFI_IP) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) { for (uint8_t i = 0; i<4; i++) { eeprom_update(config_addr(ADDR_CONF_IP+i), cmdbuf[i]); } }
      #endif
    } else if (command == CMD_WIFI_NM) {
      #if HAS_WIFI
        if (sbyte == FESC) { ESCAPE = true; }
        else {
          if (ESCAPE) {
            if (sbyte == TFEND) sbyte = FEND;
            if (sbyte == TFESC) sbyte = FESC;
            ESCAPE = false;
          }
          if (frame_len < CMD_L) cmdbuf[frame_len++] = sbyte;
        }

        if (frame_len == 4) { for (uint8_t i = 0; i<4; i++) { eeprom_update(config_addr(ADDR_CONF_NM+i), cmdbuf[i]); } }
      #endif
    } else if (command == CMD_BT_CTRL) {
      #if HAS_BLUETOOTH || HAS_BLE
        if (sbyte == 0x00) {
          bt_stop();
          bt_conf_save(false);
        } else if (sbyte == 0x01) {
          bt_start();
          bt_conf_save(true);
        } else if (sbyte == 0x02) {
          if (bt_state == BT_STATE_OFF) {
            bt_start();
            bt_conf_save(true);
          }
          if (bt_state != BT_STATE_CONNECTED) {
            bt_enable_pairing();
          }
        }
      #endif
    } else if (command == CMD_BT_UNPAIR) {
      #if HAS_BLE
        if (sbyte == 0x01) { bt_debond_all(); }
      #endif
    } else if (command == CMD_DISP_INT) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            display_intensity = sbyte;
            di_conf_save(display_intensity);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DISP_ADDR) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            display_addr = sbyte;
            da_conf_save(display_addr);
        }

      #endif
    } else if (command == CMD_DISP_BLNK) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            db_conf_save(sbyte);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DISP_ROT) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            drot_conf_save(sbyte);
            display_unblank();
        }
      #endif
    } else if (command == CMD_DIS_IA) {
      if (sbyte == FESC) {
          ESCAPE = true;
      } else {
          if (ESCAPE) {
              if (sbyte == TFEND) sbyte = FEND;
              if (sbyte == TFESC) sbyte = FESC;
              ESCAPE = false;
          }
          dia_conf_save(sbyte);
      }
    } else if (command == CMD_DISP_RCND) {
      #if HAS_DISPLAY
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (sbyte > 0x00) recondition_display = true;
        }
      #endif
    } else if (command == CMD_NP_INT) {
      #if HAS_NP
        if (sbyte == FESC) {
            ESCAPE = true;
        } else {
            if (ESCAPE) {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            sbyte;
            led_set_intensity(sbyte);
            np_int_conf_save(sbyte);
        }

      #endif
    }
  }
}

#if MCU_VARIANT == MCU_ESP32
  portMUX_TYPE update_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

bool medium_free() {
  update_modem_status();
  if (avoid_interference && interference_detected) { return false; }
  return !dcd;
}

bool noise_floor_sampled = false;
int noise_floor_sample = 0;
int noise_floor_buffer[NOISE_FLOOR_SAMPLES] = {0};
void update_noise_floor() {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    if (!dcd) {
      #if HAS_LORA_LNA == false
      if (!noise_floor_sampled || current_rssi < noise_floor + CSMA_INFR_THRESHOLD_DB) {
      #else
      if ((!noise_floor_sampled || current_rssi < noise_floor + CSMA_INFR_THRESHOLD_DB) || (noise_floor_sampled && (noise_floor < LNA_GD_THRSHLD && current_rssi <= LNA_GD_LIMIT))) {
      #endif
        #if HAS_LORA_LNA


          if (current_rssi < noise_floor-LORA_LNA_GVT) { return; }
        #endif
        bool sum_noise_floor = false;
        noise_floor_buffer[noise_floor_sample] = current_rssi;
        noise_floor_sample = noise_floor_sample+1;
        if (noise_floor_sample >= NOISE_FLOOR_SAMPLES) {
          noise_floor_sample %= NOISE_FLOOR_SAMPLES;
          noise_floor_sampled = true;
          sum_noise_floor = true;
        }

        if (noise_floor_sampled && sum_noise_floor) {
          noise_floor = 0;
          for (int ni = 0; ni < NOISE_FLOOR_SAMPLES; ni++) { noise_floor += noise_floor_buffer[ni]; }
          noise_floor /= NOISE_FLOOR_SAMPLES;
        }
      }
    }
  #endif
}

#define LED_ID_TRIG 16
uint8_t led_id_filter = 0;
uint32_t interference_start = 0;
bool interference_persists = false;
void update_modem_status() {
  #if MCU_VARIANT == MCU_ESP32
    portENTER_CRITICAL(&update_lock);
  #elif MCU_VARIANT == MCU_NRF52
    portENTER_CRITICAL();
  #endif

  bool carrier_detected = LoRa->dcd();
  current_rssi = LoRa->currentRssi();
  last_status_update = millis();

  #if MCU_VARIANT == MCU_ESP32
    portEXIT_CRITICAL(&update_lock);
  #elif MCU_VARIANT == MCU_NRF52
    portEXIT_CRITICAL();
  #endif

  #if HAS_LORA_LNA
    if (noise_floor > LNA_GD_THRSHLD) { interference_detected = !carrier_detected && (current_rssi > (noise_floor+CSMA_INFR_THRESHOLD_DB)); }
    else { interference_detected = !carrier_detected && (current_rssi > LNA_GD_LIMIT); }
  #else
    interference_detected = !carrier_detected && (current_rssi > (noise_floor+CSMA_INFR_THRESHOLD_DB));
  #endif

  if (interference_detected) { if (led_id_filter < LED_ID_TRIG) { led_id_filter += 1; } }
  else { if (led_id_filter > 0) {led_id_filter -= 1; } }




  if (interference_detected && current_rssi < CSMA_RFENV_RECAL_LIMIT_DB) {
    if (!interference_persists) { interference_persists = true; interference_start = millis(); }
    else {
      if (millis()-interference_start >= CSMA_RFENV_RECAL_MS) { noise_floor_sampled = false; interference_persists = false; }
    }
  } else { interference_persists = false; }

  if (carrier_detected) { dcd = true; } else { dcd = false; }

  dcd_led = dcd;
  if (dcd_led) { led_rx_on(); }
  else {
    if (interference_detected && noise_floor_sampled) {
      if (led_id_filter >= LED_ID_TRIG) { led_id_on(); }
    } else {
      if (airtime_lock) { led_indicate_airtime_lock(); }
      else { led_rx_off(); led_id_off(); }
    }
  }

  update_noise_floor();
}

void check_modem_status() {
  if (millis()-last_status_update >= status_interval_ms) {
    update_modem_status();

    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
      util_samples[dcd_sample] = dcd;
      dcd_sample = (dcd_sample+1)%DCD_SAMPLES;
      if (dcd_sample % UTIL_UPDATE_INTERVAL == 0) {
        int util_count = 0;
        for (int ui = 0; ui < DCD_SAMPLES; ui++) {
          if (util_samples[ui]) util_count++;
        }
        local_channel_util = (float)util_count / (float)DCD_SAMPLES;
        total_channel_util = local_channel_util + airtime;
        if (total_channel_util > 1.0) total_channel_util = 1.0;

        int16_t cb = current_airtime_bin();
        uint16_t nb = cb+1; if (nb == AIRTIME_BINS) { nb = 0; }
        if (total_channel_util > longterm_bins[cb]) longterm_bins[cb] = total_channel_util;
        longterm_bins[nb] = 0.0;

        update_airtime();
      }
    #endif
  }
}

void validate_status() {
  #if MCU_VARIANT == MCU_1284P
      uint8_t boot_flags = OPTIBOOT_MCUSR;
      uint8_t F_POR = PORF;
      uint8_t F_BOR = BORF;
      uint8_t F_WDR = WDRF;
  #elif MCU_VARIANT == MCU_2560
      uint8_t boot_flags = OPTIBOOT_MCUSR;
      if (boot_flags == 0x00) boot_flags = 0x03;
      uint8_t F_POR = PORF;
      uint8_t F_BOR = BORF;
      uint8_t F_WDR = WDRF;
  #elif MCU_VARIANT == MCU_ESP32

      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #elif MCU_VARIANT == MCU_NRF52

      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #elif MCU_VARIANT == MCU_NATIVE


      uint8_t boot_flags = 0x02;
      uint8_t F_POR = 0x00;
      uint8_t F_BOR = 0x00;
      uint8_t F_WDR = 0x01;
  #endif

  if (hw_ready || device_init_done) {
    hw_ready = false;
    printf("[init] Error, invalid hardware check state\r\n");
    #if HAS_DISPLAY
      if (disp_ready) {
        device_init_done = true;
        update_display();
      }
    #endif
    led_indicate_boot_error();
  }

  if (boot_flags & (1<<F_POR)) {
    boot_vector = START_FROM_POWERON;
  } else if (boot_flags & (1<<F_BOR)) {
    boot_vector = START_FROM_BROWNOUT;
  } else if (boot_flags & (1<<F_WDR)) {
    boot_vector = START_FROM_BOOTLOADER;
  } else {
      Serial.write("Error, indeterminate boot vector\r\n");
      #if HAS_DISPLAY
        if (disp_ready) {
          device_init_done = true;
          update_display();
        }
      #endif
      led_indicate_boot_error();
  }

  if (boot_vector == START_FROM_BOOTLOADER || boot_vector == START_FROM_POWERON) {
    if (eeprom_lock_set()) {
      if (eeprom_product_valid() && eeprom_model_valid() && eeprom_hwrev_valid()) {
#ifdef DISABLE_FIRMWARE_CHECKSUM



        if (true) {
#else
        if (eeprom_checksum_valid()) {
#endif
          eeprom_ok = true;
          if (modem_installed) {
            #if PLATFORM == PLATFORM_ESP32 || PLATFORM == PLATFORM_NRF52 || PLATFORM == PLATFORM_NATIVE
              if (device_init()) {
                hw_ready = true;
              } else {
                hw_ready = false;
                printf("[init] Error, device init failed\r\n");
              }
            #else
              hw_ready = true;
            #endif
          } else {
            hw_ready = false;
            printf("[init] No radio module found\r\n");
            #if HAS_DISPLAY
              if (disp_ready) {
                device_init_done = true;
                update_display();
              }
            #endif
          }

          if (hw_ready && eeprom_have_conf()) {
            eeprom_conf_load();
            op_mode = MODE_TNC;
            startRadio();
          }
        } else {
          hw_ready = false;
          printf("[init] Invalid EEPROM checksum\r\n");
          #if HAS_DISPLAY
            if (disp_ready) {
              device_init_done = true;
              update_display();
            }
          #endif
        }
      } else {
        hw_ready = false;
        printf("[init] Invalid EEPROM configuration\r\n");
        #if HAS_DISPLAY
          if (disp_ready) {
            device_init_done = true;
            update_display();
          }
        #endif
      }
    } else {
      hw_ready = false;
      printf("[init] Device unprovisioned, no device configuration found in EEPROM\r\n");
      #if HAS_DISPLAY
        if (disp_ready) {
          device_init_done = true;
          update_display();
        }
      #endif
    }
  } else {
    hw_ready = false;
    printf("[init] Error, incorrect boot vector\r\n");
    #if HAS_DISPLAY
      if (disp_ready) {
        device_init_done = true;
        update_display();
      }
    #endif
    led_indicate_boot_error();
  }
}

#if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
  void update_csma_parameters() {
    int airtime_pct = (int)(airtime*100);
    int new_cw_band = cw_band;

    if (airtime_pct <= CSMA_BAND_1_MAX_AIRTIME) { new_cw_band = 1; }
    else {
      int at = airtime_pct + CSMA_BAND_1_MAX_AIRTIME;
      new_cw_band = map(at, CSMA_BAND_1_MAX_AIRTIME, CSMA_BAND_N_MIN_AIRTIME, 2, CSMA_CW_BANDS);
    }

    if (new_cw_band > CSMA_CW_BANDS) { new_cw_band = CSMA_CW_BANDS; }
    if (new_cw_band != cw_band) {
      cw_band = (uint8_t)(new_cw_band);
      cw_min = (cw_band-1) * CSMA_CW_PER_BAND_WINDOWS;
      cw_max = (cw_band) * CSMA_CW_PER_BAND_WINDOWS - 1;
      kiss_indicate_csma_stats();
    }
  }
#endif

void tx_queue_handler() {
  if (!airtime_lock && queue_height > 0) {
    if (csma_cw == -1) {
      csma_cw = random(cw_min, cw_max);
      cw_wait_target = csma_cw * csma_slot_ms;
    }

    if (difs_wait_start == -1) {
      if (medium_free()) { difs_wait_start = millis(); return; }
      else { return; } }

    else {
      if (!medium_free()) { difs_wait_start = -1; cw_wait_start = -1; return; }
      else {
        if (millis() < difs_wait_start+difs_ms) { return; }
        else {
          if (cw_wait_start == -1) { cw_wait_start = millis(); return; }
          else {
            cw_wait_passed += millis()-cw_wait_start; cw_wait_start = millis();
            if (cw_wait_passed < cw_wait_target) { return; }
            else {
              bool should_flush = !lora_limit_rate && !lora_guard_rate;
              if (should_flush) { flush_queue(); } else { pop_queue(); }
              cw_wait_passed = 0; csma_cw = -1; difs_wait_start = -1; }
          }
        }
      }
    }
  }
}

void work_while_waiting() { loop(); }

void loop() {

  #if MCU_VARIANT == MCU_NATIVE





    extern bool native_reboot_pending();
    extern void native_reboot_perform();
    if (native_reboot_pending()) native_reboot_perform();
  #endif

#ifdef HAS_RNS

  if (reticulum) {
    try {
      reticulum.loop();
    }
    catch (const std::bad_alloc&) {
      ERROR("RNS loop failed: bad_alloc - out of memory");
    }
    catch (std::exception& e) {
      ERRORF("RNS loop failed: %s", e.what());
    }
  }
#endif

  if (radio_online) {
    #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NATIVE
      LoRa->handleDio0IfPending();
      modem_packet_t *modem_packet = NULL;
      if(modem_packet_queue && xQueueReceive(modem_packet_queue, &modem_packet, 0) == pdTRUE && modem_packet) {
        host_write_len = modem_packet->len;
        last_rssi = modem_packet->rssi;
        last_snr_raw = modem_packet->snr_raw;
        memcpy(&pbuf, modem_packet->data, modem_packet->len);
        free(modem_packet);
        modem_packet = NULL;

        kiss_indicate_stat_rssi();
        kiss_indicate_stat_snr();
        kiss_write_packet();
      }

      airtime_lock = false;
      if (st_airtime_limit != 0.0 && airtime >= st_airtime_limit) airtime_lock = true;
      if (lt_airtime_limit != 0.0 && longterm_airtime >= lt_airtime_limit) airtime_lock = true;

    #elif MCU_VARIANT == MCU_NRF52
      LoRa->handleDio0IfPending();
      modem_packet_t *modem_packet = NULL;
      if(modem_packet_queue && xQueueReceive(modem_packet_queue, &modem_packet, 0) == pdTRUE && modem_packet) {
        memcpy(&pbuf, modem_packet->data, modem_packet->len);
        host_write_len = modem_packet->len;
        free(modem_packet);
        modem_packet = NULL;

        portENTER_CRITICAL();
        last_rssi = LoRa->packetRssi();
        last_snr_raw = LoRa->packetSnrRaw();
        portEXIT_CRITICAL();
        kiss_indicate_stat_rssi();
        kiss_indicate_stat_snr();
        kiss_write_packet();
      }

      airtime_lock = false;
      if (st_airtime_limit != 0.0 && airtime >= st_airtime_limit) airtime_lock = true;
      if (lt_airtime_limit != 0.0 && longterm_airtime >= lt_airtime_limit) airtime_lock = true;

    #endif

    tx_queue_handler();
    check_modem_status();
    #if MCU_VARIANT == MCU_NATIVE



      native_kiss_tcp::check_active();
    #endif

  } else {
    if (hw_ready) {
      if (console_active) {
        #if HAS_CONSOLE
          console_loop();
        #endif
      } else {
        led_indicate_standby();
      }
    } else {

      led_indicate_not_ready();
      stopRadio();
    }
  }

  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
      buffer_serial();
      if (!fifo_isempty(&serialFIFO)) serial_poll();
  #else
    if (!fifo_isempty_locked(&serialFIFO)) serial_poll();
  #endif

  #if defined(ENABLE_WEBSOCKETS) && __has_include(<WiFi.h>)
    ws_console::service();
  #endif

  #if HAS_DISPLAY
    if (disp_ready && !display_updating) update_display();
  #endif

  #if BOARD_MODEL == BOARD_TDECK
    td_ui::update();
  #endif

  #if HAS_PMU
    if (pmu_ready) update_pmu();
  #endif

  #if HAS_BLUETOOTH || HAS_BLE == true
    if (!console_active && bt_ready) update_bt();
  #endif

  #if HAS_WIFI
    if (wifi_initialized) update_wifi();
  #endif

  #if HAS_INPUT
    input_read();
  #endif


#if MCU_VARIANT == MCU_ESP32
  esp_task_wdt_reset();
#elif MCU_VARIANT == MCU_NRF52
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
#endif

  if (memory_low) {
    #if PLATFORM == PLATFORM_ESP32
      if (esp_get_free_heap_size() < 8192) {
        kiss_indicate_error(ERROR_MEMORY_LOW); memory_low = false;
      } else {
        memory_low = false;
      }
    #else
      kiss_indicate_error(ERROR_MEMORY_LOW); memory_low = false;
    #endif
  }
}

void sleep_now() {
  #if HAS_SLEEP == true
    stopRadio();
    #if PLATFORM == PLATFORM_ESP32
      #if BOARD_MODEL == BOARD_T3S3 || BOARD_MODEL == BOARD_XIAO_S3
        #if HAS_DISPLAY
          display_intensity = 0;
          update_display(true);
        #endif
      #endif
      #if BOARD_MODEL == BOARD_HELTEC32_V4
          digitalWrite(LORA_PA_CPS, LOW);
          digitalWrite(LORA_PA_CSD, LOW);
          digitalWrite(LORA_PA_PWR_EN, LOW);
          digitalWrite(Vext, HIGH);
      #endif
      #if BOARD_MODEL == BOARD_HELTEC_TRACKER_V2
          digitalWrite(LORA_PA_CTX, LOW);
          digitalWrite(LORA_PA_CSD, LOW);
          digitalWrite(LORA_PA_PWR_EN, LOW);
          digitalWrite(Vext, VEXT_OFF);
      #endif
      #if PIN_DISP_SLEEP >= 0
        pinMode(PIN_DISP_SLEEP, OUTPUT);
        digitalWrite(PIN_DISP_SLEEP, DISP_SLEEP_LEVEL);
      #endif
      #if HAS_BLUETOOTH
        if (bt_state == BT_STATE_CONNECTED) {
          bt_stop();
          delay(100);
        }
      #endif
      esp_sleep_enable_ext0_wakeup(PIN_WAKEUP, WAKEUP_LEVEL);
      esp_deep_sleep_start();
    #elif PLATFORM == PLATFORM_NRF52
      #if BOARD_MODEL == BOARD_HELTEC_T114
        npset(0,0,0);
        digitalWrite(PIN_VEXT_EN, LOW);
        digitalWrite(PIN_T114_TFT_BLGT, HIGH);
        digitalWrite(PIN_T114_TFT_EN, HIGH);
      #elif BOARD_MODEL == BOARD_TECHO
        for (uint8_t i = display_intensity; i > 0; i--) { analogWrite(pin_backlight, i-1); delay(1); }
        epd_black(true); delay(300); epd_black(true); delay(300); epd_black(false);
        delay(2000);
        analogWrite(PIN_VEXT_EN, 0);
        delay(100);
      #endif
      sd_power_gpregret_set(0, 0x6d);
      nrf_gpio_cfg_sense_input(pin_btn_usr1, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
      NRF_POWER->SYSTEMOFF = 1;
    #endif
  #endif
}

void button_event(uint8_t event, unsigned long duration) {
  #if MCU_VARIANT == MCU_ESP32 || MCU_VARIANT == MCU_NRF52 || MCU_VARIANT == MCU_NATIVE
    if (display_blanked) {
      display_unblank();
    } else {
      if (duration > 10000) {
        #if HAS_CONSOLE
          #if HAS_BLUETOOTH || HAS_BLE
            bt_stop();
          #endif
          console_active = true;
          console_start();
        #endif
      } else if (duration > 5000) {
        #if HAS_BLUETOOTH || HAS_BLE
          if (bt_state != BT_STATE_CONNECTED) { bt_enable_pairing(); }
        #endif
      } else if (duration > 700) {
        #if HAS_SLEEP
          sleep_now();
        #endif
      } else {
        #if HAS_BLUETOOTH || HAS_BLE
        if (bt_state != BT_STATE_CONNECTED) {
          if (bt_state == BT_STATE_OFF) {
            bt_start();
            bt_conf_save(true);
          } else {
            bt_stop();
            bt_conf_save(false);
          }
        }
        #endif
      }
    }
  #endif
}

volatile bool serial_polling = false;
void serial_poll() {
  serial_polling = true;

  #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52 && MCU_VARIANT != MCU_NATIVE
  while (!fifo_isempty_locked(&serialFIFO)) {
  #else
  while (!fifo_isempty(&serialFIFO)) {
  #endif
    char sbyte = fifo_pop(&serialFIFO);
    serial_callback(sbyte);
  }

  serial_polling = false;
}

#if MCU_VARIANT != MCU_ESP32
  #define MAX_CYCLES 20
#else
  #define MAX_CYCLES 10
#endif
void buffer_serial() {
  if (!serial_buffering) {
    serial_buffering = true;

    uint8_t c = 0;

    #if MCU_VARIANT == MCU_NATIVE



    native_kiss_tcp::poll_accept();
    while (c < MAX_CYCLES && native_kiss_tcp::available()) {
      c++;
      if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, native_kiss_tcp::read()); }
    }
    #else
    #if HAS_BLUETOOTH || HAS_BLE == true
    while (
      c < MAX_CYCLES &&
      #if HAS_WIFI
      ( (bt_state != BT_STATE_CONNECTED && Serial.available()) || (bt_state == BT_STATE_CONNECTED && SerialBT.available()) || (wr_state >= WR_STATE_ON && wifi_remote_available()) )
      #else
      ( (bt_state != BT_STATE_CONNECTED && Serial.available()) || (bt_state == BT_STATE_CONNECTED && SerialBT.available()) )
      #endif
      )
    #else
    while (c < MAX_CYCLES && Serial.available())
    #endif
    {
      c++;

      #if MCU_VARIANT != MCU_ESP32 && MCU_VARIANT != MCU_NRF52
        if (!fifo_isfull_locked(&serialFIFO)) { fifo_push_locked(&serialFIFO, Serial.read()); }
      #elif HAS_BLUETOOTH || HAS_BLE == true || HAS_WIFI
        if (bt_state == BT_STATE_CONNECTED) { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, SerialBT.read()); } }
        #if HAS_WIFI
        else if (wifi_host_is_connected()) { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, wifi_remote_read()); } }
        #endif
        else { if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, Serial.read()); } }
      #else
        if (!fifo_isfull(&serialFIFO)) { fifo_push(&serialFIFO, Serial.read()); }
      #endif
    }
    #endif

    serial_buffering = false;
  }
}

void serial_interrupt_init() {
  #if MCU_VARIANT == MCU_1284P
      TCCR3A = 0;
      TCCR3B = _BV(CS10) |
               _BV(WGM33)|
               _BV(WGM32);


      ICR3 = 16000;
      TIMSK3 = _BV(ICIE3);

  #elif MCU_VARIANT == MCU_2560



      TCCR3A = 0;
      TCCR3B = _BV(CS10) |
               _BV(WGM33)|
               _BV(WGM32);


      ICR3 = 16000;
      TIMSK3 = _BV(ICIE3);

  #elif MCU_VARIANT == MCU_ESP32

  #endif

}

#if MCU_VARIANT == MCU_1284P || MCU_VARIANT == MCU_2560
  ISR(TIMER3_CAPT_vect) { buffer_serial(); }
#endif