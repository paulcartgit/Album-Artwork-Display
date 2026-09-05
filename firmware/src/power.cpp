#include "power.h"
#include "config.h"
#include <Wire.h>
#include <XPowersLib.h>

static XPowersAXP2101 s_pmu;

bool powerBegin() {
    if (!s_pmu.begin(Wire, AXP2101_ADDR, I2C_SDA, I2C_SCL)) {
        // Not fatal: the rails this board needs are on by default.
        Serial.println("[Power] PMIC did not respond — running on default rails");
        return false;
    }
    s_pmu.setDC1Voltage(3300);   s_pmu.enableDC1();
    s_pmu.setALDO1Voltage(3300); s_pmu.enableALDO1();
    s_pmu.setALDO2Voltage(3300); s_pmu.enableALDO2();
    s_pmu.setALDO3Voltage(3300); s_pmu.enableALDO3();
    s_pmu.setALDO4Voltage(3300); s_pmu.enableALDO4();
    Serial.println("[Power] PMIC initialised — rails enabled");
    return true;
}
