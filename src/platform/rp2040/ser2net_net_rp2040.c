#include "adapters.h"

#if SER2NET_TARGET == SER2NET_PLATFORM_RP2040

#include "ser2net_log.h"

const struct ser2net_network_if *
ser2net_rp2040_get_network_if(const struct ser2net_rp2040_network_cfg *cfg)
{
    (void) cfg;
    SER2NET_LOGW("ser2net_adapter", "RP2040 network backend not implemented yet");
    return NULL;
}

void
ser2net_rp2040_release_network_if(const struct ser2net_network_if *iface)
{
    (void) iface;
}

#endif /* SER2NET_PLATFORM_RP2040 */
