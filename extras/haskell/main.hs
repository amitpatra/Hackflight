{--
  Copyright(C) 2021 Simon D.Levy

  MIT License
--}

import Hackflight(hackflightFun)
import Mixer(quadXAPMixer)
import Server(runServer)
import PidControllers(rateController,
                      levelController,
                      altHoldController,
                      posHoldController,
                      yawController)

main :: IO ()

main = let 
           rate = rateController 0.225    -- Kp
                                 0.001875 -- Ki
                                 0.375    -- Kd
                                 0.4      -- windupMax
                                 40       -- maxDegreesPerSecond

           yaw = yawController 2.0 -- Kp
                               0.1 -- Ki
                               0.4 -- windupMax

           level = levelController 0.2 -- Kp
                                   45  -- maxAngleDegrees

           altHold = altHoldController 0.75 -- Kp
                                       1.5  -- Ki
                                       0.4  -- windupMax
                                       2.5  -- pilotVelZMax
                                       0.2  -- stickDeadband

           posHold = posHoldController 0.1 -- Kp
                                       0.2 -- stickDeadband

       in runServer hackflightFun
                    [posHold, rate, level, altHold, yaw]
                    quadXAPMixer
