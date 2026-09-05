// Entry point.  All behaviour lives in controller.cpp; this file exists only to
// satisfy the Arduino framework's setup()/loop() contract.
#include <Arduino.h>
#include "controller.h"

void setup() { controllerSetup(); }
void loop()  { controllerLoop(); }
