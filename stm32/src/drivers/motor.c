/*
This file is part of Hackflight.

Hackflight is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

Hackflight is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
Hackflight. If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <time.h>
#include <maths.h>
#include <motor.h>

#include "platform.h"
#include "dshot.h" // for DSHOT_ constants in mixerInitEscEndpoints
#include "pwm_output.h" // for PWM_TYPE_* and others
#include "dshot_bitbang.h"
#include "dshot_dpwm.h"
#include "motordev.h"
#include "systemdev.h"

#define CONVERT_PARAMETER_TO_PERCENT(param) (0.01f * param)

static FAST_DATA_ZERO_INIT motorDevice_t *motorDevice;

static bool motorProtocolEnabled = false;
static bool motorProtocolDshot = false;

static bool motorEnableNull(void)
{
    return false;
}

static void motorDisableNull(void)
{
}

static bool motorIsEnabledNull(uint8_t index)
{
    UNUSED(index);

    return false;
}

static void motorShutdownNull(void)
{
}

static void motorWriteIntNull(uint8_t index, uint16_t value)
{
    UNUSED(index);
    UNUSED(value);
}


static float motorConvertFromExternalNull(uint16_t value)
{
    UNUSED(value);
    return 0.0f ;
}

static uint16_t motorConvertToExternalNull(float value)
{
    UNUSED(value);
    return 0;
}

static const motorVTable_t motorNullVTable = {
    .postInit = motorPostInitNull,
    .enable = motorEnableNull,
    .disable = motorDisableNull,
    .isMotorEnabled = motorIsEnabledNull,
    .updateStart = motorUpdateStartNull,
    .write = motorWriteNull,
    .writeInt = motorWriteIntNull,
    .updateComplete = motorUpdateCompleteNull,
    .convertExternalToMotor = motorConvertFromExternalNull,
    .convertMotorToExternal = motorConvertToExternalNull,
    .shutdown = motorShutdownNull,
};

static motorDevice_t motorNullDevice = {
    .initialized = false,
    .enabled = false,
};


// ======================================================================================

void motorShutdown(void)
{
    motorDevice->vTable.shutdown();
    motorDevice->enabled = false;
    motorDevice->motorEnableTimeMs = 0;
    motorDevice->initialized = false;
    delayMicroseconds(1500);
}
void motorWrite(float *values)
{
    if (motorDevice->enabled) {
        if (!motorDevice->vTable.updateStart()) {
            return;
        }
        for (int i = 0; i < motorDevice->count; i++) {
            motorDevice->vTable.write(i, values[i]);
        }
        motorDevice->vTable.updateComplete();
    }
}

unsigned motorDeviceCount(void)
{
    return motorDevice->count;
}

motorVTable_t motorGetVTable(void)
{
    return motorDevice->vTable;
}

//bool checkMotorProtocolEnabled(const motorDevConfig_t *motorDevConfig, bool *isProtocolDshot)
bool checkMotorProtocolEnabled(bool *isProtocolDshot)
{
    //(void)motorDevConfig;

    bool enabled = false;
    bool isDshot = false;

    switch (MOTOR_PWM_PROTOCOL) {
    case PWM_TYPE_STANDARD:
    case PWM_TYPE_ONESHOT125:
    case PWM_TYPE_ONESHOT42:
    case PWM_TYPE_MULTISHOT:
    case PWM_TYPE_BRUSHED:
        enabled = true;

        break;

    case PWM_TYPE_DSHOT150:
    case PWM_TYPE_DSHOT300:
    case PWM_TYPE_DSHOT600:
    case PWM_TYPE_PROSHOT1000:
        enabled = true;
        isDshot = true;

        break;
    default:

        break;
    }

    if (isProtocolDshot) {
        *isProtocolDshot = isDshot;
    }

    return enabled;
}

float motorConvertFromExternal(uint16_t externalValue)
{
    return motorDevice->vTable.convertExternalToMotor(externalValue);
}

uint16_t motorConvertToExternal(float motorValue)
{
    return motorDevice->vTable.convertMotorToExternal(motorValue);
}

void motorPostInit()
{
    motorDevice->vTable.postInit();
}

void motorPostInitNull(void)
{
}

bool motorUpdateStartNull(void)
{
    return true;
}

void motorWriteNull(uint8_t index, float value)
{
    UNUSED(index);
    UNUSED(value);
}

void motorUpdateCompleteNull(void)
{
}

bool motorIsProtocolEnabled(void)
{
    return motorProtocolEnabled;
}

bool motorIsProtocolDshot(void)
{
    return motorProtocolDshot;
}

void motorInit(uint8_t motorCount) {

    motorProtocolEnabled = checkMotorProtocolEnabled(&motorProtocolDshot);

    memset(motors, 0, sizeof(motors));

    motorDevice = dshotBitbangDevInit(motorCount);

    if (motorDevice) {
        motorDevice->count = motorCount;
        motorDevice->initialized = true;
        motorDevice->motorEnableTimeMs = 0;
        motorDevice->enabled = false;
    } else {
        motorNullDevice.vTable = motorNullVTable;
        motorDevice = &motorNullDevice;
    }
}

void motorDisable(void)
{
    motorDevice->vTable.disable();
    motorDevice->enabled = false;
    motorDevice->motorEnableTimeMs = 0;
}

void motorEnable(void)
{
    if (motorDevice->initialized && motorDevice->vTable.enable()) {
        motorDevice->enabled = true;
        motorDevice->motorEnableTimeMs = millis();
    }
}

bool motorIsEnabled(void)
{
    return motorDevice->enabled;
}

bool motorIsMotorEnabled(uint8_t index)
{
    return motorDevice->vTable.isMotorEnabled(index);
}

uint32_t motorGetMotorEnableTimeMs(void)
{
    return motorDevice->motorEnableTimeMs;
}

float getDigitalIdleOffset(void)
{
    uint16_t digitalIdleOffsetValue = 450;
    return CONVERT_PARAMETER_TO_PERCENT(digitalIdleOffsetValue * 0.01f);
}
