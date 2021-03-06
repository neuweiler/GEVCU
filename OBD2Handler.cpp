/*
 * OBD2Handler.cpp - A simple utility class that can be used to handle OBDII traffic
 * of any form. It takes pointers to buffers where upwards of 8 characters reside.
 * Doesn't care about how the traffic came out or how it'll go out. That is handled
 * in places like CanOBD2 or ELM327Emu
 *
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

#include "OBD2Handler.h"

OBD2Handler::OBD2Handler()
{
    motorController = (MotorController*) deviceManager.getMotorController();
    accelPedal = (Throttle*) deviceManager.getAccelerator();
    brakePedal = (Throttle*) deviceManager.getBrake();
    BMS = (BatteryManager*) deviceManager.getDeviceByType(DEVICE_BMS);
}

OBD2Handler *OBD2Handler::getInstance()
{
    static OBD2Handler *obd2Handler = new OBD2Handler();
    return obd2Handler;
}

/*
Public method to process OBD2 requests.
    inData is whatever payload the request might need to have sent - it's OK to be NULL if this is a run of the mill PID request with no payload
    outData should be a preallocated buffer of at least 6 bytes. The format is as follows:
    outData[0] is the length of the data actually returned
    outData[1] is the returned mode (input mode + 0x40)
    there after, the rest of the bytes are the data requested. This should be 1-5 bytes

    SAE standard says that this is the format for SAE requests to us:
    byte 0 = # of bytes following
    byte 1 = mode for PID request
    byte 2 = PID requested

    However, the sky is the limit for non-SAE frames (modes over 0x09)
    In that case we'll use two bytes for our custom PIDS (sent MSB first like
    all other PID traffic) MSB = byte 2, LSB = byte 3.

    These are the PIDs I think we should support (mode 1)
    0 = lets the other side know which pids we support. A bitfield that runs from MSb of first byte to lsb of last byte (32 bits)
    1 = Returns 32 bits but we really can only support the first byte which has bit 7 = Malfunction? Bits 0-6 = # of DTCs
    2 = Freeze DTC
    4 = Calculated engine load (A * 100 / 255) - Percentage
    5 = Engine Coolant Temp (A - 40) = Degrees Centigrade
    0x0C = Engine RPM (A * 256 + B) / 4
    0x11 = Throttle position (A * 100 / 255) - Percentage
    0x1C = Standard supported (We return 1 = OBDII)
    0x1F = runtime since engine start (A*256 + B)
    0x20 = pids supported (next 32 pids - formatted just like PID 0)
    0x21 = Distance traveled with fault light lit (A*256 + B) - In km
    0x2F = Fuel level (A * 100 / 255) - Percentage
    0x40 = PIDs supported, next 32
    0x51 = What type of fuel do we use? (We use 8 = electric, presumably.)
    0x60 = PIDs supported, next 32
    0x61 = Driver requested torque (A-125) - Percentage
    0x62 = Actual Torque delivered (A-125) - Percentage
    0x63 = Reference torque for engine - presumably max torque - A*256 + B - Nm

    Mode 3
    Returns DTC (diag trouble codes) - Three per frame
    bits 6-7 = DTC first character (00 = P = Powertrain, 01=C=Chassis, 10=B=Body, 11=U=Network)
    bits 4-5 = Second char (00 = 0, 01 = 1, 10 = 2, 11 = 3)
    bits 0-3 = third char (stored as normal nibble)
    Then next byte has two nibbles for the next 2 characters (fourth char = bits 4-7, fifth = 0-3)

    Mode 9 PIDs
    0x0 = Mode 9 pids supported (same scheme as mode 1)
    0x9 = How long is ECU name returned by 0x0A?
    0xA = ASCII string of ECU name. 20 characters are to be returned, I believe 4 at a time
*/
bool OBD2Handler::processRequest(byte *inData, byte *outData)
{
    bool ret = false;

    outData[2] = inData[2]; //copy standard PID
    outData[0] = 2;

    if (inData[1] > 0x50) {
        outData[3] = inData[3]; //if using proprietary PIDs then copy second byte too
        outData[0] = 3;
    }

    uint8_t mode = inData[1];
    uint16_t pid;
    if (mode < 10) {
        pid = inData[2];
    } else {
        pid = inData[2] * 256 + inData[3];
    }

    switch (mode) {
        case 1: //show current data
            ret = processShowData(pid, inData, outData);
            outData[1] = mode + 0x40;
            outData[2] = pid;
            break;

        case 2: //show freeze frame data - not sure we'll be supporting this
            break;

        case 3: //show stored diagnostic codes - we can probably map our faults to some existing DTC codes or roll our own
            break;

        case 4: //clear diagnostic trouble codes - If we get this frame we just clear all codes no questions asked.
            break;

        case 6: //test results over CANBus (this replaces mode 5 from non-canbus) - I know nothing of this
            break;

        case 7: //show pending diag codes (current or last driving cycle) - Might just overlap with mode 3
            break;

        case 8: //control operation of on-board systems - this sounds really proprietary and dangerous. Maybe ignore this?
            break;

        case 9: //request vehicle info - We can identify ourselves here but little else
            break;

        case 0x20: //custom PID codes we made up for GEVCU
            break;
    }

    return ret;
}

//Process SAE standard PID requests. Function returns whether it handled the request or not.
bool OBD2Handler::processShowData(uint16_t pid, byte *inData, byte *outData)
{
    int temp;


    switch (pid) {
        case 0: //pids 1-0x20 that we support - bitfield
            //returns 4 bytes so immediately indicate that.
            outData[0] = 4;
            outData[3] = 0b11011000; //pids 1 - 8 - starting with pid 1 in the MSB and going from there
            outData[4] = 0b00010000; //pids 9 - 0x10
            outData[5] = 0b10000000; //pids 0x11 - 0x18
            outData[6] = 0b00010011; //pids 0x19 - 0x20
            return true;
            break;

        case 1: //Returns 32 bits but we really can only support the first byte which has bit 7 = Malfunction? Bits 0-6 = # of DTCs
            outData[0] = 4;
            outData[3] = 0; //TODO: We aren't properly keeping track of faults yet but when we do fix this.
            outData[4] = 0; //these next three are really related to ICE diagnostics
            outData[5] = 0; //so ignore them.
            outData[6] = 0;
            return true;
            break;

        case 2: //Freeze DTC
            return false; //don't support freeze framing yet. Might be useful in the future.
            break;

        case 4: //Calculated engine load (A * 100 / 255) - Percentage
            temp = (255 * motorController->getTorqueActual()) / motorController->getTorqueAvailable();
            outData[0] = 1;
            outData[3] = (uint8_t)(temp & 0xFF);
            return true;
            break;

        case 5: //Engine Coolant Temp (A - 40) = Degrees Centigrade
            //our code stores temperatures as a signed integer for tenths of a degree so translate
            temp =  motorController->getTemperatureController() / 10;

            if (temp < -40) {
                temp = -40;
            }

            if (temp > 215) {
                temp = 215;
            }

            temp += 40;
            outData[0] = 1; //returning only one byte
            outData[3] = (uint8_t)(temp);
            return true;
            break;

        case 0xC: //Engine RPM (A * 256 + B) / 4
            temp = motorController->getSpeedActual() * 4; //we store in RPM while the PID code wants quarter rpms
            outData[0] = 2;
            outData[3] = (uint8_t)(temp / 256);
            outData[4] = (uint8_t)(temp);
            return true;
            break;

        case 0x11: //Throttle position (A * 100 / 255) - Percentage
            temp = motorController->getThrottleLevel() / 10; //getThrottle returns in 10ths of a percent

            if (temp < 0) {
                temp = 0;    //negative throttle can't be shown for OBDII
            }

            temp = (255 * temp) / 100;
            outData[0] = 1;
            outData[3] = (uint8_t)(temp);
            return true;
            break;

        case 0x1C: //Standard supported (We return 1 = OBDII)
            outData[0] = 1;
            outData[3] = 1;
            return true;
            break;

        case 0x1F: //runtime since engine start (A*256 + B)
            outData[0] = 2;
            outData[3] = 0; //TODO: Get the actual runtime.
            outData[4] = 0;
            return true;
            break;

        case 0x20: //pids supported (next 32 pids - formatted just like PID 0)
            outData[0] = 4;
            outData[3] = 0b10000000; //pids 0x21 - 0x28 - starting with pid 0x21 in the MSB and going from there
            outData[4] = 0b00000010; //pids 0x29 - 0x30
            outData[5] = 0b00000000; //pids 0x31 - 0x38
            outData[6] = 0b00000001; //pids 0x39 - 0x40
            return true;
            break;

        case 0x21: //Distance traveled with fault light lit (A*256 + B) - In km
            outData[0] = 2;
            outData[3] = 0; //TODO: Can we get this information?
            outData[4] = 0;
            return true;
            break;

        case 0x2F: //Fuel level (A * 100 / 255) - Percentage
            outData[0] = 1;
            outData[3] = 0; //TODO: finish BMS interface and get this value
            return true;
            break;

        case 0x40: //PIDs supported, next 32
            outData[0] = 4;
            outData[3] = 0b00000000; //pids 0x41 - 0x48 - starting with pid 0x41 in the MSB and going from there
            outData[4] = 0b00000000; //pids 0x49 - 0x50
            outData[5] = 0b10000000; //pids 0x51 - 0x58
            outData[6] = 0b00000001; //pids 0x59 - 0x60
            return true;
            break;

        case 0x51: //What type of fuel do we use? (We use 8 = electric, presumably.)
            outData[0] = 1;
            outData[3] = 8;
            return true;
            break;

        case 0x60: //PIDs supported, next 32
            outData[0] = 4;
            outData[3] = 0b11100000; //pids 0x61 - 0x68 - starting with pid 0x61 in the MSB and going from there
            outData[4] = 0b00000000; //pids 0x69 - 0x70
            outData[5] = 0b00000000; //pids 0x71 - 0x78
            outData[6] = 0b00000000; //pids 0x79 - 0x80
            return true;
            break;

        case 0x61: //Driver requested torque (A-125) - Percentage
            temp = (100 * motorController->getTorqueRequested()) / motorController->getTorqueAvailable();
            temp += 125;
            outData[0] = 1;
            outData[3] = (uint8_t) temp;
            return true;
            break;

        case 0x62: //Actual Torque delivered (A-125) - Percentage
            temp = (100 * motorController->getTorqueActual()) / motorController->getTorqueAvailable();
            temp += 125;
            outData[0] = 1;
            outData[3] = (uint8_t) temp;
            return true;
            break;

        case 0x63: //Reference torque for engine - presumably max torque - A*256 + B - Nm
            temp = motorController->getTorqueAvailable();
            outData[0] = 2;
            outData[3] = (uint8_t)(temp / 256);
            outData[4] = (uint8_t)(temp & 0xFF);
            return true;
            break;
    }

    return false;
}

bool OBD2Handler::processShowCustomData(uint16_t pid, byte *inData, byte *outData)
{
    switch (pid) {
    }
    return false;
}


