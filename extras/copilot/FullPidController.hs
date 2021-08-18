{--
  Classic PID controller

  Copyright(C) 2021 Simon D.Levy

  MIT License
--}

{-# LANGUAGE RebindableSyntax #-}
{-# LANGUAGE DataKinds        #-}

module FullPidController

where

import Language.Copilot
import Copilot.Compile.C99

import Utils(constrain_abs)

data FullPidState = 

    FullPidState { errorIntegral :: Stream Double,
                   deltaError1 :: Stream Double,
                   deltaError2 :: Stream Double,
                   errorPrev :: Stream Double }

initFullPidState :: FullPidState

initFullPidState = FullPidState 0 0 0 0

computeDemand :: Stream Double ->  -- kp
                 Stream Double ->  -- ki
                 Stream Double ->  -- kd
                 Stream Double ->  -- windupMax
                 Stream Double ->  -- valueMax
                 FullPidState ->   
                 Stream Double ->  -- demand
                 Stream Double ->  -- value
                 (Stream Double, FullPidState)

computeDemand kp ki kd windupMax valueMax pidState demand value =

    let 

        --  Reset PID state on quick value change
        reset = abs value > valueMax
        errorIntegral' = if reset then 0 else errorIntegral pidState
        errorPrev' = if reset then 0 else errorPrev pidState
        deltaError1' = if reset then 0 else deltaError1 pidState
        deltaError2' = if reset then 0 else deltaError2 pidState

        err = demand - value

        errI = constrain_abs (errorIntegral' + err) windupMax

        deltaErr = err - errorPrev'

        -- Run a simple low-pass filter on the error derivative
        errD = deltaError1' + deltaError2' + deltaErr

    in (kp * err + ki * errI + kd * errD, 
       FullPidState errorIntegral' deltaErr  deltaError1' err)
