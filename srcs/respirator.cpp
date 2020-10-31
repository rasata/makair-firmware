/******************************************************************************
 * @author Makers For Life
 * @copyright Copyright (c) 2020 Makers For Life
 * @file respirator.cpp
 * @brief Entry point of ventilator program
 *****************************************************************************/

#pragma once

#include "../includes/config.h"
#if MODE == MODE_PROD

// INCLUDES ==================================================================

// External
#include "Arduino.h"
#include <HardwareSerial.h>
#include <IWatchdog.h>
#include <LiquidCrystal.h>

// Internal
#include "../includes/battery.h"
#include "../includes/blower.h"
#include "../includes/buzzer.h"
#include "../includes/buzzer_control.h"
#include "../includes/debug.h"
#include "../includes/end_of_line_test.h"
#include "../includes/keyboard.h"
#include "../includes/main_state_machine.h"
#include "../includes/mass_flow_meter.h"
#include "../includes/parameters.h"
#include "../includes/pressure.h"
#include "../includes/pressure_controller.h"
#include "../includes/pressure_valve.h"
#include "../includes/screen.h"
#include "../includes/serial_control.h"
#include "../includes/telemetry.h"

// PROGRAM =====================================================================

PressureValve inspiratoryValve;
PressureValve expiratoryValve;
HardwareTimer* hardwareTimer1;
HardwareTimer* hardwareTimer3;
Blower* blower_pointer;
Blower blower;

int16_t pressureOffset;
int32_t pressureOffsetSum;
uint32_t pressureOffsetCount;
int16_t minOffsetValue = 0;
int16_t maxOffsetValue = 0;

PressureController pController;
AlarmController alarmController;

HardwareSerial Serial6(PIN_TELEMETRY_SERIAL_RX, PIN_TELEMETRY_SERIAL_TX);

/**
 * Block execution for a given duration
 *
 * @param ms  Duration of the blocking in millisecond
 */
void waitForInMs(uint16_t ms) {
    uint16_t start = millis();
    minOffsetValue = readPressureSensor(0, 0);
    maxOffsetValue = readPressureSensor(0, 0);
    pressureOffsetSum = 0;
    pressureOffsetCount = 0;

    while ((millis() - start) < ms) {
        // Measure 1 pressure per ms we wait
        if ((millis() - start) > pressureOffsetCount) {
            int16_t pressureValue = readPressureSensor(0, 0);
            pressureOffsetSum += pressureValue;
            minOffsetValue = min(pressureValue, minOffsetValue);
            maxOffsetValue = max(pressureValue, maxOffsetValue);
            pressureOffsetCount++;
        }
        continue;
    }
}

uint32_t lastpControllerComputeDate;

void setup(void) {
    DBG_DO(Serial.begin(115200);)
    DBG_DO(Serial.println("Booting the system...");)

    startScreen();

    initBattery();
    if (isBatteryDeepDischarged()) {
        screen.clear();
        screen.setCursor(0, 0);
        screen.print("Battery very low");
        screen.setCursor(0, 2);
        screen.print("Please charge");
        screen.setCursor(0, 3);
        screen.print("before running.");
        while (true) {
        }
    }

    initTelemetry();
    sendBootMessage();

    pinMode(PIN_PRESSURE_SENSOR, INPUT);
    pinMode(PIN_BATTERY, INPUT);

    // Timer for servos
    hardwareTimer3 = new HardwareTimer(TIM3);
    hardwareTimer3->setOverflow(SERVO_VALVE_PERIOD, MICROSEC_FORMAT);

    // Servo blower setup
    inspiratoryValve = PressureValve(hardwareTimer3, TIM_CHANNEL_SERVO_VALVE_BLOWER,
                                     PIN_SERVO_BLOWER, VALVE_OPEN_STATE, VALVE_CLOSED_STATE);
    inspiratoryValve.setup();
    hardwareTimer3->resume();

    // Servo patient setup
    expiratoryValve = PressureValve(hardwareTimer3, TIM_CHANNEL_SERVO_VALVE_PATIENT,
                                    PIN_SERVO_PATIENT, VALVE_OPEN_STATE, VALVE_CLOSED_STATE);
    expiratoryValve.setup();
    hardwareTimer3->resume();

    hardwareTimer1 = new HardwareTimer(TIM1);
    hardwareTimer1->setOverflow(ESC_PPM_PERIOD, MICROSEC_FORMAT);
    blower = Blower(hardwareTimer1, TIM_CHANNEL_ESC_BLOWER, PIN_ESC_BLOWER);
    blower.setup();
    blower_pointer = &blower;

    // Turn on the raspberry power
    pinMode(PIN_ENABLE_PWR_RASP, OUTPUT);
    digitalWrite(PIN_ENABLE_PWR_RASP, PWR_RASP_ACTIVE);

    // Activate test mode if a service button is pressed. The end of line test mode cannot be
    // activated later on.
    // Autotest inputs: the service button on PB12, top right of the board's rear side
    pinMode(PB12, INPUT);
    if (HIGH == digitalRead(PB12)) {
        eolTest.activate();
        screen.clear();
        screen.print("EOL Test Mode");
        while (HIGH == digitalRead(PB12)) {
            continue;
        }
    }

    // Open both valves at startup
    inspiratoryValve.open();
    inspiratoryValve.execute();
    expiratoryValve.open();
    expiratoryValve.execute();

    // Catch potential Watchdog reset
    // cppcheck-suppress misra-c2012-14.4 ; unknown external signature
    if (IWatchdog.isReset(true)) {
        // Run a high priority alarm
        BuzzerControl_Init();
        Buzzer_Init();
        Buzzer_High_Prio_Start();

        // Print message on the screen
        screen.clear();
        screen.setCursor(0, 0);
        screen.print("An error has occured");
        screen.setCursor(0, 2);
        screen.print("Check the machine");
        screen.setCursor(0, 3);
        screen.print("before re-using");

        // Wait infinitely
        while (1) {
        }
    }

    // Do not initialize pressure controller and keyboard in test mode
    if (!eolTest.isRunning()) {
        AlarmController alarmController = AlarmController();
        DBG_DO(Serial.print("adress of pController in respirator.cpp 180:");)
    DBG_DO(Serial.println((unsigned int)&pController);)
        pController =
            PressureController(inspiratoryValve, expiratoryValve, &alarmController, blower_pointer);

        initKeyboard();
    }

    // Prepare LEDs
    pinMode(PIN_LED_START, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);

    BuzzerControl_Init();
    Buzzer_Init();

    // escBlower needs 5s at speed 0 to be properly initalized

    // RCM-SW-17 (Christmas tree at startup)
    Buzzer_Boot_Start();
    digitalWrite(PIN_LED_START, LED_START_ACTIVE);
    digitalWrite(PIN_LED_GREEN, LED_GREEN_ACTIVE);
    digitalWrite(PIN_LED_RED, LED_RED_ACTIVE);
    digitalWrite(PIN_LED_YELLOW, LED_YELLOW_ACTIVE);
    waitForInMs(1000);
    digitalWrite(PIN_LED_START, LED_START_INACTIVE);
    digitalWrite(PIN_LED_GREEN, LED_GREEN_INACTIVE);
    digitalWrite(PIN_LED_RED, LED_RED_INACTIVE);
    digitalWrite(PIN_LED_YELLOW, LED_YELLOW_INACTIVE);
    waitForInMs(3000);

    screen.setCursor(0, 0);
    screen.print("Calibrating P offset");
    screen.setCursor(0, 2);
    screen.print("Patient must be");
    screen.setCursor(0, 3);
    screen.print("unplugged");
    waitForInMs(3000);

// Mass Flow Meter, if any
#ifdef MASS_FLOW_METER
    (void)MFM_init();
    MFM_calibrateZero();  // Patient unplugged, also set the zero of mass flow meter
#endif

    resetScreen();
    if (pressureOffsetCount != 0u) {
        pressureOffset = pressureOffsetSum / static_cast<int32_t>(pressureOffsetCount);
    } else {
        pressureOffset = 0;
    }
    DBG_DO({
        Serial.print("pressure offset = ");
        Serial.print(pressureOffsetSum);
        Serial.print(" / ");
        Serial.print(pressureOffsetCount);
        Serial.print(" = ");
        Serial.print(pressureOffset);
        Serial.println();
    })

    // Happens when patient is plugged at starting
    if ((maxOffsetValue - minOffsetValue) >= 10) {
        resetScreen();
        screen.setCursor(0, 0);
        char line1[SCREEN_LINE_LENGTH + 1];
        (void)snprintf(line1, SCREEN_LINE_LENGTH + 1, "P offset is unstable");
        screen.print(line1);
        screen.setCursor(0, 1);
        char line2[SCREEN_LINE_LENGTH + 1];
        (void)snprintf(line2, SCREEN_LINE_LENGTH + 1, "Max-Min: %3d mmH2O",
                       maxOffsetValue - minOffsetValue);
        screen.print(line2);
        screen.setCursor(0, 2);
        screen.print("Unplug patient and");
        screen.setCursor(0, 3);
        screen.print("reboot");
        Buzzer_High_Prio_Start();
        while (true) {
        }
    }

    if (pressureOffset >= MAX_PRESSURE_OFFSET) {
        resetScreen();
        screen.setCursor(0, 0);
        char line1[SCREEN_LINE_LENGTH + 1];
        (void)snprintf(line1, SCREEN_LINE_LENGTH + 1, "P offset: %3d mmH2O", pressureOffset);
        screen.print(line1);
        screen.setCursor(0, 1);
        char line2[SCREEN_LINE_LENGTH + 1];
        (void)snprintf(line2, SCREEN_LINE_LENGTH + 1, "P offset is > %-3d", MAX_PRESSURE_OFFSET);
        screen.print(line2);
        screen.setCursor(0, 2);
        screen.print("Unplug patient and");
        screen.setCursor(0, 3);
        screen.print("reboot");
        Buzzer_High_Prio_Start();
        while (true) {
        }
    }

    screen.setCursor(0, 3);
    char message[SCREEN_LINE_LENGTH + 1];
    (void)snprintf(message, SCREEN_LINE_LENGTH + 1, "P offset: %3d mmH2O", pressureOffset);
    screen.print(message);
    waitForInMs(1000);

    lastpControllerComputeDate = micros();

    // No watchdog in end of line test mode
    if (!eolTest.isRunning()) {
        DBG_DO(Serial.println("beforeMsms");)
        // Init the watchdog timer. It must be reloaded frequently otherwise MCU resests
        mainStateMachine.activate();
        mainStateMachine.setupAndStart(&alarmController, &pController);
        DBG_DO(Serial.println("beforeMsms");)
        // TODO enable again
        // IWatchdog.begin(WATCHDOG_TIMEOUT);
        // IWatchdog.reload();
    } else {
        eolTest.setupAndStart();
    }
}

// cppcheck-suppress unusedFunction
void loop(void) {}

#endif
