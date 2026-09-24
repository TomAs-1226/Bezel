/* What hal_tab5.c and hal_tab5_net.c share. Not part of the HAL contract (that is hal.h). */
#pragma once

/* Wi-Fi through the C6, and mDNS. Called once from hal_start(), after NVS and the expanders (the C6's
 * power is E2.P0, switched on in power_init()). */
void hal_net_init(void);
/* The USB tether on the USB-A port. Called once from hal_settle(), just after the port's 5 V (E2.P3). */
void hal_net_tether_init(void);
/* The speaker, the microphones and the wake word (hal_tab5_audio.c). hal_audio_init() once from hal_start(),
 * after I2C and the expanders; hal_audio_volume() is hal_set_volume()'s codec half. */
void hal_audio_init(void);
void hal_audio_volume(float v01);
