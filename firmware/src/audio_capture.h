#pragma once
#include <cstdint>
#include <cstddef>

bool audioInit();
bool audioRecord(uint8_t* buffer, size_t bufferSize, size_t& bytesRecorded);
void audioDeinit();
