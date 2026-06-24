#pragma once
#include <cstdint>

bool Modem_init();
void Modem_task(void *pvParameters);
int8_t Modem_getSignalStrength();
void Modem_checkIncomingSMS();

