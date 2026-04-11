#pragma once

// Download JPEG from URL, decode, scale to 800×480, dither, and push to display
bool pipelineProcessUrl(const char* url);

// Process a local JPEG file from SD card
bool pipelineProcessFile(const char* path);
