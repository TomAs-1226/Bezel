/* What hal_tab5.c and hal_tab5_net.c share. Not part of the HAL contract (that is hal.h). */
#pragma once

/* Wi-Fi through the C6, and mDNS. Called once from hal_start(), after NVS and the expanders (the C6's
 * power is E2.P0, switched on in power_init()). */
void hal_net_init(void);
/* The USB tether on the USB-A port. Called once from hal_settle(), just after the port's 5 V (E2.P3). */
void hal_net_tether_init(void);
