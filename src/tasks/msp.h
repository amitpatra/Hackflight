/*
   Copyright (c) 2022 Simon D. Levy

   This file is part of Hackflight.

   Hackflight is free software: you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation, either version 3 of the License, or (at your option)
   any later version.

   Hackflight is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
   more details.

   You should have received a copy of the GNU General Public License along with
   Hackflight. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "task.h"
#include "../msp.h"

class MspTask : public Task {
    
    friend class Hackflight;

    private:

        Msp *          m_msp;
        VehicleState * m_vstate;

        float  motors[MAX_SUPPORTED_MOTORS];

        Receiver::sticks_t * m_rxSticks;

    public:

        MspTask() : Task(100) { } // Hz

        void begin(
                Msp & msp,
                Esc * esc,
                Arming * arming,
                Receiver::sticks_t * rxSticks,
                VehicleState * vstate)
        {
            m_msp = &msp;

            m_vstate = vstate;

            m_msp->begin(esc, arming);

            m_rxSticks = rxSticks;
        }

        virtual void fun(uint32_t usec) override
        {
            (void)usec;

            m_msp->update(m_vstate, m_rxSticks, motors);
        }
};
