// Centralised extern declarations for firmware globals used across UI screens
// and other subsystems. Include this header instead of scattering redundant
// extern statements in individual .cpp files.
#pragma once

#include <cstdint>   // uint8_t, uint32_t, int8_t

// ── radio / transport globals (defined in RNode_Firmware.ino / Config.h) ──
extern float    lora_freq;
extern float    lora_bw;
extern uint8_t  lora_sf;
extern uint8_t  lora_cr;
extern int8_t   lora_txp;
extern bool     radio_online;
extern uint32_t lora_airtime_limit;
extern uint32_t lora_packet_rx;
extern uint32_t lora_packet_tx;

// ── display / power globals ─────────────────────────────────────────────
extern uint8_t display_intensity;

// ── setter functions (defined in RNode_Firmware.ino / Utilities.h) ──────
extern void set_lora_frequency(float freq);
extern void set_lora_bandwidth(float bw);
extern void set_lora_spreading_factor(uint8_t sf);
extern void set_lora_coding_rate(uint8_t cr);
extern void set_lora_tx_power(int8_t dbm);
extern void set_display_intensity(uint8_t level);
extern void request_restart();

// ── read‑only sensor helpers (defined in board‑specific .ino / .cpp) ────
extern float battery_percent();
extern float read_battery_voltage();
extern float read_battery_current();
extern float read_chip_temperature();