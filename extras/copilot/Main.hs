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

import Altimeter
import Gyrometer
import Euler

import YawPid
import AltHoldPid

import Hackflight

spec = do

  let sensors = [euler, gyrometer, altimeter]

  let altHold = altHoldController 0.75 -- Kp
                                  1.5  -- Ki
                                  0.4  -- windupMax
                                  2.5  -- pilotVelZMax
                                  0.2  -- stickDeadband

  let yaw = yawController 2.0 -- Kp
                          0.1 -- Ki
                          0.4 -- windupMax

  let pidControllers = [yaw, altHold]

  let mixer = QuadXAPMixer

  let demands = hackflight sensors pidControllers

  -- Use the mixer to convert the demands into motor values
  let m1 = getMotor1 mixer demands
  let m2 = getMotor2 mixer demands
  let m3 = getMotor3 mixer demands
  let m4 = getMotor4 mixer demands

  -- Send the motor values to the external C function
  trigger "runMotors" true [arg m1, arg m2, arg m3, arg m4]

-- Compile the spec
main = reify spec >>= compile "hackflight"
