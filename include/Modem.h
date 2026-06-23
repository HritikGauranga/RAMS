#pragma once
#include <cstdint> //cstdint is a 

bool Modem_init();
void Modem_task(void *pvParameters);

// Get modem signal strength (0-31, where 0=very weak, 31=excellent, -1=error)
int8_t Modem_getSignalStrength();

