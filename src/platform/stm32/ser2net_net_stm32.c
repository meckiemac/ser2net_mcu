#include "adapters.h"

#if SER2NET_TARGET == SER2NET_PLATFORM_STM32

#include "ser2net_log.h"

const struct ser2net_network_if *
ser2net_stm32_get_network_if(const struct ser2net_stm32_network_cfg *cfg)
{
    (void) cfg;
    SER2NET_LOGW("ser2net_adapter", "STM32 network backend not implemented yet");
    return NULL;
}

void
ser2net_stm32_release_network_if(const struct ser2net_network_if *iface)
{
    (void) iface;
}

#endif /* SER2NET_PLATFORM_STM32 */
