/*
   Filters module

   Copyright (C) 2022 Simon D. Levy

   MIT License
 */

use std::f32::consts::PI;

// Pt1 --------------------------------------------------------------------

use crate::utils::DT;

#[derive(Clone,Copy)]
pub struct Pt1 {

    state: f32,
    k: f32
}

pub fn apply_pt1(mut filter: Pt1, input: f32) -> f32 {

    filter.state = filter.state + filter.k * (input - filter.state);

    filter.state
}

pub fn make_pt1(f_cut: f32) -> Pt1 {

    let k = compute_pt1_gain(f_cut);

    Pt1 {state: 0.0, k: k }
}

pub fn adjust_pt1_gain(mut filter: Pt1, f_cut: f32)
{
    filter.k = compute_pt1_gain(f_cut);
}


fn compute_pt1_gain(f_cut:f32) -> f32 {

    let rc = 1.0 / (2.0 * PI * f_cut);

    DT / (rc + DT)
}

// Pt2 --------------------------------------------------------------------

#[derive(Clone,Copy)]
pub struct Pt2 {

    state: f32,
    state1: f32,
    k: f32
}

pub fn apply_pt2(mut filter: Pt2, input: f32) -> f32 {

    let state1 = filter.state1 + filter.k * (input - filter.state1);

    filter.state = filter.state + filter.k * (state1 - filter.state);

    filter.state
}

pub fn make_pt2(f_cut: f32) -> Pt2 {

    let k = compute_pt2_gain(f_cut);

    Pt2 {state: 0.0, state1: 0.0, k: k }
}

fn compute_pt2_gain(f_cut: f32) -> f32 {

    let order: f32 = 2.0;
    let two: f32 = 2.0;
    let order_cutoff_correction = 1.0 / (two.powf(1.0 / order) - 1.0).sqrt();
    let rc = 1.0 / (2.0 * order_cutoff_correction * PI * f_cut);

    DT / (rc + DT)
}


// Pt3 --------------------------------------------------------------------

#[derive(Clone,Copy)]
pub struct Pt3 {

    state: f32,
    state1: f32,
    state2: f32,
    k: f32
}

pub fn apply_pt3(mut filter: Pt3, input: f32) -> f32 {

    let state1 = filter.state1 + filter.k * (input - filter.state1);
    let state2 = filter.state2 + filter.k * (state1 - filter.state2);

    filter.state = filter.state + filter.k * (state2 - filter.state);

    filter.state
}

pub fn make_pt3(f_cut: f32) -> Pt3 {

    let k = compute_pt3_gain(f_cut);

    Pt3 {state: 0.0, state1: 0.0, state2: 0.0, k: k }
}


fn compute_pt3_gain(f_cut: f32) -> f32 {

    let order: f32 = 3.0;
    let two: f32 = 2.0;
    let order_cutoff_correction = 1.0 / (two.powf(1.0 / order) - 1.0).sqrt();
    let rc = 1.0 / (2.0 * order_cutoff_correction * PI * f_cut);

    DT / (rc + DT)
}


