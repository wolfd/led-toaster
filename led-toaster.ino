#include <Reactduino.h>
#include <Arduino.h>
#include <Battery.h>
#include <TM1637Display.h>

// keyswitch

// on/off switch
#define SWITCH_PIN 2

// 7-segment display
#define CLK 4
#define DIO 7

// Main light pins
// layout:
// 3 2 3
// 1 0 1
// 3 2 3
#define LIGHT_0 3
#define LIGHT_1 5
#define LIGHT_2 6
#define LIGHT_3 9

// fan pins
#define FAN_CENTER 8
#define FAN_SIDES 11

// mode selector potentiometer
#define MODE_SELECTOR_PIN A1
// analogRead values: 0, 339, 510, 694, 1020

#define MODE_LED_SINGLE 1
#define MODE_LED_HORIZ 2
#define MODE_LED_CROSS 3
#define MODE_LED_FULL 4
#define MODE_LED_STROBE 5

//  mode modifier potentiometer
#define MODIFIER_POT_PIN A2

// battery definitions
#define MAX_BATTERY_VOLTAGE 4200
#define MIN_BATTERY_VOLTAGE 3500

// strobe definitions
#define MAX_STROBE_MS 1000
#define MIN_STROBE_MS 20
#define STROBE_ON_MS 10

uint8_t mode = MODE_LED_SINGLE;

bool lightIsOn = false;

// battery
Battery battery(MIN_BATTERY_VOLTAGE, MAX_BATTERY_VOLTAGE, A0);
uint8_t lastBatteryPercent = 0;

TM1637Display display(CLK, DIO);
uint8_t displayData[] = {0xff, 0xff, 0xff, 0xff};

uint8_t lights[] = {LIGHT_0, LIGHT_1, LIGHT_2, LIGHT_3};

// 7-segment display show estimated time remaining
// or if false show percent
bool showEstTimeRemaining = true;

// fan
uint32_t fanOnTime = 0;
uint32_t fanSpinDownTime = 0;

// strobe
reaction strobeLoopId = -1;
uint16_t lastStrobeRate = 0;

void turnOn();
void turnOff();
uint8_t readMode();
void fanSpinDown();
void fanIntensity(uint8_t intensity);
void lightIntensity(uint8_t intensity);
uint32_t remainingSeconds(uint8_t lightMode);

Reactduino app([]() {
  // setup pins
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(FAN_CENTER, OUTPUT);
  pinMode(FAN_SIDES, OUTPUT);

  pinMode(LIGHT_0, OUTPUT);
  pinMode(LIGHT_1, OUTPUT);
  pinMode(LIGHT_2, OUTPUT);
  pinMode(LIGHT_3, OUTPUT);

  pinMode(MODE_SELECTOR_PIN, INPUT);
  pinMode(MODIFIER_POT_PIN, INPUT);

  lightIntensity(0);
  fanIntensity(0);

  // start serial
  Serial.begin(115200);
  while (!Serial)
    ;

  // setup battery display
  display.setBrightness(0);
  display.setSegments(displayData);

  // start recording battery data
  battery.begin(5000, 1.0, &sigmoidal);

  // switch "loop"
  app.onPinChange(SWITCH_PIN, []() {
    if (digitalRead(SWITCH_PIN))
    {
      // off (pullup)
      turnOff();
    }
    else
    {
      // on (pullup)
      turnOn();
    }
  });

  // fan spindown loop
  app.repeat(256, []() {
    // early exit if light is on to prevent spindown cycle
    if (lightIsOn)
    {
      return;
    }
    if (fanSpinDownTime == 0)
    {
      fanIntensity(0);
    }
    else if (fanSpinDownTime > 0 && fanSpinDownTime < 3000)
    {
      fanIntensity(1);
    }
    else if (fanSpinDownTime >= 3000 && fanSpinDownTime < 7000)
    {
      fanIntensity(2);
    }
    else if (fanSpinDownTime >= 7000)
    {
      fanIntensity(3);
    }

    // decrement cooldown counter
    if (fanSpinDownTime >= 256)
    {
      fanSpinDownTime -= 256;
    }
    else
    {
      fanSpinDownTime = 0;
    }
  });

  // battery loop and other periodic updates
  // also sets strobe value
  app.repeat(128, []() {
    lastBatteryPercent = battery.level();
    if (showEstTimeRemaining)
    {
      // will double read the battery when calling remainingSeconds
      // convert between seconds and MM:SS on display
      uint16_t seconds = remainingSeconds(mode);
      uint16_t timeMinutes = (seconds / 60) * 100;
      uint16_t timeSeconds = (seconds % 60);
      // turn on colon
      display.showNumberDecEx(timeMinutes + timeSeconds, 0b01000000);
    }
    else
    {
      display.showNumberDec(lastBatteryPercent);
    }

    // battery failsafe off
    if (lastBatteryPercent == 0)
    {
      // immediately trigger off, all subsequent calls should turn things off
      // no matter the value provided
      lightIntensity(0);
      fanIntensity(0);
      turnOff(); // so you have to toggle switch off-on to get it back on
    }

    // strobe code
    if (lightIsOn && mode == 5)
    {
      uint16_t strobeRate = analogRead(MODIFIER_POT_PIN);
      if (lastStrobeRate != strobeRate)
      {
        app.free(strobeLoopId); // cancel last loop
        uint16_t strobeDelayMs = MIN_STROBE_MS + ((strobeRate * (MAX_STROBE_MS - MIN_STROBE_MS)) / 1024);

        strobeLoopId = app.repeat(strobeDelayMs, []() {
          lightIntensity(MODE_LED_FULL);
          delay(STROBE_ON_MS);
          lightIntensity(0);
        });
      }
    }
  });

  // blink estimated time or percent
  app.repeat(2000, []() {
    showEstTimeRemaining = !showEstTimeRemaining;
  });

  // tight loop for mode switch
  app.onTick([]() {
    mode = readMode();

    if (lightIsOn)
    {
      if (mode < 5)
      {
        lightIntensity(mode); // is same
      }
    }
  });
});

// sets light on, corresponds with switch "on"
void turnOn()
{
  lightIsOn = true;
  display.setBrightness(7); // make 7-seg display brighter
  // fans need to be on to cool lights
  fanIntensity(3);
  digitalWrite(LED_BUILTIN, HIGH);
  if (mode < 5)
  {
    lightIntensity(mode); // mode is same as light intensity value
  }
}

// sets light off, corresponds with switch "off"
void turnOff()
{
  lightIsOn = false;
  display.setBrightness(0); // turn down display
  fanSpinDownTime = 10000; // in ms
  digitalWrite(LED_BUILTIN, LOW);
  lightIntensity(0); // off!
}

// sets amount of fans on
// input 0-3, output 0-3 fans on
void fanIntensity(uint8_t intensity)
{
  if (lastBatteryPercent == 0)
  {
    intensity = 0; // turn fans off!
  }
  Serial.print("Fans: ");
  Serial.println(intensity, DEC);
  switch (intensity)
  {
  case 0:
    digitalWrite(FAN_CENTER, LOW);
    digitalWrite(FAN_SIDES, LOW);
    break;
  case 1:
    digitalWrite(FAN_CENTER, HIGH);
    digitalWrite(FAN_SIDES, LOW);
    break;
  case 2:
    digitalWrite(FAN_CENTER, LOW);
    digitalWrite(FAN_SIDES, HIGH);
  case 3:
    digitalWrite(FAN_CENTER, HIGH);
    digitalWrite(FAN_SIDES, HIGH);
  default:
    digitalWrite(FAN_CENTER, HIGH);
    digitalWrite(FAN_SIDES, HIGH);
  }
}


// sets number of lights on
// input 0-4, output 0-9 lights on
void lightIntensity(uint8_t intensity)
{
  if (lastBatteryPercent == 0)
  {
    intensity = 0; // turn lights off!
  }
  for (uint8_t i = 0; i < intensity; i++)
  {
    digitalWrite(lights[i], HIGH);
  }
  for (uint8_t i = intensity; i < 4; i++)
  {
    digitalWrite(lights[i], LOW);
  }
}

// read resistor value on rotary switch
// each terminal on the rotary switch is hooked up to
// a resistor, and serial monitor was used to figure out analog
// values
uint8_t readMode()
{
  // 0, 339, 510, 694, 1020
  uint16_t analogValue = analogRead(MODE_SELECTOR_PIN);
  if (analogValue < 256)
  {
    // . (1)
    return MODE_LED_SINGLE;
  }
  else if (analogValue < 400)
  {
    // - (3)
    return MODE_LED_HORIZ;
  }
  else if (analogValue < 600)
  {
    // + (5)
    return MODE_LED_CROSS;
  }
  else if (analogValue < 900)
  {
    // full on (9)
    return MODE_LED_FULL;
  }
  else
  {
    // strobe
    return MODE_LED_STROBE;
  }
  // safe default
  return MODE_LED_SINGLE;
}

// calculate remaining seconds given a light mode
// uses battery estimation and does own curve calculation inline
uint32_t remainingSeconds(uint8_t lightMode)
{
  // ((2 * 5000mAh) * 33V) / ((numLights * 100W) + (3 * 1.2A * 12V))
  // (2 * 5Ah * 33V * 3600sec) / (numLights * 100W + 43W) -> seconds
  uint8_t numLights;
  switch (lightMode)
  {
  case MODE_LED_SINGLE:
    numLights = 1;
    break;
  case MODE_LED_HORIZ:
    numLights = 3;
    break;
  case MODE_LED_CROSS:
    numLights = 5;
    break;
  case MODE_LED_FULL:
    numLights = 9;
    break;
  }

  uint16_t permilliFull = 10 * (105 - (105 / (1 + pow(1.724 * (battery.voltage() - MIN_BATTERY_VOLTAGE) / (MAX_BATTERY_VOLTAGE - MIN_BATTERY_VOLTAGE), 5.5))));

  return max(0, (permilliFull * (2 * 5 * 33 * 36) / (numLights * 100 + 43)) / 10);
}
