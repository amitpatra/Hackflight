{--
  Hackflight core algorithms

  Copyright(C) 2021 on D.Levy

  MIT License
--}

{-# LANGUAGE RebindableSyntax #-}

module Hackflight where

import Language.Copilot

import Receiver
import State
import Sensor
import Demands
import PidController
import Safety
import Time
import Mixers
import Parser
import Serial
import Utils

-------------------------------------------------------------------------------

hackflight :: Receiver -> [Sensor] -> [PidFun] -> (State -> State) -> Mixer -> (SFloat -> SFloat)
  -> (Demands, State, Demands, Motors)

hackflight receiver sensors pidfuns statefun mixer mixfun
  = (rdemands, vstate, pdemands, motors)

  where

    -- Get receiver demands from external C functions
    rdemands = getDemands receiver

    -- Get the vehicle state by composing the sensor functions over the current state
    vstate = compose sensors (statefun vstate)

    -- Periodically get the demands by composing the PID controllers over the receiver
    -- demands
    (_, _, pdemands) = compose pidfuns (vstate, timerReady 300, rdemands)

    -- Run mixer on demands to get motor values
    motors = mixer mixfun pdemands

-------------------------------------------------------------------------------

hackflightFull :: Receiver -> [Sensor] -> [PidFun] -> Mixer
  -> (MessageBuffer, Motors, SBool)

hackflightFull receiver sensors pidfuns mixer
  = (msgbuff, motors, led)

  where

    -- Check safety (arming / failsafe)
    (armed, failsafe, mzero) = safety rdemands vstate

    -- Disables motors on failsafe, throttle-down, ...
    safemix = \m -> if mzero then 0 else m

    -- Run the core Hackflight algorithm
    (rdemands, vstate, pdemands, motors') = hackflight receiver
                                                       sensors
                                                       pidfuns
                                                       state'
                                                       mixer
                                                       safemix

    -- Blink LED during first couple of seconds; keep it solid when armed
    led = if micros < 2000000 then (mod (div micros 50000) 2 == 0) else armed

    -- Run the serial comms parser
    (msgtyp, sending, payindex, _checked) = parse stream_serialAvailable stream_serialByte

    -- Convert the message into a buffer to send to GCS
    msgbuff = message sending msgtyp vstate

    -- Check for incoming SET_MOTOR messages from GCS
    motor_index = if msgtyp == 215 && payindex == 1 then stream_serialByte
                  else motor_index' where motor_index' = [0] ++ motor_index
    motor_percent = if msgtyp == 215 && payindex == 2 then stream_serialByte
                    else motor_percent' where motor_percent' = [0] ++ motor_percent

    motorfun armed flying_value index target percent =
      if armed then flying_value
      else if index == target then (unsafeCast percent) / 100
      else 0

    -- Set motors based on arming state and whether we have GCS input
    -- XXX Should work for more than quad
    m1_val = motorfun armed (m1 motors') motor_index 1 motor_percent
    m2_val = motorfun armed (m2 motors') motor_index 2 motor_percent
    m3_val = motorfun armed (m3 motors') motor_index 3 motor_percent
    m4_val = motorfun armed (m4 motors') motor_index 4 motor_percent

    motors = QuadMotors m1_val m2_val m3_val m4_val

-------------------------------------------------------------------------------

hackflightSim :: Receiver -> [Sensor] -> [PidFun] -> Mixer -> Motors

hackflightSim receiver sensors pidfuns mixer = motors

  where

    (_, _, _, motors) = hackflight receiver sensors pidfuns statefun mixer mixfun

    mixfun = \m -> constrain m

    statefun = \_ -> State 0 0 0 0 0 0 0 0 0 0 0 0
