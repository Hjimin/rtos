#ifndef __MANAGER_H__
#define __MANAGER_H__

#include <stdint.h>

void manager_init();
uint32_t manager_get_ip();
void manager_set_ip(uint32_t ip);
uint32_t manager_get_gateway();
void manager_set_gateway(uint32_t gw);
uint32_t manager_get_netmask();
void manager_set_netmask(uint32_t nm);

#endif /* __MANAGER_H__ */
