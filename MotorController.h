/*
 * MotorController.h
  *
 * Parent class for all motor controllers.
 *
 Copyright (c) 2013 Collin Kidder, Michael Neuweiler, Charles Galpin

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#ifndef MOTORCTRL_H_
#define MOTORCTRL_H_

#include <Arduino.h>
#include "config.h"
#include "Device.h"
#include "Throttle.h"
#include "CanHandler.h"
#include "DeviceManager.h"
#include "FaultHandler.h"
#include "PID_v1.h"


#define MOTORCTL_INPUT_DRIVE_EN    3
#define MOTORCTL_INPUT_FORWARD     4
#define MOTORCTL_INPUT_REVERSE     5
#define MOTORCTL_INPUT_LIMP        6

class CanHandler;

enum PowerMode {
    modeTorque = 0,
    modeSpeed = 1
};

class MotorControllerConfiguration : public DeviceConfiguration
{
public:
    bool invertDirection; // should an AC motor run in reverse mode? (e.g. negative speed for forward motion)
    uint16_t speedMax; // in rpm
    uint16_t torqueMax; // maximum torque in 0.1 Nm
    uint16_t slewRateMotor; // slew rate of torque/speed value for motoring in 0.1 percent/sec
    uint16_t slewRateRegen; // slew rate of torque/speed value for regen in 0.1 percent/sec
    uint16_t maxMechanicalPowerMotor; // maximal mechanical power of motor in 100W steps
    uint16_t maxMechanicalPowerRegen; // maximal mechanical power of regen in 100W steps
    uint8_t reversePercent;
    uint16_t nominalVolt; //nominal pack voltage in tenths of a volt
    PowerMode powerMode;
    uint8_t creepLevel; // percentage of torque used for creep function (imitate creep of automatic transmission, set 0 to disable)
    uint16_t creepSpeed; // max speed for creep
    uint8_t brakeHold; // percentage of max torque to achieve brake hold (0 = off)
    uint8_t brakeHoldForceCoefficient; // quotient by which the negative rpm is divided to get the force increase/decrease during brake hold (must NOT be 0!)
    double cruiseKp, cruiseKi, cruiseKd; // PID parameter for cruise control
	uint16_t cruiseLongPressDelta; // delta in rpm/kph to increase/decrease target speed for cc during long button press
	uint16_t cruiseStepDelta; // delta in rpm/kph to increase/decrease target speed for cc at short button press
	bool cruiseUseRpm; // true = use rpm to control cruise control, false = use vehicle speed instead
	uint16_t speedSet[CFG_CRUISE_SIZE_SPEED_SET]; // speed sets for dashboard buttons
};

class MotorController: public Device, public CanObserver
{
public:
    enum Gears {
        GEAR_NEUTRAL = 0,
        GEAR_DRIVE = 1,
        GEAR_REVERSE = 2,
        GEAR_ERROR = 3
    };

    enum CruiseControlButton {
    	NONE, TOGGLE, RECALL, PLUS, MINUS, DISENGAGE
    };

    MotorController();
    DeviceType getType();
    void setup();
    void tearDown();
    void handleTick();
    void handleCanFrame(CAN_FRAME *);
    void handleStateChange(Status::SystemState, Status::SystemState);
    void cruiseControlToggle();
    void cruiseControlDisengage();
    void cruiseControlAdjust(int16_t);
    void cruiseControlSetSpeed(int16_t);
    void handleCruiseControlButton(CruiseControlButton);
    bool isCruiseControlEnabled();
    int16_t getCruiseControlSpeed();

    void loadConfiguration();
    void saveConfiguration();

    int16_t getThrottleLevel();
    Gears getGear();
    int16_t getSpeedRequested();
    int16_t getSpeedActual();
    int16_t getTorqueRequested();
    int16_t getTorqueActual();
    int16_t getTorqueAvailable();

    uint16_t getDcVoltage();
    int16_t getDcCurrent();
    uint16_t getAcCurrent();
    int16_t getMechanicalPower();
    int16_t getTemperatureMotor();
    int16_t getTemperatureController();
    int16_t getNominalVolt();

protected:
    int16_t speedActual; // in rpm
    int16_t torqueActual; // in 0.1 Nm
    int16_t torqueAvailable; // the maximum available torque in 0.1Nm

    uint16_t dcVoltage; // DC voltage in 0.1 Volts
    int16_t dcCurrent; // DC current in 0.1 Amps
    uint16_t acCurrent; // AC current in 0.1 Amps
    int16_t temperatureMotor; // temperature of motor in 0.1 degree C
    int16_t temperatureController; // temperature of controller in 0.1 degree C

    bool rolling; // flag wether to save power consumption to eeprom
    void reportActivity();

private:
    int16_t throttleLevel; // -1000 to 1000 (per mille of throttle level)
    int16_t torqueRequested; // in 0.1 Nm, calculated in MotorController - must not be manipulated by subclasses
    int16_t speedRequested; // in rpm, calculated in MotorController - must not be manipulated by subclasses
    uint8_t ticksNoMessage; // counter how many ticks the device went through without any message from the controller
    uint32_t slewTimestamp; // time stamp of last slew rate calculation
    int16_t minimumBatteryTemperature; // battery temperature in 0.1 deg Celsius below which no regen will not occur
    bool brakeHoldActive; // flag to signal if brake hold was activated by a standing car and pressed brake
    uint32_t brakeHoldStart; // timestamp at which the brake hold was activated
    int16_t brakeHoldLevel; // current throttle level applied by brake hold (must be signed to prevent overflow!!)
    uint32_t gearChangeTimestamp;
    Gears gear;
    double cruiseSpeedTarget, cruiseSpeedActual, cruiseThrottle; // PID parameters for cruise control, cruiseThrottle is 0-2000 (+1000 offset to throttle because PID can't handle negative values.
    double cruiseSpeedLast; // the last active cruise control speed (for recall)
    PID *cruisePid; // PID controller for cruise control
    int16_t cruiseSpeedBuffer[CFG_CRUISE_SPEED_BUFFER_SIZE]; // buffer to average actual speed measurements and reduce jumps
    uint8_t cruiseSpeedBufferPtr; // pointer to current speed buffer position
    uint32_t cruiseSpeedSum; // temp variable to sum up buffered speed
    bool cruiseControlEnabled; // main switch if cruise control is enabled at all (power switch)
    CruiseControlButton cruiseLastButton; // which button was last pressed
    uint32_t cruiseButtonPressed; // timestamp when a cruise control button was pressed

    void updateStatusIndicator();
    void checkActivity();
    void processThrottleLevel();
    void updateGear();
    int16_t processBrakeHold(MotorControllerConfiguration *config, int16_t throttleLevel, int16_t brakeLevel);
    void processGearChange();
    bool checkBatteryTemperatureForRegen();
};

#endif
