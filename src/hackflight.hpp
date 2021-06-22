/*
   Hackflight core algorithm

   Copyright (c) 2020 Simon D. Levy

   MIT License
 */

#pragma once

#include "mspparser.hpp"
#include "receiver.hpp"
#include "state.hpp"
#include "pidcontroller.hpp"
#include "motor.hpp"
#include "pidtask.hpp"
#include "serialtask.hpp"

#include <RFT_board.hpp>
#include <RFT_sensor.hpp>
#include <RFT_actuator.hpp>
#include <RFT_debugger.hpp>
#include <RFT_filters.hpp>

namespace hf {

    class Hackflight {

        private:

            static constexpr float MAX_ARMING_ANGLE_DEGREES = 25.0f;

            // Supports periodic ad-hoc debugging
            rft::Debugger _debugger;

            // Actuator
            rft::Actuator * _actuator = NULL;

            // Sensors 
            rft::Sensor * _sensors[256] = {};
            uint8_t _sensor_count = 0;

            // Safety
            bool _safeToArm = false;

            // Timer task for PID controllers
            PidTask _pidTask;

            // Serial timer task for GCS
            SerialTask _serialTask;

            bool safeAngle(uint8_t axis)
            {
                return fabs(_state.x[axis]) < rft::Filter::deg2rad(MAX_ARMING_ANGLE_DEGREES);
            }

            rft::Board  * _board = NULL;
            Receiver * _receiver = NULL;

            // Vehicle state
            State _state;

            void checkSensors(void)
            {
                for (uint8_t k=0; k<_sensor_count; ++k) {
                    rft::Sensor * sensor = _sensors[k];
                    float time = _board->getTime();
                    if (sensor->ready(time)) {
                        sensor->modifyState((rft::State *)&_state, time);
                    }
                }
            }

            void checkReceiver(void)
            {
                // Sync failsafe to receiver
                if (_receiver->lostSignal() && _state.armed) {
                    _actuator->cut();
                    _state.armed = false;
                    _state.failsafe = true;
                    _board->showArmedStatus(false);
                    return;
                }

                // Check whether receiver data is available
                if (!_receiver->ready()) return;

                // Disarm
                if (_state.armed && !_receiver->inArmedState()) {
                    _state.armed = false;
                } 

                // Avoid arming if aux1 switch down on startup
                if (!_safeToArm) {
                    _safeToArm = !_receiver->inArmedState();
                }

                // Arm (after lots of safety checks!)
                if (_safeToArm && !_state.armed && _receiver->inactive() && _receiver->inArmedState() && 
                        !_state.failsafe && safeAngle(State::PHI) && safeAngle(State::THETA)) {
                    _state.armed = true;
                }

                // Cut motors on throttle-down
                if (_state.armed && _receiver->inactive()) {
                    _actuator->cut();
                }

                // Set LED based on arming status
                _board->showArmedStatus(_state.armed);

            } // checkReceiver

            void startSensors(void)
            {
                for (uint8_t k=0; k<_sensor_count; ++k) {
                    _sensors[k]->begin();
                }
             }

        public:

            Hackflight(rft::Board * board, Receiver * receiver, rft::Actuator * actuator)
            {  
                // Store the essentials
                _board = board;
                _receiver = receiver;
                _actuator = actuator;

                // Support adding new sensors
                _sensor_count = 0;
            }

            void begin(bool armed=false)
            {  
                // Start the board
                _board->begin();

                // Ad-hoc debugging support
                _debugger.begin(_board);

                // Initialize state
                memset(&_state.x, 0, sizeof(_state.x));

                // Start the receiver
                _receiver->begin();

                // Setup failsafe
                _state.failsafe = false;

                // Initialize timer task for PID controllers
                _pidTask.begin(_board, _receiver, _actuator, &_state);
 
                // Initialize serial timer task
                _serialTask.begin(_board, &_state, _receiver, _actuator);

                // Support safety override by simulator
                _state.armed = armed;

                // Start the sensors
                startSensors();

                // Tell the actuator to start the motors
                _actuator->begin();

            } // begin

            void addSensor(rft::Sensor * sensor) 
            {
                _sensors[_sensor_count++] = sensor;
            }

            void addPidController(PidController * pidController, uint8_t auxState=0) 
            {
                _pidTask.addPidController(pidController, auxState);
            }

            void update(void)
            {
                // Grab control signal if available
                checkReceiver();

                // Update PID controllers task
                _pidTask.update();

                // Check sensors
                checkSensors();

                // Update serial comms task
                _serialTask.update();
            }

    }; // class Hackflight

} // namespace
