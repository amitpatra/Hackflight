/*
   Support for treating quaternion as a sensor
   
   Supports IMUs like EM7180 SENtral Sensor Fusion solution, where 
   quaternion is computed in hardware, and simulation platforms like
   UE4, where quaternion is provided by physics engine. For other IMUs 
   and simulators, you can use quaternion-filter classes in filters.hpp.

   Copyright (c) 2018 Simon D. Levy

   MIT License
 */

#pragma once

#include <math.h>

#include "sensors/surfacemount.hpp"

namespace hf {

    class Quaternion : public SurfaceMountSensor {

        friend class Hackflight;

        private:

            float _w = 0;
            float _x = 0;
            float _y = 0;
            float _z = 0;

        protected:

            Quaternion(void)
            {
                _w = 0;
                _x = 0;
                _y = 0;
                _z = 0;
            }

            virtual void modifyState(State & state, float time) override
            {
                (void)time;

                computeEulerAngles(_w, _x, _y, _z,
                                   state.x[State::PHI], state.x[State::THETA], state.x[State::PSI]);

                // Convert heading from [-pi,+pi] to [0,2*pi]
                if (state.x[State::PSI] < 0) {
                    state.x[State::PSI] += 2*M_PI;
                }

                // Compensate for different mounting orientations
                imu->adjustEulerAngles(state.x[State::PHI], state.x[State::THETA], state.x[State::PSI]);

            }

            virtual bool ready(float time) override
            {
                return imu->getQuaternion(_w, _x, _y, _z, time);
            }

        public:

            // We make this public so we can use it in different sketches
            static void computeEulerAngles(float qw, float qx, float qy,float qz, 
                                           float & phi, float & theta, float & psi)
            {
                phi = atan2(2.0f*(qw*qx+qy*qz),qw*qw-qx*qx-qy*qy+qz*qz);
                theta = asin(2.0f*(qx*qz-qw*qy));
                psi = atan2(2.0f*(qx*qy+qw*qz),qw*qw+qx*qx-qy*qy-qz*qz);
            }

    };  // class Quaternion

} // namespace hf
