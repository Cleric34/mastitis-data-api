#pragma once

/*
  Permanent measurement conversion layer.

  Keep these mappings separate from mastitis_esp32.ino. The main firmware
  reads filtered ADC values; this file alone converts them to simulated EC
  and pH values. Change only these constants after a calibration exercise.

  These settings describe the prototype potentiometers, not laboratory pH
  or conductivity-sensor calibration. Replace the implementation later if
  physical sensor electronics replace the potentiometers.
*/

struct LinearCalibration {
  int rawLow;
  int rawHigh;
  float valueLow;
  float valueHigh;
};

// Confirmed on this ESP32 with the installed potentiometers:
// EC raw range = 0..4095; pH raw range = 0..4095.
// Keep these values unchanged when building the final mastitis firmware.
const LinearCalibration EC_CALIBRATION = {
  0, 4095,
  3.0f, 9.0f       // simulated EC in mS/cm
};

const LinearCalibration PH_CALIBRATION = {
  0, 4095,
  5.5f, 7.5f       // simulated pH
};

inline float applyLinearCalibration(int raw, const LinearCalibration &c) {
  if (c.rawHigh <= c.rawLow) return c.valueLow;
  float clamped = constrain(raw, c.rawLow, c.rawHigh);
  return c.valueLow +
    (clamped - c.rawLow) * (c.valueHigh - c.valueLow) /
    (float)(c.rawHigh - c.rawLow);
}

inline float ecFromAdc(int raw) { return applyLinearCalibration(raw, EC_CALIBRATION); }
inline float phFromAdc(int raw) { return applyLinearCalibration(raw, PH_CALIBRATION); }
