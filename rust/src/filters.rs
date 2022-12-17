/*
   Filters module

   Copyright (C) 2022 Simon D. Levy

   MIT License
*/


pub mod filters {

    use std::f32::consts::PI;

    #[derive(Clone,Copy)]
    pub struct Pt1 {

        state: f32,
        k: f32
    }

    pub fn applyPt1(filter: Pt1, input: f32) -> (f32, Pt1) {

        let state = filter.state + filter.k * (input - filter.state);

        (state, Pt1 {state: state, k: filter.k})
    }

    pub fn makePt1(f_cut: f32, dt: f32) -> Pt1 {

        let rc = 1.0 / (2.0 * PI * f_cut);
        let k = dt / (rc + dt);

        Pt1 {state: 0.0, k: k }

    }

    fn computeGainPt1(filter: Pt1, f_cut: f32, dt: f32) -> f32 {

        let rc = 1.0 / (2.0 * PI * f_cut);

        dt / (rc + dt)
    }

    #[derive(Clone,Copy)]
    pub struct Pt2 {

        state: f32,
        state1: f32,
        k: f32
    }

    pub fn applyPt2(filter: Pt2, input: f32) -> (f32, Pt2) {

        let state1 = filter.state1 + filter.k * (input - filter.state1);
        let state = filter.state + filter.k * (state1 - filter.state);

        (state, Pt2 {state: state, state1: state1, k: filter.k})
    }

    pub fn makePt2(f_cut: f32, dt: f32) -> Pt2 {

        let order: f32 = 2.0;
        let two: f32 = 2.0;
        let order_cutoff_correction = 1.0 / (two.powf(1.0 / order) - 1.0).sqrt();
        let rc = 1.0 / (2.0 * order_cutoff_correction * PI * f_cut);
        let k = dt / (rc + dt);

        Pt2 {state: 0.0, state1: 0.0, k: k }
    }

} // pub mod filters
