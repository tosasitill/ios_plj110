#pragma once
#include <stdint.h>
/* X9 display keep-alive and pipeline discovery (wiki/MT6993-Display.md). */

void display_early_diag(void);
void display_power_diag(void);
void display_replay_config(void);
void display_lk_ungate(void);
void display_mutex_kick(void);
void display_panel_wake(int minimal);
void display_video_start(void);
void display_gce_diag(void);
void display_gce_signal(void);
void display_gce_post(void);
void display_mutex_arm(void);
void display_route_chain(int hold_ms);
void display_exdma_counters(const char *tag);
void display_green_hold(int loops);
void display_post_diag(void);
void display_backlight_on(void);
void fb_fill(uint32_t color);
void dsi_trigger_lk(void);
uint32_t dsi_intsta(void);
int dsi_wait_idle(void);
