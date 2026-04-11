#pragma once
#include <cstdint>

bool displayInit();
void displayShowImage(const uint8_t* packedBuffer);
void displayShowMessage(const char* msg);
void displayClear();
