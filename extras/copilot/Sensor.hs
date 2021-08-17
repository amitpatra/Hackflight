{--
  Haskell Copilot support for sensors

  Copyright(C) 2021 Simon D.Levy

  MIT License
--}

{-# LANGUAGE DataKinds        #-}

module Sensor

where

import Language.Copilot

import VehicleState

type Sensor = VehicleState -> VehicleState
