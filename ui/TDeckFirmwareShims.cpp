// T-Deck firmware interface shims
// These provide stub implementations for functions the UI screens
// expect from the main firmware. On real hardware they are backed by
// the LoRa driver, PMU, etc. -- these stubs let the UI link cleanly.

#include <Arduino.h>
#include <stdint.h>

// dashboard globals
uint32_t lora_airtime_limit = 0;
uint32_t lora_packet_rx     = 0;
uint32_t lora_packet_tx     = 0;

// dashboard helpers
float battery_percent()         { return 0.0f; }
float read_battery_current()    { return 0.0f; }
float read_chip_temperature()   { return 25.0f; }

// settings setters
void request_restart()          { /* no-op stub */ }
void set_lora_frequency(float)           { /* no-op stub */ }
void set_lora_bandwidth(float)           { /* no-op stub */ }
void set_lora_spreading_factor(uint8_t)  { /* no-op stub */ }
void set_lora_coding_rate(uint8_t)       { /* no-op stub */ }
void set_lora_tx_power(int8_t)           { /* no-op stub */ }
void set_display_intensity(uint8_t)      { /* no-op stub */ }