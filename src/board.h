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

#pragma once

#include <stdint.h>
#include <stdarg.h>

#include <vector>
using namespace std;

#include "arming.h"
#include "core/mixer.h"
#include "core/motors.h"
#include "esc.h"
#include "imu.h"
#include "receiver.h"
#include "scheduler.h"
#include "task/accelerometer.h"
#include "task/attitude.h"
#include "task/visualizer.h"
#include "task/receiver.h"
#include "warning.h"

class Board {

    private:

        // Gyro interrupt counts over which to measure loop time and skew
        static const uint32_t CORE_RATE_COUNT = 25000;
        static const uint32_t GYRO_LOCK_COUNT = 400;

        // Motor safety
        bool m_failsafeIsActive;

        // Arming safety
        Arming m_arming;

        // LED
        uint8_t m_ledPin;
        bool m_ledInverted;

        Scheduler m_scheduler;

        VehicleState m_vstate;

        AttitudeTask m_attitudeTask = AttitudeTask(m_vstate);

        ReceiverTask m_receiverTask;

        VisualizerTask m_visualizerTask =
            VisualizerTask(m_msp, m_vstate, m_skyrangerTask);

        Msp m_msp;

        // Initialzed in sketch()
        Esc *   m_esc;
        Mixer * m_mixer;
        vector<PidController *> * m_pidControllers;

        void checkCoreTasks(uint32_t nowCycles)
        {
            const uint32_t usec = micros(); // unsafe

            int32_t loopRemainingCycles = m_scheduler.getLoopRemainingCycles();
            uint32_t nextTargetCycles = m_scheduler.getNextTargetCycles();

            m_scheduler.corePreUpdate();

            while (loopRemainingCycles > 0) {
                nowCycles = getCycleCounter(); // unsafe
                loopRemainingCycles =
                    intcmp(nextTargetCycles, nowCycles);
            }

            if (m_imu->gyroIsReady()) {

                auto angvels = m_imu->readGyroDps();

                m_vstate.dphi   = angvels.x;
                m_vstate.dtheta = angvels.y;
                m_vstate.dpsi   = angvels.z;
            }

            Demands demands = m_receiverTask.receiver->getDemands();

            auto motors = m_mixer->step(
                    demands,
                    m_vstate,
                    m_pidControllers,
                    m_receiverTask.receiver->gotPidReset(),
                    usec);

            float mixmotors[Motors::MAX_SUPPORTED] = {};

            for (auto i=0; i<m_mixer->getMotorCount(); i++) {

                mixmotors[i] =
                    m_esc->getMotorValue(motors.values[i], m_failsafeIsActive);
            }

            // unsafe; we should move unsafe ESC code to Board class
            m_esc->write(m_arming.isArmed ?  mixmotors : m_visualizerTask.motors);

            m_scheduler.corePostUpdate(nowCycles);

            // Bring the scheduler into lock with the gyro Track the actual
            // gyro rate over given number of cycle times and set the expected
            // timebase
            static uint32_t _terminalGyroRateCount;
            static int32_t _sampleRateStartCycles;

            if ((_terminalGyroRateCount == 0)) {
                _terminalGyroRateCount =
                    m_imu->getGyroInterruptCount() + CORE_RATE_COUNT;
                _sampleRateStartCycles = nowCycles;
            }

            if (m_imu->getGyroInterruptCount() >= _terminalGyroRateCount) {
                // Calculate number of clock cycles on average between gyro
                // interrupts
                uint32_t sampleCycles = nowCycles - _sampleRateStartCycles;
                m_scheduler.desiredPeriodCycles = sampleCycles / CORE_RATE_COUNT;
                _sampleRateStartCycles = nowCycles;
                _terminalGyroRateCount += CORE_RATE_COUNT;
            }

            // Track actual gyro rate over given number of cycle times and
            // remove skew
            static uint32_t _terminalGyroLockCount;
            static int32_t _gyroSkewAccum;

            auto gyroSkew =
                m_imu->getGyroSkew(nextTargetCycles, m_scheduler.desiredPeriodCycles);

            _gyroSkewAccum += gyroSkew;

            if ((_terminalGyroLockCount == 0)) {
                _terminalGyroLockCount =
                    m_imu->getGyroInterruptCount() + GYRO_LOCK_COUNT;
            }

            if (m_imu->getGyroInterruptCount() >= _terminalGyroLockCount) {
                _terminalGyroLockCount += GYRO_LOCK_COUNT;

                // Move the desired start time of the gyroSampleTask
                m_scheduler.lastTargetCycles -= (_gyroSkewAccum/GYRO_LOCK_COUNT);

                _gyroSkewAccum = 0;
            }

        } // checkCoreTasks

        bool readyToArm(void)
        {
            return 
                m_arming.accDoneCalibrating &&
                m_arming.angleOkay &&
                !m_arming.gotFailsafe &&
                m_arming.haveSignal &&
                m_arming.gyroDoneCalibrating &&
                m_arming.switchOkay &&
                m_arming.throttleIsDown;
        }

        void disarm(void)
        {
            if (m_arming.isArmed) {
                m_esc->stop();
            }

            m_arming.isArmed = false;
        }

        void attemptToArm(const uint32_t usec, const bool aux1IsSet)
        {
            static bool _doNotRepeat;

            if (aux1IsSet) {

                if (readyToArm()) {

                    if (m_arming.isArmed) {
                        return;
                    }

                    if (!m_esc->isReady(usec)) {
                        return;
                    }

                    m_arming.isArmed = true;
                }

            } else {

                if (m_arming.isArmed) {
                    disarm();
                    m_arming.isArmed = false;
                }
            }

            if (!(m_arming.isArmed || _doNotRepeat || !readyToArm())) {
                _doNotRepeat = true;
            }
        }

        Warning m_warning;

        int32_t getTaskGuardCycles(void)
        {
            return m_scheduler.getTaskGuardCycles();
        }

        //////////////////////// unsafe below here ////////////////////////////

        uint32_t getAnticipatedEndCycles(Task & task)
        {
            const auto nowCycles = getCycleCounter();

            const uint32_t taskRequiredCycles = 
                task.checkReady(
                        m_scheduler.getNextTargetCycles(),
                        nowCycles,
                        getTaskGuardCycles());

            return taskRequiredCycles > 0 ? 
                    nowCycles + taskRequiredCycles :
                    0;
        }

        void updateArmingFromReceiver(Receiver * receiver, const uint32_t usec)
        {
            switch (receiver->getState()) {

                case Receiver::STATE_UPDATE:
                    attemptToArm(usec, receiver->aux1IsSet());
                    break;

                case Receiver::STATE_CHECK:
                    updateFromReceiver(
                            receiver->throttleIsDown(),
                            receiver->aux1IsSet(),
                            receiver->hasSignal());
                    break;

                default:
                    break;
            }
        }

        void updateFromReceiver(
                const bool throttleIsDown, const bool aux1IsSet, const bool haveSignal)
        {
            if (m_arming.isArmed) {

                if (!haveSignal && m_arming.haveSignal) {
                    m_arming.gotFailsafe = true;
                    disarm();
                }
                else {
                    ledSet(true);
                }
            } else {

                m_arming.throttleIsDown = throttleIsDown;

                // If arming is disabled and the ARM switch is on
                if (!readyToArm() && aux1IsSet) {
                    m_arming.switchOkay = false;
                } else if (!aux1IsSet) {
                    m_arming.switchOkay = true;
                }

                if (!readyToArm()) {
                    m_warning.blink();
                } else {
                    m_warning.disable();
                }

                ledWarningUpdate();
            }

            m_arming.haveSignal = haveSignal;
        }

        void ledToggle(void)
        {
            m_warning.toggleLed();
            ledSet(m_warning.ledOn);
        }

        void ledWarningUpdate(void)
        {
            if ((int32_t)(micros() - m_warning.timer) < 0) {
                return;
            }

            switch (m_warning.state) {
                case Warning::OFF:
                    ledSet(false);
                    break;
                case Warning::ON:
                    ledSet(true);
                    break;
                case Warning::BLINK:
                    ledToggle();
                    break;
            }

            m_warning.setTimer(micros());
        }

        void checkDynamicTasks(void)
        {
            if (m_visualizerTask.gotRebootRequest()) {
                reboot();
            }

            Task::prioritizer_t prioritizer = {Task::NONE, 0};

            const uint32_t usec = micros(); 

            m_receiverTask.prioritize(usec, prioritizer);
            m_attitudeTask.prioritize(usec, prioritizer);
            m_visualizerTask.prioritize(usec, prioritizer);

            prioritizeExtraTasks(prioritizer, usec);

            switch (prioritizer.id) {

                case Task::ATTITUDE:
                    runTask(m_attitudeTask);
                    m_arming.updateFromImu(*m_imu, m_vstate);
                    break;

                case Task::VISUALIZER:
                    runVisualizerTask();
                    break;

                case Task::RECEIVER:
                    runTask(m_receiverTask);
                    updateArmingFromReceiver(m_receiverTask.receiver, micros());
                    break;

                case Task::ACCELEROMETER:
                    runTask(m_accelerometerTask);
                    break;

                case Task::SKYRANGER:
                    runTask(m_skyrangerTask);
                    break;

                default:
                    break;
            }
        }

        void runTask(Task & task)
        {
            const uint32_t anticipatedEndCycles = getAnticipatedEndCycles(task);

            if (anticipatedEndCycles > 0) {

                const uint32_t usec = micros();

                task.run(usec);

                postRunTask(task, usec, anticipatedEndCycles);
            } 
        }

        void postRunTask(
                Task & task,
                const uint32_t usec,
                const uint32_t anticipatedEndCycles)
        {
            task.update(usec, micros()-usec);
            m_scheduler.updateDynamic(getCycleCounter(), anticipatedEndCycles);
        }

        void ledSet(bool on)
        {
            if (m_ledPin > 0) {
                digitalWrite(m_ledPin, m_ledInverted ? on : !on);
            }

            m_warning.ledOn = on;
        }

        void ledBegin(void)
        {
            if (m_ledPin > 0) {
                pinMode(m_ledPin, OUTPUT);
            }
        }

        void ledFlash(uint8_t reps, uint16_t delayMs)
        {
            ledSet(false);
            for (auto i=0; i<reps; i++) {
                ledToggle();
                delay(delayMs);
            }
            ledSet(false);
        }

        // STM32F boards have no auto-reset bootloader support, so we reboot on
        // an external input
        virtual void reboot(void)
        {
        }

        static void outbuf(char * buf)
        {
            Serial.print(buf); 
            Serial.flush();
        }

        void runVisualizerTask(void)
        {
            const uint32_t anticipatedEndCycles =
                getAnticipatedEndCycles(m_visualizerTask);

            if (anticipatedEndCycles > 0) {

                const auto usec = micros();

                while (Serial.available()) {

                    if (m_visualizerTask.parse(Serial.read())) {
                        Serial.write(m_msp.payload, m_msp.payloadSize);
                    }
                }

                postRunTask(m_visualizerTask, usec, anticipatedEndCycles);
            }
        }


    protected:

        // Initialized in sketch
        Imu * m_imu;

        AccelerometerTask m_accelerometerTask; 

        SkyrangerTask m_skyrangerTask = SkyrangerTask(m_vstate);

        Board(
                Receiver & receiver,
                Imu & imu,
                vector<PidController *> & pidControllers,
                Mixer & mixer,
                Esc & esc,
                const int8_t ledPin)
        {
            m_receiverTask.receiver = &receiver;

            m_imu = &imu;
            m_pidControllers = &pidControllers;
            m_mixer = &mixer;
            m_esc = &esc;

            m_ledPin = ledPin < 0 ? -ledPin : ledPin;
            m_ledInverted = ledPin < 0;

            esc.m_board = this;
            receiver.m_board = this;
        }

        virtual void prioritizeExtraTasks(
                Task::prioritizer_t & prioritizer, const uint32_t usec)
        {
            (void)prioritizer;
            (void)usec;
        }

    public:

        uint32_t microsToCycles(uint32_t micros)
        {
            return getClockSpeed() / 1000000 * micros;
        }

        virtual uint32_t getClockSpeed(void)  = 0;

        virtual uint32_t getCycleCounter(void) = 0;

        virtual void startCycleCounter(void) = 0;

        virtual void dmaInit(
                const vector<uint8_t> * motorPins, const uint32_t outputFreq)
        {
            (void)motorPins;
            (void)outputFreq;
        }

        virtual void dmaUpdateComplete(void)
        {
        }

        virtual void dmaUpdateStart(void)
        {
        }

        virtual void dmaWriteMotor(uint8_t index, uint16_t packet)
        {
            (void)index;
            (void)packet;
        }

        void begin(void)
        {
            startCycleCounter();

            m_attitudeTask.begin(m_imu);

            m_visualizerTask.begin(m_esc, m_receiverTask.receiver);

            m_imu->begin(getClockSpeed());

            m_esc->begin();

            ledBegin();
            ledFlash(10, 50);
        }

        void step(void)
        {
            // Realtime gyro/filtering/PID task get complete priority
            auto nowCycles = getCycleCounter();

            if (m_scheduler.isCoreReady(nowCycles)) {
                checkCoreTasks(nowCycles);
            }

            if (m_scheduler.isDynamicReady(getCycleCounter())) {
                checkDynamicTasks();
            }
        }

        void step(HardwareSerial & serial)
        {
            step();

            while (m_skyrangerTask.imuDataAvailable()) {
                serial.write(m_skyrangerTask.readImuData());
            }
        }

        static void setInterrupt(
                const uint8_t pin, void (*irq)(void), const uint32_t mode)
        {
            pinMode(pin, INPUT);
            attachInterrupt(pin, irq, mode);  
        }

        static void handleReceiverSerialEvent(
                Receiver & rx, HardwareSerial & serial) {

            while (serial.available()) {

                rx.parse(serial.read(), micros());
            }
        }

        static void printf(const char * fmt, ...)
        {
            va_list ap;
            va_start(ap, fmt);
            char buf[200];
            vsnprintf(buf, 200, fmt, ap); 
            outbuf(buf);
            va_end(ap);
        }

        static void reportForever(const char * fmt, ...)
        {
            va_list ap;
            va_start(ap, fmt);
            char buf[200];
            vsnprintf(buf, 200, fmt, ap); 
            va_end(ap);

            strcat(buf, "\n");

            while (true) {
                outbuf(buf);
                delay(500);
            }
        }

}; // class Board
