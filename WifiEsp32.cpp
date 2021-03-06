/*
 * Esp32Wifi.cpp
 *
 * Class to interface with the Esp32 based wifi adapter
 *
 Copyright (c) 2020 Collin Kidder, Michael Neuweiler, Charles Galpin

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

#include "WifiEsp32.h"

WifiEsp32::WifiEsp32() : Wifi()
{
    prefsHandler = new PrefHandler(ESP32WIFI);

    serialInterface->begin(115200);

    commonName = "WIFI (ESP32)";

    didParamLoad = false;
    connected = false;
    inPos = outPos = 0;
    timeStarted = 0;
    timeHeartBeat = 0;
    dataPointCount = 0;
    psWritePtr = psReadPtr = 0;
    updateCount = 0;
    heartBeatEnabled = true;

    pinMode(CFG_WIFI_ENABLE, OUTPUT);
    digitalWrite(CFG_WIFI_ENABLE, LOW);
}

/**
 * \brief Initialization of hardware and parameters
 *
 */
void WifiEsp32::setup()
{
    digitalWrite(CFG_WIFI_ENABLE, HIGH);

    didParamLoad = false;
    connected = false;
    inPos = outPos = 0;
    timeStarted = timeHeartBeat = millis();
    dataPointCount = 0;
    psWritePtr = psReadPtr = 0;

    // don't try to re-attach if called from reset() - to avoid warning message
    if (!tickHandler.isAttached(this, CFG_TICK_INTERVAL_WIFI)) {
        tickHandler.attach(this, CFG_TICK_INTERVAL_WIFI);
    }

    ready = true;
    running = true;
}

/**
 * \brief Tear down the device in a safe way.
 *
 */
void WifiEsp32::tearDown()
{
    Device::tearDown();
    digitalWrite(CFG_WIFI_ENABLE, LOW);
}

/**
 * \brief Send a command to ichip
 *
 * A version which defaults to IDLE which is what most of the code used to assume.
 *
 * \param cmd the command string to be sent
 */
void WifiEsp32::sendCmd(String cmd)
{
    if (logger.isDebug()) {
        logger.debug(this, "buffer: %s\n", cmd.c_str());
    }
    sendBuffer[psWritePtr++] = cmd;
    if (psWritePtr >= CFG_SERIAL_SEND_BUFFER_SIZE) {
        psWritePtr = 0;
    }
}

/**
 * \brief Try to send a buffered command to ESP32
 */
void WifiEsp32::sendBufferedCommand()
{
    if (psReadPtr != psWritePtr) {
        serialInterface->print(sendBuffer[psReadPtr++]);
        serialInterface->write(13);
        if (psReadPtr >= CFG_SERIAL_SEND_BUFFER_SIZE) {
            psReadPtr = 0;
        }
    }
}

/**
 * \brief Perform regular tasks triggered by a timer
 *
 * Query for changed parameters of the config page, socket status
 * and query for incoming data on sockets
 *
 */
void WifiEsp32::handleTick()
{
    if (connected) {
        sendSocketUpdate();
    }

    if (!didParamLoad && millis() > 3000 + timeStarted) {
        loadParameters();
        didParamLoad = true;
    }

    if (heartBeatEnabled && timeHeartBeat + 10000 < millis()) {
        logger.error(this, "No heartbeat received from ESP32, resetting.");
        reset();
    }

    if (!ready && !running && (timeHeartBeat + 1000 < millis())) {
        logger.info("Re-initializing ESP32 after reset.");
        setup(); // re-init after reset
    }
}

/**
 * \brief Handle a message sent by the DeviceManager
 *
 * \param messageType the type of the message
 * \param message the message to be processed
 */
void WifiEsp32::handleMessage(uint32_t messageType, void* message)
{
    Device::handleMessage(messageType, message);

    switch (messageType) {
    case MSG_SET_PARAM: {
        char **params = (char **) message;
        setParam((char *) params[0], (char *) params[1]);
        break;
    }

    case MSG_CONFIG_CHANGE:
        loadParameters();
        break;

    case MSG_COMMAND:
        sendCmd((char *) message);
        break;

    case MSG_LOG:
        String *params = (String *) message;
        sendLogMessage(params[0], params[1], params[2]);
        break;
    }
}

/**
 * \brief Act on system state changes and send an update to the socket client
 *
 * \param oldState the previous system state
 * \param newState the new system state
 */
void WifiEsp32::handleStateChange(Status::SystemState oldState, Status::SystemState newState)
{
    Device::handleStateChange(oldState, newState);
    sendSocketUpdate();
}

/**
 * \brief Process serial input waiting from the wifi module.
 * or send next buffered command
 *
 * The method is called by the main loop
 */
void WifiEsp32::process()
{
	sendBufferedCommand();

    int ch;
    while (serialInterface->available()) {
        ch = serialInterface->read();
//SerialUSB.print((char)ch);
        if (ch == -1) { //and there is no reason it should be -1
            return;
        }

        if (ch == 13 || inPos > (CFG_WIFI_BUFFER_SIZE - 2)) {
            inBuffer[inPos] = 0;
            inPos = 0;

            if (logger.isDebug()) {
                logger.debug(this, "incoming: '%s'", inBuffer);
            }
            String input = inBuffer;
            if (input.startsWith("cfg:")) {
                processParameterChange(input.substring(4));
            } else if (input.startsWith("cmd:")) {
                processIncomingSocketCommand(input.substring(4));
            } else if (input.startsWith("hb:")) {
                timeHeartBeat = millis();
                if (input.indexOf("stop") != -1) {
                    heartBeatEnabled = false;
                } else if (input.indexOf("start") != -1) {
                    heartBeatEnabled = true;
                }
            }

            return; // before processing the next line, return to the loop() to allow other devices to process.
        } else if (ch != 10) { // don't add a LF character
            inBuffer[inPos++] = (char) ch;
        }
    }
}

/**
 * \brief Process incoming data from a socket
 */
void WifiEsp32::processIncomingSocketCommand(String input)
{
    logger.debug(this, "processing incoming socket command");

    int pos = input.indexOf('=');
    if (pos > 0) {
        String key = input.substring(0, pos);
        String value = input.substring(pos + 1);

        if (key.equals("cruise")) {
            int num = value.toInt();
            if (value.charAt(0) == '-' || value.charAt(0) == '+') {
                deviceManager.getMotorController()->cruiseControlAdjust(num);
            } else {
                deviceManager.getMotorController()->cruiseControlSetSpeed(num);
            }
        } else if (key.equals("regen")) {
            status.enableRegen = value.equals("true");
            logger.debug("Regen is now switched %s", (status.enableRegen ? "on" : "off"));
        } else if (key.equals("creep")) {
            status.enableCreep = value.equals("true");;
            logger.debug("Creep is now switched %s", (status.enableCreep ? "on" : "off"));
        } else if (key.equals("ehps")) {
            systemIO.setPowerSteering(value.equals("true"));
            logger.debug("EHPS is now switched %s", (status.powerSteering ? "on" : "off"));
        } else if (key.equals("heater")) {
            bool flag = value.equals("true");
            systemIO.setEnableHeater(flag);
            systemIO.setHeaterPump(flag);
            logger.debug("Heater is now switched %s", (status.enableHeater ? "on" : "off"));
        } else if (key.equals("chargeInput")) {
            logger.debug("Setting charge level to %d Amps", value.toInt());
            deviceManager.getCharger()->overrideMaximumInputCurrent(value.toDouble() * 10);
        }
    } else {
        if (input.equals("stopCharge")) {
            status.setSystemState(Status::charged);
        } else if (input.equals("cruiseToggle")) {
            deviceManager.getMotorController()->cruiseControlToggle();
        } else if (input.equals("connected")) {
            logger.debug("Client connected, clearing value cache");
            valueCache.clear(); // new connection -> clear the cache to have all values sent
            connected = true;
        } else if (input.equals("disconnected")) {
            logger.debug("Client disconnected");
            connected = false;
        } else if (input.equals("loadConfig")) {
            didParamLoad = false;
        } else if (input.equals("getLog")) {
            logger.printHistory(*serialInterface);
        }
    }
}

/**
 * \brief Set a parameter to the given string value
 *
 * \param paramName the name of the parameter
 * \param value the value to set
 */
void WifiEsp32::setParam(String paramName, String value)
{
    if (logger.isDebug()) {
        logger.debug(this, "setParam: cfg:%s=%s", paramName.c_str(), value.c_str());
    }
    sendCmd("cfg:" + paramName + "=" + value);
}

/**
 * \brief send log message as JSON
 *
 * \param logLevel the level of the log entry
 * \param deviceName name of the device which created the log entry
 * \param message the log message
 * \return the prepared log message which can be sent to the socket
 *
 */
void WifiEsp32::sendLogMessage(String logLevel, String deviceName, String message)
{
    String data = "json:{\"logMessage\": {\"level\": \"";
    data.concat(logLevel);
    data.concat("\",\"message\": \"");
    if (deviceName.length() > 0) {
        data.concat(deviceName);
        data.concat(": ");
    }
    data.concat(message);
    data.concat("\"}}");

    sendCmd(data);
}

/**
 * \brief Send update to all active sockets
 *
 * The message to be sent is created by the assigned SocketProcessor
 *
 */
void WifiEsp32::sendSocketUpdate()
{
    outPos = 0;
    dataPointCount = 0;

    prepareSystemData();
    prepareMotorControllerData();
    prepareBatteryManagerData();
    if (updateCount == 0) {
        prepareDcDcConverterData();
    }
    if (status.getSystemState() == Status::charging || status.getSystemState() == Status::charged) {
        prepareChargerData();
    }

    if (outPos > 0) {
        String header = "data:";
        header.concat(dataPointCount);
        serialInterface->print(header); // indicate that we will send a binary data stream of x bytes
        serialInterface->write(13); // CR
        serialInterface->write(outBuffer, outPos); // send the binary data
    }
    if (++updateCount > 5)
        updateCount = 0;
}

void WifiEsp32::prepareMotorControllerData() {
    MotorController* motorController = deviceManager.getMotorController();
    BatteryManager* batteryManager = deviceManager.getBatteryManager();

    if (motorController) {
        processValue(&valueCache.bitfieldMotor, status.getBitFieldMotor(), bitfieldMotor);

        processValue(&valueCache.torqueActual, motorController->getTorqueActual(), torqueActual);
        processValue(&valueCache.torqueAvailable, motorController->getTorqueAvailable(), torqueAvailable);
        processValue(&valueCache.speedActual, motorController->getSpeedActual(), speedActual);
        processValue(&valueCache.throttle, motorController->getThrottleLevel(), throttle);
        if (!batteryManager || !batteryManager->hasPackVoltage()) {
            processValue(&valueCache.dcVoltage, motorController->getDcVoltage(), dcVoltage);
            processLimits(&valueCache.dcVoltageMin, motorController->getDcVoltage(), dcVoltageMin, false);
            processLimits(&valueCache.dcVoltageMax, motorController->getDcVoltage(), dcVoltageMax, true);
        }
        if (!batteryManager || !batteryManager->hasPackCurrent()) {
            processValue(&valueCache.dcCurrent, motorController->getDcCurrent(), dcCurrent);
            processLimits(&valueCache.dcCurrentMin, motorController->getDcCurrent(), dcCurrentMin, false);
            processLimits(&valueCache.dcCurrentMax, motorController->getDcCurrent(), dcCurrentMax, true);
        }
        processValue(&valueCache.temperatureMotor, motorController->getTemperatureMotor(), temperatureMotor);
        processLimits(&valueCache.temperatureMotorMax, motorController->getTemperatureMotor(), tempMotorMax, true);
        processValue(&valueCache.temperatureController, motorController->getTemperatureController(), temperatureController);
        processLimits(&valueCache.temperatureControllerMax, motorController->getTemperatureController(), tempControllerMax, true);
        processValue(&valueCache.cruiseControlSpeed, motorController->getCruiseControlSpeed(), cruiseControlSpeed);
        processValue(&valueCache.enableCruiseControl, motorController->isCruiseControlEnabled(), enableCruiseControl);
    }
}

void WifiEsp32::prepareDcDcConverterData() {
    DcDcConverter* dcDcConverter = deviceManager.getDcDcConverter();

    if (dcDcConverter) {
        processValue(&valueCache.dcDcHvVoltage, dcDcConverter->getHvVoltage(), dcDcHvVoltage);
        processValue(&valueCache.dcDcHvCurrent, dcDcConverter->getHvCurrent(), dcDcHvCurrent);
        processValue(&valueCache.dcDcLvVoltage, dcDcConverter->getLvVoltage(), dcDcLvVoltage);
        processValue(&valueCache.dcDcLvCurrent, dcDcConverter->getLvCurrent(), dcDcLvCurrent);
        processValue(&valueCache.dcDcTemperature, dcDcConverter->getTemperature(), dcDcTemperature);
    }
}

void WifiEsp32::prepareChargerData() {
    Charger* charger = deviceManager.getCharger();
    BatteryManager* batteryManager = deviceManager.getBatteryManager();

    if (charger) {
        processValue(&valueCache.chargerInputVoltage, charger->getInputVoltage(), chargerInputVoltage);
        processValue(&valueCache.chargerInputCurrent, charger->getInputCurrent(), chargerInputCurrent);
        processValue(&valueCache.chargerBatteryVoltage, charger->getBatteryVoltage(), chargerBatteryVoltage);
        processValue(&valueCache.chargerBatteryCurrent, charger->getBatteryCurrent(), chargerBatteryCurrent);
        processValue(&valueCache.chargerTemperature, charger->getTemperature(), chargerTemperature);
        processValue(&valueCache.maximumInputCurrent, charger->calculateMaximumInputCurrent(), maximumInputCurrent);

        uint16_t minutesRemaining = charger->calculateTimeRemaining();
        processValue(&valueCache.chargeHoursRemain, (uint8_t)(minutesRemaining / 60), chargeHoursRemain);
        processValue(&valueCache.chargeMinsRemain, (uint8_t)(minutesRemaining % 60), chargeMinsRemain);
        if (batteryManager && batteryManager->hasSoc())
            processValue(&valueCache.chargeLevel, batteryManager->getSoc() * 50, chargeLevel);
    }
}

void WifiEsp32::prepareSystemData() {
    processValue(&valueCache.systemState, (uint8_t) status.getSystemState(), systemState);
    processValue(&valueCache.bitfieldIO, status.getBitFieldIO(), bitfieldIO);

    processValue(&valueCache.flowCoolant, status.flowCoolant * 6, flowCoolant);
    processValue(&valueCache.flowHeater, status.flowHeater * 6, flowHeater);
    processValue(&valueCache.heaterPower, status.heaterPower, heaterPower);
    for (int i = 0; i < CFG_NUMBER_BATTERY_TEMPERATURE_SENSORS; i++) {
        processValue(&valueCache.temperatureBattery[i], status.temperatureBattery[i], (DataPointCode)(temperatureBattery1 + i));
    }
    processValue(&valueCache.temperatureCoolant, status.temperatureCoolant, temperatureCoolant);
    processValue(&valueCache.temperatureHeater, status.heaterTemperature, temperatureHeater);
    if (status.temperatureExterior != CFG_NO_TEMPERATURE_DATA) {
        processValue(&valueCache.temperatureExterior, status.temperatureExterior, temperatureExterior);
    }

    processValue(&valueCache.powerSteering, status.powerSteering, powerSteering);
    processValue(&valueCache.enableRegen, status.enableRegen, enableRegen);
    processValue(&valueCache.enableHeater, status.enableHeater, enableHeater);
    processValue(&valueCache.enableCreep, status.enableCreep, enableCreep);
}

void WifiEsp32::prepareBatteryManagerData() {
    BatteryManager* batteryManager = deviceManager.getBatteryManager();

    if (batteryManager) {
        processValue(&valueCache.bitfieldBms, status.getBitFieldBms(), bitfieldBms);

        if (batteryManager->hasSoc())
            processValue(&valueCache.soc, (uint16_t)(batteryManager->getSoc() * 50), soc);
        if (batteryManager->hasPackVoltage()) {
            processValue(&valueCache.dcVoltage, batteryManager->getPackVoltage(), dcVoltage);
            processLimits(&valueCache.dcVoltageMin, batteryManager->getPackVoltage(), dcVoltageMin, false);
            processLimits(&valueCache.dcVoltageMax, batteryManager->getPackVoltage(), dcVoltageMax, true);
        }
        if (batteryManager->hasPackCurrent())
            processValue(&valueCache.dcCurrent, batteryManager->getPackCurrent(), dcCurrent);
        if (batteryManager->hasDischargeLimit()) {
            processValue(&valueCache.dischargeLimit, batteryManager->getDischargeLimit(), dischargeLimit);
            processValue(&valueCache.dcCurrentMax, batteryManager->getDischargeLimit() * 10, dcCurrentMax);
        } else {
            processValue(&valueCache.dischargeAllowed, batteryManager->isDischargeAllowed(), dischargeAllowed);
        }
        if (batteryManager->hasChargeLimit()) {
            processValue(&valueCache.chargeLimit, batteryManager->getChargeLimit(), chargeLimit);
            processValue(&valueCache.dcCurrentMin, batteryManager->getChargeLimit() * -10, dcCurrentMin);
        } else {
            processValue(&valueCache.chargeAllowed, batteryManager->isChargeAllowed(), chargeAllowed);
        }
        if (batteryManager->hasCellTemperatures()) {
            processValue(&valueCache.lowestCellTemp, batteryManager->getLowestCellTemp(), lowestCellTemp);
            processValue(&valueCache.highestCellTemp, batteryManager->getHighestCellTemp(), highestCellTemp);
            processValue(&valueCache.lowestCellTempId, batteryManager->getLowestCellTempId(), lowestCellTempId);
            processValue(&valueCache.highestCellTempId, batteryManager->getHighestCellTempId(), highestCellTempId);
        }
        if (batteryManager->hasCellVoltages()) {
            processValue(&valueCache.lowestCellVolts, batteryManager->getLowestCellVolts(), lowestCellVolts);
            processValue(&valueCache.highestCellVolts, batteryManager->getHighestCellVolts(), highestCellVolts);
            processValue(&valueCache.averageCellVolts, batteryManager->getAverageCellVolts(), averageCellVolts);
            processValue(&valueCache.deltaCellVolts, batteryManager->getHighestCellVolts() - batteryManager->getLowestCellVolts(), deltaCellVolts);
            processValue(&valueCache.lowestCellVoltsId, batteryManager->getLowestCellVoltsId(), lowestCellVoltsId);
            processValue(&valueCache.highestCellVoltsId, batteryManager->getHighestCellVoltsId(), highestCellVoltsId);
        }
        if (batteryManager->hasCellResistance()) {
            processValue(&valueCache.lowestCellResistance, batteryManager->getLowestCellResistance(), lowestCellResistance);
            processValue(&valueCache.highestCellResistance, batteryManager->getHighestCellResistance(), highestCellResistance);
            processValue(&valueCache.averageCellResistance, batteryManager->getAverageCellResistance(), averageCellResistance);
            processValue(&valueCache.deltaCellResistance, (batteryManager->getHighestCellResistance() - batteryManager->getLowestCellResistance()), deltaCellResistance);
            processValue(&valueCache.lowestCellResistanceId, batteryManager->getLowestCellResistanceId(), lowestCellResistanceId);
            processValue(&valueCache.highestCellResistanceId, batteryManager->getHighestCellResistanceId(), highestCellResistanceId);
        }
        if (batteryManager->hasPackResistance()) {
            processValue(&valueCache.packResistance, batteryManager->getPackResistance(), packResistance);
        }
        if (batteryManager->hasPackHealth()) {
            processValue(&valueCache.packHealth, batteryManager->getPackHealth(), packHealth);
        }
        if (batteryManager->hasPackCycles()) {
            processValue(&valueCache.packCycles, batteryManager->getPackCycles(), packCycles);
        }
        processValue(&valueCache.bmsTemperature, batteryManager->getSystemTemperature(), bmsTemperature);
    }
}

void WifiEsp32::processValue(bool *cacheValue, bool value, DataPointCode code) {
    if (*cacheValue == value)
        return;
    *cacheValue = value;

    outBuffer[outPos++] = DATA_POINT_START;
    outBuffer[outPos++] = code;
    outBuffer[outPos++] = (value ? 1 : 0);
    dataPointCount++;
}

void WifiEsp32::processValue(uint8_t *cacheValue, uint8_t value, DataPointCode code) {
    if (*cacheValue == value)
        return;
    *cacheValue = value;

    outBuffer[outPos++] = DATA_POINT_START;
    outBuffer[outPos++] = code;
    outBuffer[outPos++] = value;
    dataPointCount++;
}

void WifiEsp32::processValue(uint16_t *cacheValue, uint16_t value, DataPointCode code) {
    if (*cacheValue == value)
        return;
    *cacheValue = value;

    outBuffer[outPos++] = DATA_POINT_START;
    outBuffer[outPos++] = code;
    outBuffer[outPos++] = (value & 0xFF00) >> 8;
    outBuffer[outPos++] = value & 0x00FF;
    dataPointCount++;
}

void WifiEsp32::processValue(int16_t *cacheValue, int16_t value, DataPointCode code) {
    if (*cacheValue == value)
        return;
    *cacheValue = value;

    outBuffer[outPos++] = DATA_POINT_START;
    outBuffer[outPos++] = code;
    outBuffer[outPos++] = (value & 0xFF00) >> 8;
    outBuffer[outPos++] = value & 0x00FF;
    dataPointCount++;
}

void WifiEsp32::processValue(uint32_t *cacheValue, uint32_t value, DataPointCode code) {
    if (*cacheValue == value)
        return;
    *cacheValue = value;

    outBuffer[outPos++] = DATA_POINT_START;
    outBuffer[outPos++] = code;
    outBuffer[outPos++] = (value & 0xFF000000) >> 24;
    outBuffer[outPos++] = (value & 0x00FF0000) >> 16;
    outBuffer[outPos++] = (value & 0x0000FF00) >> 8;
    outBuffer[outPos++] = value & 0x000000FF;
    dataPointCount++;
}

void WifiEsp32::processLimits(uint16_t *cacheValue, uint16_t value, DataPointCode code, boolean maximum) {
    if (maximum && cacheValue && value > *cacheValue) {
        processValue(cacheValue, value, code);
    }
    if (!maximum && cacheValue && value < *cacheValue) {
        processValue(cacheValue, value, code);
    }
}

void WifiEsp32::processLimits(int16_t *cacheValue, int16_t value, DataPointCode code, boolean maximum) {
    if (maximum && cacheValue && value > *cacheValue) {
        processValue(cacheValue, value, code);
    }
    if (!maximum && cacheValue && value < *cacheValue) {
        processValue(cacheValue, value, code);
    }
}

void WifiEsp32::reset() {
    running = false;
    ready = false;

    digitalWrite(CFG_WIFI_ENABLE, LOW);
    timeHeartBeat = millis();
}


/**
 * \brief Get the device type
 *
 * \return the type of the device
 */
DeviceType WifiEsp32::getType()
{
    return DEVICE_WIFI;
}

/**
 * \brief Get the device ID
 *
 * \return the ID of the device
 */
DeviceId WifiEsp32::getId()
{
    return ESP32WIFI;
}
