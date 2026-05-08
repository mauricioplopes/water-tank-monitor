// ============================================================
// voltage_test.ino
// ============================================================
// Echo-Line Voltage Validation Sketch
//
// PURPOSE
// -------
// The JSN-SR04T (and HC-SR04) ECHO pin outputs 5 V pulses.
// The ESP32 GPIO pins are only rated for 3.3 V input.
// Connecting a 5 V ECHO directly to an ESP32 GPIO without
// protection will permanently damage the microcontroller.
//
// This sketch drives the TRIGGER pin continuously so that
// the sensor is always ranging and the ECHO line stays active.
// While this sketch is running, measure the voltage at the
// OUTPUT of each resistor voltage divider with an oscilloscope
// or a multimeter in "AC peak" / "max" mode.
//
// CIRCUIT (for each sensor)
// -------------------------
//   ECHO (5 V) ──── R1 (1 kΩ) ──┬── R2 (2.2 kΩ) ──── GND
//                                │
//                           Measure here
//                         (must be ≤ 3.3 V)
//
// Expected output: ≈ 3.36 V  (confirmed with oscilloscope)
//
// PROCEDURE
// ---------
//  1. Wire the sensors and voltage dividers.
//  2. Leave the voltage-divider output DISCONNECTED from the ESP32.
//  3. Flash this sketch and apply power.
//  4. Probe the divider output node of each sensor.
//  5. Confirm voltage ≤ 3.3 V before making any GPIO connection.
//
// ⚠️  Do NOT connect the ECHO lines to the ESP32 GPIOs until
//     the voltage measurement confirms they are safe.
//
// Author : Maurício Lopes
// ============================================================

const int triggerPin = 4; // GPIO4 drives the TRIGGER line

void setup() {
  pinMode(triggerPin, OUTPUT);
}

void loop() {
  // Emit a 10 µs HIGH pulse – the minimum required by JSN-SR04T
  // to start a ranging cycle.
  digitalWrite(triggerPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(triggerPin, LOW);

  // Wait ~1 ms before the next trigger pulse.
  // This keeps the sensor continuously active while you measure.
  delay(1);
}
