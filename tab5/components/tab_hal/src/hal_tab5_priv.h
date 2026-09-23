/* What hal_tab5.c and hal_tab5_net.c share. Not part of the HAL contract (that is hal.h). */
#pragma once

/* Wi-Fi through the C6, the USB tether on the USB-A port, mDNS. Called once from hal_init(), after
 * NVS and the expanders (the USB-A port's 5 V is E2.P3, switched on in power_init()). */
void hal_net_init(void);
