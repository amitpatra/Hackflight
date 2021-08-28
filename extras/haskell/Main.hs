{--
  Haskell Copilot support for Hackflight

  Copyright(C) 2021 on D.Levy

  MIT License
--}

{-# LANGUAGE RebindableSyntax #-}
{-# LANGUAGE DataKinds        #-}

module Main where

import Language.Copilot
import Copilot.Compile.C99

import Mixer

-- Sensors
import Gyrometer
import Quaternion
-- import Altimeter
-- import OpticalFlow

-- PID controllers
import RatePid
import YawPid
import LevelPid
-- import AltHoldPid
-- import PosHoldPid

import Hackflight

import State

spec = do

  -- These sensors will be run right-to-left via composition
  -- let sensors = [opticalFlow, gyrometer, quaternion, altimeter]
  let sensors = [gyrometer, quaternion]

  let rate = rateController 0.225    -- Kp
                            0.001875 -- Ki
                            0.375    -- Kd
                            0.4      -- windupMax
                            40       -- maxDegreesPerSecond

  let yaw = yawController 2.0 -- Kp
                          0.1 -- Ki
                          0.4 -- windupMax

  let level = levelController 0.2 -- Kp
                              45  -- maxAngleDegrees

  {--
  let altHold = altHoldController 0.75 -- Kp
                                  1.5  -- Ki
                                  0.4  -- windupMax
                                  2.5  -- pilotVelZMax
                                  0.2  -- stickDeadband

  let posHold = posHoldController 0.1 -- Kp
                                  0.2 -- stickDeadband
  --}

  -- Pos-hold goes first so that it can access roll/pitch demands from receiver
  -- let pidControllers = [posHold, rate, yaw, level, altHold]
  let pidControllers = [rate, yaw, level]

  let mixer = QuadXAPMixer

  let demands = hackflight sensors pidControllers

  -- Use the mixer to convert the demands into motor values
  let m1 = getMotor1 mixer demands
  let m2 = getMotor2 mixer demands
  let m3 = getMotor3 mixer demands
  let m4 = getMotor4 mixer demands

  -- Send the motor values to the external C function
  trigger "copilot_runMotors" true [arg m1, arg m2, arg m3, arg m4]

-- Compile the spec
main = reify spec >>= compile "hackflight"
