#include "adapters.h"

#if SER2NET_TARGET == SER2NET_PLATFORM_RP2040

#include "ser2net_log.h"

const struct ser2net_serial_if *
ser2net_rp2040_get_serial_if(const struct ser2net_rp2040_serial_port_cfg *cfg,
                             size_t num_ports)
{
    (void) cfg;
    (void) num_ports;
    SER2NET_LOGW("ser2net_adapter", "RP2040 serial backend not implemented yet");
    return NULL;
}

BaseType_t
ser2net_rp2040_register_port(const struct ser2net_rp2040_serial_port_cfg *cfg)
{
    (void) cfg;
    return SER2NET_OS_STATUS_FAIL;
}

void
ser2net_rp2040_unregister_port(int port_id)
{
    (void) port_id;
}

#endif /* SER2NET_PLATFORM_RP2040 */
