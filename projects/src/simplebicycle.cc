#include "simplebicycle.h"

#include <boost/math/tools/tuple.hpp>
#include <boost/math/tools/roots.hpp>
#include "constants.h"
#include "parameters.h"

namespace model {

SimpleBicycle::SimpleBicycle(real_t v, real_t dt) :
        m_M(parameters::benchmark::M),
        m_C1(parameters::benchmark::C1),
        m_K0(parameters::benchmark::K0),
        m_K2(parameters::benchmark::K2),
        m_pose(BicyclePoseMessage_init_zero),
        m_v(0.0),
        m_dt(dt),
        m_T_m(0.0),
        m_w(parameters::benchmark::wheelbase),
        m_c(parameters::benchmark::trail),
        m_lambda(parameters::benchmark::steer_axis_tilt),
        m_rr(parameters::benchmark::rear_wheel_radius),
        m_rf(parameters::benchmark::front_wheel_radius) {
    set_moore_parameters();
    set_v(v);
    reset_pose();
}

void SimpleBicycle::set_v(real_t v) {
    m_v = v;
    m_C = v*m_C1;
    m_K = constants::g*m_K0 + v*v*m_K2;
}

void SimpleBicycle::set_dt(real_t dt) {
    m_dt = dt;
}

void SimpleBicycle::reset_pose() {
    m_x.setZero();
    m_x_aux.setZero();
    m_T_m = 0.0;
    m_pose = BicyclePoseMessage{}; /* reset pose to zero */

    m_x_aux[auxiliary_state_index_t::pitch_angle] =
        solve_constraint_pitch(m_x, m_x_aux[auxiliary_state_index_t::pitch_angle]);
    m_pose.pitch = m_x_aux[auxiliary_state_index_t::pitch_angle];
}

void SimpleBicycle::update(real_t roll_torque_input, real_t steer_torque_input,
    real_t yaw_angle_measurement, real_t steer_angle_measurement) {
    (void)roll_torque_input;
    (void)steer_torque_input;
    (void)yaw_angle_measurement;

    update_state(steer_angle_measurement);
    m_x_aux = integrate_auxiliary_state(m_x, m_x_aux);
    m_x_aux[auxiliary_state_index_t::pitch_angle] =
        solve_constraint_pitch(m_x, m_x_aux[auxiliary_state_index_t::pitch_angle]);
    update_feedback_torque();

    set_pose();
}

void SimpleBicycle::set_moore_parameters() {
    m_d1 = std::cos(m_lambda)*(m_c + m_w - m_rr*std::tan(m_lambda));
    m_d3 = -std::cos(m_lambda)*(m_c - m_rf*std::tan(m_lambda));
    m_d2 = (m_rr + m_d1*std::sin(m_lambda) - m_rf + m_d3*std::sin(m_lambda)) / std::cos(m_lambda);
}

void SimpleBicycle::set_pose() {
    m_pose.timestamp += static_cast<decltype(m_pose.timestamp)>(m_dt * 1000); /* timestamp in milliseconds */
    m_pose.x = m_x_aux[auxiliary_state_index_t::x];
    m_pose.y = m_x_aux[auxiliary_state_index_t::y];
    m_pose.pitch = m_x_aux[auxiliary_state_index_t::pitch_angle];
    m_pose.yaw = m_x_aux[auxiliary_state_index_t::yaw_angle];
    m_pose.roll = m_x[state_index_t::roll_angle];
    m_pose.steer = m_x[state_index_t::steer_angle];
}

/*
 * Simplified equations of motion are used to simulate the bicycle dynamics.
 * Roll/steer rate and acceleration terms are ignored resulting in:
 *          (g*K0 + v^2*K2) [phi  ] = [T_phi  ]
 *                          [delta] = [T_delta]
 *
 */

void SimpleBicycle::update_state(real_t steer_angle_measurement) {
    real_t next_roll = -m_K(0, 1)/m_K(0, 0) * steer_angle_measurement;

    m_x[state_index_t::steer_rate] = (steer_angle_measurement - m_x[state_index_t::steer_angle])/m_dt;
    m_x[state_index_t::roll_rate] = (next_roll - m_x[state_index_t::roll_angle])/m_dt;
    m_x[state_index_t::steer_angle] = steer_angle_measurement;
    m_x[state_index_t::roll_angle] = next_roll;
}

void SimpleBicycle::update_feedback_torque() {
    m_T_m = -(m_K(1, 1) - m_K(0, 1)*m_K(1, 0)/m_K(0, 0))*m_x[state_index_t::steer_angle];
}

SimpleBicycle::auxiliary_state_t SimpleBicycle::integrate_auxiliary_state(const state_t& x, const auxiliary_state_t& x_aux) {
    auxiliary_state_t x_out = x_aux;
    real_t yaw_rate = (m_v*x[state_index_t::steer_angle]+ m_c*x[state_index_t::steer_rate])/m_w*cos(m_lambda);
    real_t v = m_v;

    m_auxiliary_stepper.do_step([v, yaw_rate](
                const auxiliary_state_t& x, auxiliary_state_t& dxdt, const real_t t) -> void {
                (void)t;
                dxdt[auxiliary_state_index_t::x] = v*std::cos(x[auxiliary_state_index_t::yaw_angle]); // xdot = v*cos(psi)
                dxdt[auxiliary_state_index_t::y] = v*std::sin(x[auxiliary_state_index_t::yaw_angle]); // ydot = v*sin(psi)
                dxdt[auxiliary_state_index_t::pitch_angle] = 0.0; // pitch is calculated separately
                dxdt[auxiliary_state_index_t::yaw_angle] = yaw_rate; // yawdot = (v*delta + c*deltadot)/w*cos(lambda)
            }, x_out, 0.0, m_dt);

    return x_out;
}

real_t SimpleBicycle::solve_constraint_pitch(const state_t& x, real_t guess) const {
    // constraint function generated by script 'generate_pitch.py'.
    static constexpr int digits = std::numeric_limits<real_t>::digits*2/3;
    static constexpr real_t two = static_cast<real_t>(2.0);
    static constexpr real_t one_point_five = static_cast<real_t>(1.5);
    static const real_t min = -constants::pi/2;
    static const real_t max = constants::pi/2;
    auto constraint_function = [this, x](real_t pitch)->boost::math::tuple<real_t, real_t> {
        return boost::math::make_tuple(
((m_rf*std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two) +
(m_d3*std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two)) +
m_rf*(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1])))*(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1])))*std::sqrt(std::pow(std::cos(x[0]), two)) +
std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two))*(-m_d1*std::sqrt(std::pow(std::cos(x[0]),
two))*std::sin(pitch) + m_d2*std::sqrt(std::pow(std::cos(x[0]), two))*std::cos(pitch) -
m_rr*std::cos(x[0]))*std::cos(x[0]))/(std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1]), two) + std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]),
two))*std::sqrt(std::pow(std::cos(x[0]), two)))
                ,
((m_rf*std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two) +
(m_d3*std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two)) +
m_rf*(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1])))*(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1])))*std::sqrt(std::pow(std::cos(x[0]), two)) +
std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two))*(-m_d1*std::sqrt(std::pow(std::cos(x[0]),
two))*std::sin(pitch) + m_d2*std::sqrt(std::pow(std::cos(x[0]), two))*std::cos(pitch) -
m_rr*std::cos(x[0]))*std::cos(x[0]))*((-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1]))*std::cos(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(pitch)*std::cos(pitch)*std::pow(std::cos(x[0]),
two))/(std::pow(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two), one_point_five)*std::sqrt(std::pow(std::cos(x[0]), two))) +
((-m_d1*std::sqrt(std::pow(std::cos(x[0]), two))*std::cos(pitch) - m_d2*std::sqrt(std::pow(std::cos(x[0]),
two))*std::sin(pitch))*std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1]), two) + std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two))*std::cos(x[0]) +
(-(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1]))*std::cos(pitch)*std::cos(x[0])*std::cos(x[1]) -
std::sin(pitch)*std::cos(pitch)*std::pow(std::cos(x[0]), two))*(-m_d1*std::sqrt(std::pow(std::cos(x[0]),
two))*std::sin(pitch) + m_d2*std::sqrt(std::pow(std::cos(x[0]), two))*std::cos(pitch) -
m_rr*std::cos(x[0]))*std::cos(x[0])/std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1]), two) + std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two)) +
(-2*m_rf*std::sin(pitch)*std::cos(pitch)*std::pow(std::cos(x[0]), two) -
(m_d3*std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two)) +
m_rf*(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1])))*std::cos(pitch)*std::cos(x[0])*std::cos(x[1]) +
(m_d3*(-(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1]))*std::cos(pitch)*std::cos(x[0])*std::cos(x[1]) -
std::sin(pitch)*std::cos(pitch)*std::pow(std::cos(x[0]),
two))/std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two)) -
m_rf*std::cos(pitch)*std::cos(x[0])*std::cos(x[1]))*(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) +
std::sin(x[0])*std::sin(x[1])))*std::sqrt(std::pow(std::cos(x[0]),
two)))/(std::sqrt(std::pow(-std::sin(pitch)*std::cos(x[0])*std::cos(x[1]) + std::sin(x[0])*std::sin(x[1]), two) +
std::pow(std::cos(pitch), two)*std::pow(std::cos(x[0]), two))*std::sqrt(std::pow(std::cos(x[0]), two)))
                );
    };
    return boost::math::tools::newton_raphson_iterate(constraint_function, guess, min, max, digits);
}

} // namespace model