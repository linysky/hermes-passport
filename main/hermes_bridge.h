// main/hermes_bridge.c — Hermes Bridge client for AI Passport
// Connects to Hermes Desktop plugin via WiFi, provides bot selection + voice chat
#pragma once

#include "bsp_button.h"

// Hermes Bridge page interface (conforms to demo_entry_t pattern)
void hermes_bridge_enter(void);
void hermes_bridge_exit(void);
void hermes_bridge_key(bsp_btn_t btn, bsp_btn_ev_t ev);
