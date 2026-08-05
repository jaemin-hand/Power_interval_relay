#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pins_config.h"
#include "src/lcd/jd9165_lcd.h"
#include "src/touch/gt911_touch.h"

namespace {

constexpr uint8_t kRelayPin = RELAY_CH2_GPIO;
constexpr bool kRelayActiveHigh = true;
constexpr uint32_t kDefaultPulseMs = 1000;
constexpr uint32_t kMinimumPulseMs = 50;
constexpr uint32_t kMaximumPulseMs = 5000;
constexpr uint32_t kMinimumPhaseMs = 1000;
constexpr uint32_t kUiTimeStepMs = 1000;
constexpr uint32_t kMaximumOnMs = 5000;
constexpr uint32_t kMaximumOffMs = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kMaximumCycles = 1000000;
constexpr size_t kCommandBufferSize = 96;
constexpr size_t kMaximumSerialBytesPerLoop = 32;
constexpr uint32_t kUiRefreshMs = 100;
constexpr size_t kDisplayBufferLines = 60;
constexpr uint32_t kStartHoldMs = 1500;
constexpr uint32_t kStartGaugeCompleteMs = 250;
constexpr lv_coord_t kStartButtonWidth = 286;

enum class ControllerState : uint8_t {
  kIdle,
  kPrestartOff,
  kCycleOn,
  kCycleOff,
  kPaused,
  kPulse,
  kCompleted,
};

enum class AdjustField : uint8_t {
  kOnMs,
  kOffMs,
  kCycles,
};

struct TestConfig {
  uint32_t onMs = 1000;
  uint32_t offMs = 1000;
  uint32_t targetCycles = 3;
};

struct AdjustButtonData {
  AdjustField field;
  int8_t direction;
};

char commandBuffer[kCommandBufferSize] = {};
size_t commandLength = 0;
bool discardSerialUntilNewline = false;
bool relayIsOn = false;
ControllerState controllerState = ControllerState::kIdle;
TestConfig testConfig;
uint32_t stateDeadlineMs = 0;
uint32_t completedCycles = 0;
uint32_t relayOnStartedMs = 0;
uint32_t maximumOnOverrunMs = 0;

jd9165_lcd lcd(LCD_RST);
gt911_touch touch(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT);
lv_disp_draw_buf_t drawBuffer;
lv_color_t* drawBufferA = nullptr;
lv_color_t* drawBufferB = nullptr;
bool displayReady = false;
bool touchReleasedSinceBoot = false;
uint32_t lastUiRefreshMs = 0;

lv_obj_t* relayBadge = nullptr;
lv_obj_t* relayBadgeLabel = nullptr;
lv_obj_t* stateLabel = nullptr;
lv_obj_t* cycleLabel = nullptr;
lv_obj_t* remainingLabel = nullptr;
lv_obj_t* progressBar = nullptr;
lv_obj_t* onValueLabel = nullptr;
lv_obj_t* offValueLabel = nullptr;
lv_obj_t* cyclesValueLabel = nullptr;
lv_obj_t* startButton = nullptr;
lv_obj_t* startHoldFill = nullptr;
lv_obj_t* pauseButton = nullptr;
lv_obj_t* pauseButtonLabel = nullptr;
lv_obj_t* stopButton = nullptr;
lv_obj_t* configButtons[6] = {};

uint32_t uiOnMs = 1000;
uint32_t uiOffMs = 1000;
uint32_t uiCycles = 3;
bool startHoldActive = false;
bool startHoldTriggered = false;
uint32_t startHoldStartedMs = 0;
uint32_t startHoldCompletedMs = 0;
lv_coord_t startHoldLastWidth = -1;

AdjustButtonData adjustButtonData[6] = {
    {AdjustField::kOnMs, -1},     {AdjustField::kOnMs, 1},
    {AdjustField::kOffMs, -1},    {AdjustField::kOffMs, 1},
    {AdjustField::kCycles, -1},   {AdjustField::kCycles, 1},
};

void refreshConfigurationLabels();

const __FlashStringHelper* stateName(ControllerState state) {
  switch (state) {
    case ControllerState::kIdle:
      return F("IDLE");
    case ControllerState::kPrestartOff:
      return F("PRESTART_OFF");
    case ControllerState::kCycleOn:
      return F("CYCLE_ON");
    case ControllerState::kCycleOff:
      return F("CYCLE_OFF");
    case ControllerState::kPaused:
      return F("PAUSED");
    case ControllerState::kPulse:
      return F("PULSE");
    case ControllerState::kCompleted:
      return F("COMPLETED");
  }

  return F("UNKNOWN");
}

const char* uiStateName(ControllerState state) {
  switch (state) {
    case ControllerState::kIdle:
      return "READY";
    case ControllerState::kPrestartOff:
      return "SAFE OFF WAIT";
    case ControllerState::kCycleOn:
      return "POWER ON";
    case ControllerState::kCycleOff:
      return "POWER OFF";
    case ControllerState::kPaused:
      return "PAUSED";
    case ControllerState::kPulse:
      return "TEST PULSE";
    case ControllerState::kCompleted:
      return "COMPLETED";
  }

  return "FAULT";
}

bool hasDeadline(ControllerState state) {
  return state == ControllerState::kPrestartOff ||
         state == ControllerState::kCycleOn ||
         state == ControllerState::kCycleOff ||
         state == ControllerState::kPulse;
}

bool configurationEditable() {
  return controllerState == ControllerState::kIdle ||
         controllerState == ControllerState::kCompleted;
}

bool pauseAvailable() {
  return controllerState == ControllerState::kPrestartOff ||
         controllerState == ControllerState::kCycleOn ||
         controllerState == ControllerState::kCycleOff ||
         controllerState == ControllerState::kPaused;
}

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

uint8_t relayLevel(bool turnOn) {
  const bool high = turnOn == kRelayActiveHigh;
  return high ? HIGH : LOW;
}

void driveRelay(bool turnOn) {
  digitalWrite(kRelayPin, relayLevel(turnOn));
  relayIsOn = turnOn;
}

void printStatus(uint32_t nowMs) {
  Serial.print(F("STATUS state="));
  Serial.print(stateName(controllerState));
  Serial.print(F(" relay="));
  Serial.print(relayIsOn ? F("ON") : F("OFF"));
  Serial.print(F(" completed="));
  Serial.print(completedCycles);
  Serial.print('/');
  Serial.print(testConfig.targetCycles);
  Serial.print(F(" on_ms="));
  Serial.print(testConfig.onMs);
  Serial.print(F(" off_ms="));
  Serial.print(testConfig.offMs);
  Serial.print(F(" max_on_overrun_ms="));
  Serial.print(maximumOnOverrunMs);

  if (hasDeadline(controllerState)) {
    const int32_t remainingMs = static_cast<int32_t>(stateDeadlineMs - nowMs);
    Serial.print(F(" remaining_ms="));
    Serial.print(remainingMs > 0 ? remainingMs : 0);
  }

  Serial.println();
}

void printHelp() {
  Serial.println(F("COMMANDS"));
  Serial.println(F("  config <on_ms> <off_ms> <cycles>"));
  Serial.println(F("  start             Start after one safe OFF interval"));
  Serial.println(F("  pause             Force OFF and discard partial cycle"));
  Serial.println(F("  resume            Wait one OFF interval, then resume"));
  Serial.println(F("  stop | off        Force OFF and stop the run"));
  Serial.println(F("  pulse [50..5000]  Temporary GPIO3 test pulse"));
  Serial.println(F("  status            Show controller state"));
  Serial.println(F("  help              Show this help"));
}

bool parseBoundedUint32(const char* argument, uint32_t minimum,
                        uint32_t maximum, uint32_t& value) {
  if (argument == nullptr || *argument == '\0' || *argument == '-') {
    return false;
  }

  char* end = nullptr;
  const unsigned long parsed = std::strtoul(argument, &end, 10);
  if (end == argument || *end != '\0' || parsed < minimum ||
      parsed > maximum) {
    return false;
  }

  value = static_cast<uint32_t>(parsed);
  return true;
}

void beginCycleOn() {
  controllerState = ControllerState::kCycleOn;
  Serial.print(F("PHASE ON cycle="));
  Serial.print(completedCycles + 1);
  Serial.print(F(" duration_ms="));
  Serial.println(testConfig.onMs);

  // Establish the bounded ON deadline immediately before energizing.
  relayOnStartedMs = millis();
  stateDeadlineMs = relayOnStartedMs + testConfig.onMs;
  driveRelay(true);
}

void beginCycleOff() {
  const uint32_t relayOffMs = millis();
  driveRelay(false);
  const uint32_t actualOnMs = relayOffMs - relayOnStartedMs;
  const uint32_t overrunMs =
      actualOnMs > testConfig.onMs ? actualOnMs - testConfig.onMs : 0;
  if (overrunMs > maximumOnOverrunMs) {
    maximumOnOverrunMs = overrunMs;
  }
  controllerState = ControllerState::kCycleOff;
  stateDeadlineMs = relayOffMs + testConfig.offMs;

  Serial.print(F("PHASE OFF cycle="));
  Serial.print(completedCycles + 1);
  Serial.print(F(" duration_ms="));
  Serial.print(testConfig.offMs);
  Serial.print(F(" actual_on_ms="));
  Serial.print(actualOnMs);
  Serial.print(F(" overrun_ms="));
  Serial.println(overrunMs);
}

void beginSafeOffWait(const __FlashStringHelper* reason) {
  driveRelay(false);
  controllerState = ControllerState::kPrestartOff;
  stateDeadlineMs = millis() + testConfig.offMs;

  Serial.print(F("SAFE_OFF reason="));
  Serial.print(reason);
  Serial.print(F(" duration_ms="));
  Serial.println(testConfig.offMs);
}

void stopRun(const __FlashStringHelper* reason) {
  driveRelay(false);
  controllerState = ControllerState::kIdle;
  stateDeadlineMs = 0;

  Serial.print(F("RUN_STOPPED reason="));
  Serial.print(reason);
  Serial.print(F(" completed="));
  Serial.println(completedCycles);
}

void completeRun() {
  driveRelay(false);
  controllerState = ControllerState::kCompleted;
  stateDeadlineMs = 0;

  Serial.print(F("RUN_COMPLETE completed="));
  Serial.println(completedCycles);
}

bool applyConfiguration(uint32_t onMs, uint32_t offMs, uint32_t cycles) {
  if (!configurationEditable()) {
    Serial.println(F("ERROR config allowed only while idle or completed"));
    return false;
  }

  if (onMs < kMinimumPhaseMs || onMs > kMaximumOnMs ||
      offMs < kMinimumPhaseMs || offMs > kMaximumOffMs || cycles < 1 ||
      cycles > kMaximumCycles) {
    Serial.println(F("ERROR config: on=1000..5000 ms, off=1000..86400000 ms, cycles=1..1000000"));
    return false;
  }

  testConfig = {onMs, offMs, cycles};
  uiOnMs = onMs;
  uiOffMs = offMs;
  uiCycles = cycles;
  completedCycles = 0;
  maximumOnOverrunMs = 0;
  controllerState = ControllerState::kIdle;
  stateDeadlineMs = 0;
  driveRelay(false);

  Serial.print(F("CONFIG on_ms="));
  Serial.print(testConfig.onMs);
  Serial.print(F(" off_ms="));
  Serial.print(testConfig.offMs);
  Serial.print(F(" cycles="));
  Serial.println(testConfig.targetCycles);
  if (displayReady) {
    refreshConfigurationLabels();
  }
  return true;
}

bool startRun() {
  if (!configurationEditable()) {
    Serial.println(F("ERROR start unavailable in current state"));
    return false;
  }

  completedCycles = 0;
  Serial.print(F("RUN_STARTED target="));
  Serial.println(testConfig.targetCycles);
  beginSafeOffWait(F("start"));
  return true;
}

bool pauseRun() {
  if (controllerState != ControllerState::kPrestartOff &&
      controllerState != ControllerState::kCycleOn &&
      controllerState != ControllerState::kCycleOff) {
    Serial.println(F("ERROR pause unavailable in current state"));
    return false;
  }

  driveRelay(false);
  controllerState = ControllerState::kPaused;
  stateDeadlineMs = 0;
  Serial.print(F("RUN_PAUSED completed="));
  Serial.println(completedCycles);
  return true;
}

bool resumeRun() {
  if (controllerState != ControllerState::kPaused) {
    Serial.println(F("ERROR resume requires paused state"));
    return false;
  }

  Serial.println(F("RUN_RESUMED"));
  beginSafeOffWait(F("resume"));
  return true;
}

void updateController(uint32_t nowMs) {
  if (!hasDeadline(controllerState) ||
      !deadlineReached(nowMs, stateDeadlineMs)) {
    return;
  }

  switch (controllerState) {
    case ControllerState::kPrestartOff:
      beginCycleOn();
      break;

    case ControllerState::kCycleOn:
      beginCycleOff();
      break;

    case ControllerState::kCycleOff:
      ++completedCycles;
      Serial.print(F("CYCLE_COMPLETE completed="));
      Serial.println(completedCycles);

      if (completedCycles >= testConfig.targetCycles) {
        completeRun();
      } else {
        beginCycleOn();
      }
      break;

    case ControllerState::kPulse:
      driveRelay(false);
      controllerState = ControllerState::kIdle;
      stateDeadlineMs = 0;
      Serial.println(F("PULSE_COMPLETE relay=OFF"));
      break;

    case ControllerState::kIdle:
    case ControllerState::kPaused:
    case ControllerState::kCompleted:
      driveRelay(false);
      stateDeadlineMs = 0;
      break;

    default:
      stopRun(F("invalid_state"));
      break;
  }
}

void handleConfigCommand() {
  uint32_t onMs = 0;
  uint32_t offMs = 0;
  uint32_t cycles = 0;
  const char* onArgument = std::strtok(nullptr, " \t");
  const char* offArgument = std::strtok(nullptr, " \t");
  const char* cyclesArgument = std::strtok(nullptr, " \t");
  const char* extraArgument = std::strtok(nullptr, " \t");

  if (!parseBoundedUint32(onArgument, kMinimumPhaseMs, kMaximumOnMs,
                          onMs) ||
      !parseBoundedUint32(offArgument, kMinimumPhaseMs, kMaximumOffMs,
                          offMs) ||
      !parseBoundedUint32(cyclesArgument, 1, kMaximumCycles, cycles) ||
      extraArgument != nullptr) {
    Serial.println(F("ERROR config: on=1000..5000 ms, off=1000..86400000 ms, cycles=1..1000000"));
    return;
  }

  applyConfiguration(onMs, offMs, cycles);
}

void handlePulseCommand() {
  if (!configurationEditable()) {
    Serial.println(F("ERROR pulse unavailable while a run is active"));
    return;
  }

  uint32_t durationMs = kDefaultPulseMs;
  const char* durationArgument = std::strtok(nullptr, " \t");
  const char* extraArgument = std::strtok(nullptr, " \t");

  if ((durationArgument != nullptr &&
       !parseBoundedUint32(durationArgument, kMinimumPulseMs,
                           kMaximumPulseMs, durationMs)) ||
      extraArgument != nullptr) {
    Serial.println(F("ERROR pulse duration must be 50..5000 ms"));
    return;
  }

  Serial.print(F("PULSE duration_ms="));
  Serial.println(durationMs);
  controllerState = ControllerState::kPulse;
  stateDeadlineMs = millis() + durationMs;
  driveRelay(true);
}

void handleCommand(char* line, uint32_t nowMs) {
  char* command = std::strtok(line, " \t");
  if (command == nullptr) {
    return;
  }

  if (std::strcmp(command, "config") == 0) {
    handleConfigCommand();
    return;
  }

  if (std::strcmp(command, "start") == 0) {
    if (std::strtok(nullptr, " \t") != nullptr) {
      Serial.println(F("ERROR start takes no arguments"));
      return;
    }
    startRun();
    return;
  }

  if (std::strcmp(command, "pause") == 0) {
    if (std::strtok(nullptr, " \t") != nullptr) {
      Serial.println(F("ERROR pause takes no arguments"));
      return;
    }
    pauseRun();
    return;
  }

  if (std::strcmp(command, "resume") == 0) {
    if (std::strtok(nullptr, " \t") != nullptr) {
      Serial.println(F("ERROR resume takes no arguments"));
      return;
    }
    resumeRun();
    return;
  }

  if (std::strcmp(command, "stop") == 0 ||
      std::strcmp(command, "off") == 0) {
    if (std::strtok(nullptr, " \t") != nullptr) {
      Serial.println(F("ERROR stop/off takes no arguments"));
      return;
    }
    stopRun(F("user"));
    return;
  }

  if (std::strcmp(command, "pulse") == 0) {
    handlePulseCommand();
    return;
  }

  if (std::strcmp(command, "status") == 0) {
    if (std::strtok(nullptr, " \t") != nullptr) {
      Serial.println(F("ERROR status takes no arguments"));
      return;
    }
    printStatus(nowMs);
    return;
  }

  if (std::strcmp(command, "help") == 0) {
    if (std::strtok(nullptr, " \t") != nullptr) {
      Serial.println(F("ERROR help takes no arguments"));
      return;
    }
    printHelp();
    return;
  }

  Serial.println(F("ERROR unknown command; type help"));
}

void readSerialCommands() {
  size_t processedBytes = 0;

  while (Serial.available() > 0 &&
         processedBytes < kMaximumSerialBytesPerLoop) {
    const char received = static_cast<char>(Serial.read());
    ++processedBytes;

    if (received == '\r') {
      continue;
    }

    if (discardSerialUntilNewline) {
      if (received == '\n') {
        discardSerialUntilNewline = false;
      }
      continue;
    }

    if (received == '\n') {
      commandBuffer[commandLength] = '\0';
      handleCommand(commandBuffer, millis());
      commandLength = 0;
      continue;
    }

    if (commandLength + 1 >= kCommandBufferSize) {
      commandLength = 0;
      discardSerialUntilNewline = true;
      stopRun(F("command_overflow"));
      Serial.println(F("ERROR command too long; relay forced OFF"));
      continue;
    }

    commandBuffer[commandLength++] = received;
  }
}

void setEnabled(lv_obj_t* object, bool enabled) {
  if (object == nullptr) {
    return;
  }

  if (enabled) {
    lv_obj_clear_state(object, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(object, LV_STATE_DISABLED);
  }
}

void formatDuration(uint32_t durationMs, char* output, size_t outputSize) {
  if (durationMs >= 60000) {
    const uint32_t minutes = durationMs / 60000;
    const uint32_t seconds = (durationMs % 60000) / 1000;
    std::snprintf(output, outputSize, "%lu:%02lu min",
                  static_cast<unsigned long>(minutes),
                  static_cast<unsigned long>(seconds));
  } else if (durationMs >= 1000) {
    std::snprintf(output, outputSize, "%lu.%lu s",
                  static_cast<unsigned long>(durationMs / 1000),
                  static_cast<unsigned long>((durationMs % 1000) / 100));
  } else {
    std::snprintf(output, outputSize, "%lu ms",
                  static_cast<unsigned long>(durationMs));
  }
}

void refreshConfigurationLabels() {
  if (onValueLabel == nullptr) {
    return;
  }

  char text[32];
  formatDuration(uiOnMs, text, sizeof(text));
  lv_label_set_text(onValueLabel, text);
  formatDuration(uiOffMs, text, sizeof(text));
  lv_label_set_text(offValueLabel, text);
  std::snprintf(text, sizeof(text), "%lu",
                static_cast<unsigned long>(uiCycles));
  lv_label_set_text(cyclesValueLabel, text);
}

void updateUi(uint32_t nowMs, bool force) {
  if (!displayReady || relayBadge == nullptr ||
      (!force && static_cast<uint32_t>(nowMs - lastUiRefreshMs) <
                     kUiRefreshMs)) {
    return;
  }
  lastUiRefreshMs = nowMs;

  lv_obj_set_style_bg_color(
      relayBadge, relayIsOn ? lv_color_hex(0xE5484D) : lv_color_hex(0x16865C),
      LV_PART_MAIN);
  lv_label_set_text(relayBadgeLabel, relayIsOn ? "RELAY ON" : "RELAY OFF");

  char text[80];
  std::snprintf(text, sizeof(text), "STATE  %s", uiStateName(controllerState));
  lv_label_set_text(stateLabel, text);
  std::snprintf(text, sizeof(text), "CYCLES  %lu / %lu",
                static_cast<unsigned long>(completedCycles),
                static_cast<unsigned long>(testConfig.targetCycles));
  lv_label_set_text(cycleLabel, text);

  if (hasDeadline(controllerState)) {
    const int32_t remaining = static_cast<int32_t>(stateDeadlineMs - nowMs);
    char duration[32];
    formatDuration(remaining > 0 ? static_cast<uint32_t>(remaining) : 0,
                   duration, sizeof(duration));
    std::snprintf(text, sizeof(text), "REMAINING  %s", duration);
  } else {
    std::snprintf(text, sizeof(text), "REMAINING  --");
  }
  lv_label_set_text(remainingLabel, text);

  const uint32_t progress =
      testConfig.targetCycles == 0
          ? 0
          : static_cast<uint32_t>((static_cast<uint64_t>(completedCycles) *
                                   1000ULL) /
                                  testConfig.targetCycles);
  lv_bar_set_value(progressBar, progress > 1000 ? 1000 : progress,
                   LV_ANIM_OFF);

  const bool editable = configurationEditable();
  for (lv_obj_t* button : configButtons) {
    setEnabled(button, editable);
  }
  setEnabled(startButton, editable);
  setEnabled(pauseButton, pauseAvailable());
  lv_label_set_text(pauseButtonLabel,
                    controllerState == ControllerState::kPaused ? "RESUME"
                                                                : "PAUSE");
}

void displayFlush(lv_disp_drv_t* display, const lv_area_t* area,
                  lv_color_t* colorData) {
  lcd.lcd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                      reinterpret_cast<uint16_t*>(colorData));
  lv_disp_flush_ready(display);
}

void touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t touchX = 0;
  uint16_t touchY = 0;
  if (!touch.getTouch(&touchX, &touchY)) {
    data->state = LV_INDEV_STATE_REL;
    touchReleasedSinceBoot = true;
    return;
  }

  // Ignore a finger or stale controller sample that was already present while
  // booting. A clean release is required before any UI control can activate.
  if (!touchReleasedSinceBoot) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  data->state = LV_INDEV_STATE_PR;
  data->point.x = touchX < LCD_H_RES ? touchX : LCD_H_RES - 1;
  data->point.y = touchY < LCD_V_RES ? touchY : LCD_V_RES - 1;
}

lv_obj_t* createPanel(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                      lv_coord_t width, lv_coord_t height) {
  lv_obj_t* panel = lv_obj_create(parent);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_size(panel, width, height);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x151D30), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x29344D), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(panel, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
  return panel;
}

lv_obj_t* createButton(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t width, lv_coord_t height, const char* text,
                       uint32_t color, lv_event_cb_t callback,
                       void* userData = nullptr,
                       lv_event_code_t eventCode = LV_EVENT_CLICKED) {
  lv_obj_t* button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(button, 14, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(button, callback, eventCode, userData);

  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_center(label);
  return button;
}

void onAdjustButton(lv_event_t* event) {
  if (!configurationEditable()) {
    return;
  }

  auto* data = static_cast<AdjustButtonData*>(lv_event_get_user_data(event));
  if (data == nullptr) {
    return;
  }

  uint32_t* value = nullptr;
  uint32_t minimum = 0;
  uint32_t maximum = 0;
  uint32_t step = 0;
  const bool repeating =
      lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT;
  switch (data->field) {
    case AdjustField::kOnMs:
      value = &uiOnMs;
      minimum = kMinimumPhaseMs;
      maximum = kMaximumOnMs;
      step = kUiTimeStepMs;
      break;
    case AdjustField::kOffMs:
      value = &uiOffMs;
      minimum = kMinimumPhaseMs;
      maximum = 60000;
      step = kUiTimeStepMs;
      break;
    case AdjustField::kCycles:
      value = &uiCycles;
      minimum = 1;
      maximum = kMaximumCycles;
      step = repeating ? (uiCycles >= 100 ? 100 : (uiCycles >= 10 ? 10 : 1))
                       : 1;
      break;
  }

  if (value == nullptr) {
    return;
  }

  if (data->direction < 0) {
    *value = *value > minimum + step - 1 ? *value - step : minimum;
  } else {
    *value = *value < maximum - step + 1 ? *value + step : maximum;
  }
  refreshConfigurationLabels();
}

void setStartHoldGaugeWidth(lv_coord_t width) {
  if (startHoldFill == nullptr) {
    return;
  }

  if (width <= 0) {
    lv_obj_add_flag(startHoldFill, LV_OBJ_FLAG_HIDDEN);
    startHoldLastWidth = 0;
    return;
  }

  if (width > kStartButtonWidth) {
    width = kStartButtonWidth;
  }
  lv_obj_clear_flag(startHoldFill, LV_OBJ_FLAG_HIDDEN);
  if (width != startHoldLastWidth) {
    lv_obj_set_width(startHoldFill, width);
    startHoldLastWidth = width;
  }
}

void updateStartHoldGauge(uint32_t nowMs) {
  if (startHoldActive) {
    const uint32_t elapsedMs = nowMs - startHoldStartedMs;
    const uint32_t boundedElapsedMs =
        elapsedMs < kStartHoldMs ? elapsedMs : kStartHoldMs;
    const lv_coord_t width = static_cast<lv_coord_t>(
        (static_cast<uint64_t>(kStartButtonWidth) * boundedElapsedMs) /
        kStartHoldMs);
    setStartHoldGaugeWidth(width > 0 ? width : 1);
    return;
  }

  if (startHoldTriggered &&
      static_cast<uint32_t>(nowMs - startHoldCompletedMs) <
          kStartGaugeCompleteMs) {
    setStartHoldGaugeWidth(kStartButtonWidth);
    return;
  }

  startHoldTriggered = false;
  setStartHoldGaugeWidth(0);
}

void onStartHoldEvent(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED && configurationEditable()) {
    startHoldActive = true;
    startHoldTriggered = false;
    startHoldStartedMs = millis();
    setStartHoldGaugeWidth(1);
    return;
  }

  if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
      !startHoldTriggered) {
    startHoldActive = false;
    setStartHoldGaugeWidth(0);
  }
}

void onStartButton(lv_event_t*) {
  if (applyConfiguration(uiOnMs, uiOffMs, uiCycles) && startRun()) {
    startHoldActive = false;
    startHoldTriggered = true;
    startHoldCompletedMs = millis();
    setStartHoldGaugeWidth(kStartButtonWidth);
  } else {
    startHoldActive = false;
    startHoldTriggered = false;
    setStartHoldGaugeWidth(0);
  }
  updateUi(millis(), true);
}

void onPauseButton(lv_event_t*) {
  if (controllerState == ControllerState::kPaused) {
    resumeRun();
  } else {
    pauseRun();
  }
  updateUi(millis(), true);
}

void onStopButton(lv_event_t*) {
  stopRun(F("touch"));
  updateUi(millis(), true);
}

void createConfigurationRow(lv_obj_t* parent, lv_coord_t y,
                            const char* title, lv_obj_t** valueLabel,
                            size_t buttonDataIndex) {
  lv_obj_t* titleLabel = lv_label_create(parent);
  lv_label_set_text(titleLabel, title);
  lv_obj_set_pos(titleLabel, 24, y + 18);
  lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xAAB6CF),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_18,
                             LV_PART_MAIN);

  *valueLabel = lv_label_create(parent);
  lv_obj_set_pos(*valueLabel, 154, y + 16);
  lv_obj_set_width(*valueLabel, 210);
  lv_obj_set_style_text_align(*valueLabel, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_set_style_text_color(*valueLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(*valueLabel, &lv_font_montserrat_24,
                             LV_PART_MAIN);

  configButtons[buttonDataIndex] =
      createButton(parent, 386, y, 70, 58, "-", 0x34415E,
                   onAdjustButton, &adjustButtonData[buttonDataIndex]);
  configButtons[buttonDataIndex + 1] =
      createButton(parent, 478, y, 70, 58, "+", 0x34415E,
                   onAdjustButton, &adjustButtonData[buttonDataIndex + 1]);
  lv_obj_add_event_cb(configButtons[buttonDataIndex], onAdjustButton,
                      LV_EVENT_LONG_PRESSED_REPEAT,
                      &adjustButtonData[buttonDataIndex]);
  lv_obj_add_event_cb(configButtons[buttonDataIndex + 1], onAdjustButton,
                      LV_EVENT_LONG_PRESSED_REPEAT,
                      &adjustButtonData[buttonDataIndex + 1]);
}

void createUi() {
  lv_obj_t* screen = lv_scr_act();
  lv_obj_clean(screen);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A1020), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(screen);
  lv_label_set_text(title, "POWER CYCLE TESTER");
  lv_obj_set_pos(title, 28, 20);
  lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);

  lv_obj_t* pinLabel = lv_label_create(screen);
  lv_label_set_text(pinLabel, "GPIO3  |  RELAY CH2  |  HIGH TRIGGER");
  lv_obj_align(pinLabel, LV_ALIGN_TOP_RIGHT, -28, 29);
  lv_obj_set_style_text_color(pinLabel, lv_color_hex(0x8290AA),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(pinLabel, &lv_font_montserrat_18,
                             LV_PART_MAIN);

  lv_obj_t* statusPanel = createPanel(screen, 24, 78, 360, 392);
  lv_obj_t* statusTitle = lv_label_create(statusPanel);
  lv_label_set_text(statusTitle, "STATUS");
  lv_obj_set_pos(statusTitle, 22, 18);
  lv_obj_set_style_text_color(statusTitle, lv_color_hex(0x8290AA),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(statusTitle, &lv_font_montserrat_18,
                             LV_PART_MAIN);

  relayBadge = lv_obj_create(statusPanel);
  lv_obj_set_pos(relayBadge, 22, 54);
  lv_obj_set_size(relayBadge, 316, 92);
  lv_obj_clear_flag(relayBadge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(relayBadge, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(relayBadge, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(relayBadge, 0, LV_PART_MAIN);
  relayBadgeLabel = lv_label_create(relayBadge);
  lv_obj_set_style_text_color(relayBadgeLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(relayBadgeLabel, &lv_font_montserrat_32,
                             LV_PART_MAIN);
  lv_obj_center(relayBadgeLabel);

  stateLabel = lv_label_create(statusPanel);
  lv_obj_set_pos(stateLabel, 24, 174);
  lv_obj_set_style_text_color(stateLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(stateLabel, &lv_font_montserrat_20,
                             LV_PART_MAIN);

  cycleLabel = lv_label_create(statusPanel);
  lv_obj_set_pos(cycleLabel, 24, 220);
  lv_obj_set_style_text_color(cycleLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(cycleLabel, &lv_font_montserrat_20,
                             LV_PART_MAIN);

  remainingLabel = lv_label_create(statusPanel);
  lv_obj_set_pos(remainingLabel, 24, 266);
  lv_obj_set_style_text_color(remainingLabel, lv_color_hex(0xAAB6CF),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(remainingLabel, &lv_font_montserrat_18,
                             LV_PART_MAIN);

  progressBar = lv_bar_create(statusPanel);
  lv_obj_set_pos(progressBar, 24, 326);
  lv_obj_set_size(progressBar, 312, 22);
  lv_bar_set_range(progressBar, 0, 1000);
  lv_obj_set_style_bg_color(progressBar, lv_color_hex(0x26324A),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(progressBar, lv_color_hex(0x4C8DFF),
                            LV_PART_INDICATOR);
  lv_obj_set_style_radius(progressBar, 11, LV_PART_MAIN);
  lv_obj_set_style_radius(progressBar, 11, LV_PART_INDICATOR);

  lv_obj_t* configPanel = createPanel(screen, 404, 78, 596, 392);
  lv_obj_t* configTitle = lv_label_create(configPanel);
  lv_label_set_text(configTitle, "TEST SETTINGS");
  lv_obj_set_pos(configTitle, 24, 18);
  lv_obj_set_style_text_color(configTitle, lv_color_hex(0x8290AA),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(configTitle, &lv_font_montserrat_18,
                             LV_PART_MAIN);

  createConfigurationRow(configPanel, 62, "ON TIME", &onValueLabel, 0);
  createConfigurationRow(configPanel, 162, "OFF TIME", &offValueLabel, 2);
  createConfigurationRow(configPanel, 262, "CYCLES", &cyclesValueLabel, 4);

  lv_obj_t* controlPanel = createPanel(screen, 24, 486, 976, 90);
  startButton = createButton(controlPanel, 28, 15, kStartButtonWidth, 60,
                             "HOLD TO START",
                             0x16865C, onStartButton, nullptr,
                             LV_EVENT_LONG_PRESSED);
  startHoldFill = lv_obj_create(startButton);
  lv_obj_set_pos(startHoldFill, 0, 0);
  lv_obj_set_size(startHoldFill, 1, 60);
  lv_obj_clear_flag(startHoldFill,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(startHoldFill, lv_color_hex(0x39C98A),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(startHoldFill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(startHoldFill, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(startHoldFill, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(startHoldFill, 0, LV_PART_MAIN);
  lv_obj_add_flag(startHoldFill, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_background(startHoldFill);
  lv_obj_add_event_cb(startButton, onStartHoldEvent, LV_EVENT_PRESSED,
                      nullptr);
  lv_obj_add_event_cb(startButton, onStartHoldEvent, LV_EVENT_RELEASED,
                      nullptr);
  lv_obj_add_event_cb(startButton, onStartHoldEvent, LV_EVENT_PRESS_LOST,
                      nullptr);
  pauseButton = createButton(controlPanel, 345, 15, 286, 60, "PAUSE",
                             0xB7791F, onPauseButton);
  pauseButtonLabel = lv_obj_get_child(pauseButton, 0);
  stopButton = createButton(controlPanel, 662, 15, 286, 60, "STOP",
                            0xC33C42, onStopButton, nullptr, LV_EVENT_PRESSED);

  refreshConfigurationLabels();
}

bool initializeDisplay() {
  Serial.println(F("DISPLAY init JD9165 1024x600"));
  lcd.begin();
  touch.begin();
  lv_init();

  // DMA2D is deliberately disabled in the vendor DPI configuration. A small
  // partial buffer keeps esp_lcd_panel_draw_bitmap synchronous, so LVGL never
  // recycles a buffer while a background copy is still using it.
  constexpr size_t pixelCount =
      static_cast<size_t>(LCD_H_RES) * kDisplayBufferLines;
  constexpr size_t bufferBytes = pixelCount * sizeof(lv_color_t);
  drawBufferA = static_cast<lv_color_t*>(
      heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  drawBufferB = static_cast<lv_color_t*>(
      heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (drawBufferA == nullptr || drawBufferB == nullptr) {
    driveRelay(false);
    Serial.println(F("ERROR display buffer allocation failed; relay OFF"));
    if (drawBufferA != nullptr) {
      heap_caps_free(drawBufferA);
      drawBufferA = nullptr;
    }
    if (drawBufferB != nullptr) {
      heap_caps_free(drawBufferB);
      drawBufferB = nullptr;
    }
    return false;
  }

  lv_disp_draw_buf_init(&drawBuffer, drawBufferA, drawBufferB, pixelCount);

  static lv_disp_drv_t displayDriver;
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = LCD_H_RES;
  displayDriver.ver_res = LCD_V_RES;
  displayDriver.flush_cb = displayFlush;
  displayDriver.draw_buf = &drawBuffer;
  displayDriver.full_refresh = false;
  lv_disp_drv_register(&displayDriver);

  static lv_indev_drv_t inputDriver;
  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = touchRead;
  inputDriver.long_press_time = kStartHoldMs;
  inputDriver.long_press_repeat_time = 200;
  lv_indev_drv_register(&inputDriver);

  createUi();
  displayReady = true;
  updateUi(millis(), true);
  Serial.print(F("DISPLAY ready psram_free="));
  Serial.println(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return true;
}

}  // namespace

void setup() {
  // Preload the safe output level before enabling the output driver.
  digitalWrite(kRelayPin, relayLevel(false));
  pinMode(kRelayPin, OUTPUT);
  digitalWrite(kRelayPin, relayLevel(false));

  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println(F("Power interval relay controller"));
  Serial.println(F("READY relay=OFF pin=GPIO3 trigger=ACTIVE_HIGH"));
  printStatus(millis());
  printHelp();
  initializeDisplay();
  enableLoopWDT();
  Serial.println(F("SAFETY loop watchdog enabled"));
}

void loop() {
  // Relay deadlines take priority over serial and graphics work.
  updateController(millis());
  readSerialCommands();
  updateController(millis());

  if (displayReady) {
    lv_timer_handler();
    updateController(millis());
    const uint32_t uiNowMs = millis();
    updateStartHoldGauge(uiNowMs);
    updateUi(uiNowMs, false);
  }

  delay(1);
}
