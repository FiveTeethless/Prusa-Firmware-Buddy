/**
 * @file
 * @author: Marek Bel
 *
 * This code is based on Klipper project.
 * Author: Dmitry Butyugin <dmbutyugin@google.com>
 * Source: https://github.com/Klipper3d/klipper/blob/master/klippy/extras/shaper_calibrate.py
 */

#include "../../inc/MarlinConfig.h"
#include "../gcode.h"
#include "../../module/planner.h"
#include "../../Marlin.h"
#include "../../module/stepper.h"
#include "../../module/prusa/accelerometer.h"
#include "../../feature/precise_stepping/precise_stepping.h"
#include "../../feature/input_shaper/input_shaper.h"
#include "metric.h"
#include <cmath>
#include <numbers>
#include <limits>
#include <bit>

//#define M958_OUTPUT_SAMPLES
//#define M958_VERBOSE
#ifdef M958_OUTPUT_SAMPLES
  #include "../../../../../tinyusb/src/class/cdc/cdc_device.h"
#endif

static metric_t metric_excite_freq = METRIC("excite_freq", METRIC_VALUE_FLOAT, 100, METRIC_HANDLER_ENABLE_ALL);
#if ENABLED(ACCELEROMETER)
  static metric_t accel = METRIC("tk_accel", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_ENABLE_ALL);
#endif

namespace {
class HarmonicGenerator {
    /**
     * @brief Displacement amplitude
     *
     * double integral of acceleration over time results in position amplitude
     *
     * @param acceleration in m/s-2
     * @param frequency in Hz
     * @return amplitude in meters
     */
    static float amplitudeNotRounded(float frequency, float acceleration) {
        return acceleration / ( 4 * std::numbers::pi_v<float> * std::numbers::pi_v<float> * frequency * frequency);
    }
    static int amplitudeRoundToSteps(float amplitude_not_rounded, float step_len) {
        return ceil(amplitude_not_rounded / step_len);
    }

public:
    HarmonicGenerator(float frequency, float acceleration, float step_len) :
        m_amplitude_steps(amplitudeRoundToSteps(amplitudeNotRounded(frequency, acceleration), step_len)),
        m_step(step_len),
        m_freq2pi_inv(1.f / (frequency * 2 * std::numbers::pi_v<float>)),
        m_last_time(1.f / (frequency * 4.f)),
        m_last_step(m_amplitude_steps - 1),
        m_dir_forward(false) {}

  float nextDelayDir() {
    float new_time = asinf(static_cast<float>(m_last_step) / m_amplitude_steps) * m_freq2pi_inv;

    if (m_dir_forward) {
    if (m_last_step < m_amplitude_steps) {
      ++m_last_step;
    }
    else {
      --m_last_step;
      m_dir_forward = false;
    }
    }
    else {
    if (m_last_step > -m_amplitude_steps) {
      --m_last_step;
    }
    else {
      ++m_last_step;
      m_dir_forward = true;
    }
    }

    float next_delay = new_time - m_last_time;
    m_last_time = new_time;
    return next_delay;
  }

  int getStepsPerPeriod() {
      return (m_amplitude_steps * 4);
  }

  float getFrequency() {
    float period = .0f;
    for(int i = 0; i < getStepsPerPeriod(); ++i) {
      period += abs(nextDelayDir());
    }
    return 1.f / period;
  }

  float getAcceleration(float frequency) {
    return m_amplitude_steps * m_step * 4.f * std::numbers::pi_v<float> * std::numbers::pi_v<float> * frequency * frequency;
  }

private:
const int m_amplitude_steps; ///< amplitude rounded to steps
const float m_step;
const float m_freq2pi_inv;
float m_last_time;
int m_last_step;
bool m_dir_forward;

};


/**
 * Turns automatic reports off until destructor is called.
 * Then it sets reports to previous value.
 */
class Temporary_Report_Off{
    bool suspend_reports = false;
  public:
    Temporary_Report_Off(){
      suspend_reports = suspend_auto_report;
      suspend_auto_report = true;
    }
    ~Temporary_Report_Off(){
      suspend_auto_report = suspend_reports;
    }
};

class StepDir {
public:
  static constexpr float m_ticks_per_second = STEPPER_TIMER_RATE;
  struct RetVal {
    int step_us;
    bool dir;
  };
  StepDir(HarmonicGenerator &generator) :
    m_generator(generator),
    m_step_us_fraction(.0f)
  {}
  RetVal get() {
   RetVal retval(0);
   const float next_delay_dir = m_generator.nextDelayDir();
   retval.dir = signbit(next_delay_dir);

   const float next_delay = abs(next_delay_dir);
   const float next_delay_us = next_delay * m_ticks_per_second + m_step_us_fraction;
   retval.step_us = next_delay_us;
   m_step_us_fraction = next_delay_us - retval.step_us;

   return retval;
  }
private:
  HarmonicGenerator &m_generator;
  float m_step_us_fraction;
};

struct FrequencyGain {
    float frequency;
    float gain;
};

struct FrequencyGain3D {
    float frequency;
    float gain[3];
};

class MicrostepRestorer {
public:
  MicrostepRestorer()
    : m_x_mres(stepperX.microsteps())
    , m_y_mres(stepperY.microsteps()) {}

  ~MicrostepRestorer() {
    while(has_steps()) {
        idle(true, true);
    }
    stepperX.microsteps(m_x_mres);
    stepperY.microsteps(m_y_mres);
    }
private:
  bool has_steps() {
      CRITICAL_SECTION_START;
      bool retval =  PreciseStepping::has_step_events_queued();
      CRITICAL_SECTION_END;
      return retval;
  }
  uint16_t m_x_mres;
  uint16_t m_y_mres;
};

template <size_t max_samples>
class Spectrum {
public:
  Spectrum(float start_frequency, float frequency_step)
    : m_gain()
    , m_start_frequency(start_frequency)
    , m_frequency_step(frequency_step)
    , m_size(0)
    {}

  constexpr size_t max_size() const {return max_samples;}

  size_t size() const {return m_size;}

  void put(float gain){
    if(m_size >= max_samples) {
      return;
    }
    m_gain[m_size] = gain;
    ++m_size;
  }
  FrequencyGain get(size_t index) const {
    FrequencyGain retval = {0.f, 0.f};
    if(index < m_size) {
      retval.frequency = m_start_frequency + index * m_frequency_step;
      retval.gain = m_gain[index];
    }
    return retval;
  }
  float max() const {
    float maximum = std::numeric_limits<float>::min();
    for (size_t i = 0; i < m_size; ++i) {
      if(m_gain[i] > maximum) {
        maximum = m_gain[i];
      }
    }
    return maximum;
  }
private:
  float m_gain[max_samples];
  float m_start_frequency;
  float m_frequency_step;
  size_t m_size;
};

/// Fixed length spectrum
using Fl_Spectrum = Spectrum<146>;
} // anonymous namespace

static bool is_full() {
  CRITICAL_SECTION_START;
  bool retval =  PreciseStepping::is_step_event_queue_full();
  CRITICAL_SECTION_END;
  return retval;
}

static void enqueue_step(int step_us, bool dir, StepEventFlag_t axis_flags){
  uint16_t next_queue_head = 0;

  CRITICAL_SECTION_START;
  step_event_t *step_event = PreciseStepping::get_next_free_step_event(next_queue_head);
  step_event->time_ticks = step_us;
  step_event->flags = axis_flags;
  if(dir) step_event->flags ^= STEP_EVENT_FLAG_DIR_MASK;
  PreciseStepping::step_event_queue.head = next_queue_head;
  CRITICAL_SECTION_END;
}

struct Acumulator {
    double val[3][2];
};

#if ENABLED(ACCELEROMETER)
/**
 * @brief Get recommended damping ratio for zv input shaper
 *
 * This is probably not right. This computation is based on assumption filter should damp
 * system resonant gain to 1. But from the theory of input shaper comes requirement that
 * resonant frequency of the system should be either damped to zero or the oscillation
 * excited should be immediately cancelled out.
 *
 * zv shaper gain computed as
 * https://www.wolframalpha.com/input?i=g%3D50%2C+f%3D50%2C+d%3D0.1%2C+%28sin%28x*2pi*g%29%2Be%5E%28-d*pi%2Fsqrt%281-d%5E2%29%29*sin%28%28x%2B1%2F%282*f*sqrt%281-d%5E2%29%29%29*2pi*g%29%29%2F%281%2Be%5E%28-d*pi%2Fsqrt%281-d%5E2%29%29%29
 * where g .. frequency probed[Hz], f .. shaper maximum atenuation frequency[Hz], d .. SHAPER_DAMPING_RATIO[-]
 * It computes output signal of the filter for input signal is sine wave with amplitude 1.
 * So if output amplitude (either visible from graph or computed from Reduced trigonometric form) is 0.15,
 * it means filter gain at that frequency is 0.15.
 *
 * Pre-computed values were tabulated
 *
 * | shaper gain       | damping ratio  |
 * | ----------------- | -------------- |
 * | 0.0157076902629   | 0.01           |
 * | 0.0785009         | 0.05           |
 * | 0.15676713        | 0.1            |
 * | 0.234556          | 0.15           |
 * | 0.311608          | 0.2            |
 * | 0.738780338281116 | 0.5            |
 *
 * and approximated by quadratic function.
 *
 * @param resonant_gain gain of resonator to be damped to 1
 * @return recommended zv shaper damping_ratio
 *
 */
static float get_zv_shaper_damping_ratio(float resonant_gain) {
    float shaper_gain =  1.f / resonant_gain;
    return 0.080145136132399f * sq(shaper_gain) + 0.616396503538947f * shaper_gain + 0.000807776046666f;
}
#endif

/**
 * @brief Excite harmonic vibration and measure amplitude if there is an accelerometer
 *
 * @see GcodeSuite::M958() for parameter description
 *
 * @param axis_flag StepEventFlag bit field
 * STEP_EVENT_FLAG_STEP_* is set for all the motors which should vibrate together
 * STEP_EVENT_FLAG_*_DIR encodes initial phase for each motor
 * @param klipper_mode
 * @param frequency_requested
 * @param acceleration_requested
 * @param cycles
 * @param calibrate_accelerometer
 * @return Frequency and gain measured on each axis if there is accelerometer
 */
static
#if ENABLED(ACCELEROMETER)
FrequencyGain3D
#else
void
#endif
vibrate_measure(StepEventFlag_t axis_flag, bool klipper_mode, float frequency_requested, float acceleration_requested, float step_len, uint32_t cycles, bool calibrate_accelerometer ) {
  HarmonicGenerator generator(frequency_requested, acceleration_requested, step_len);
  const float frequency = generator.getFrequency();
  StepDir stepDir(generator);

#if ENABLED(ACCELEROMETER)
  const float acceleration = generator.getAcceleration(frequency);
  PrusaAccelerometer accelerometer;

  Acumulator acumulator = {};
  const float freq_2pi = std::numbers::pi_v<float> * frequency * 2.f;
  const float period = 1 / frequency;
  float accelerometer_period_time = 0.f;
  static float sample_period = 1.f / 1344.f;

  if (calibrate_accelerometer) {
    for (int i = 0; i < 96 ; ++i) {
      idle(true, true);
      accelerometer.clear();
    }
    const uint32_t start_time = millis();
    constexpr int request_samples_num = 20'000;

    for (int i = 0; i < request_samples_num; ) {
      PrusaAccelerometer::Acceleration measured_acceleration;
      const int samples = accelerometer.get_sample(measured_acceleration);
      if(samples) {
        ++i;
      } else {
        idle(true, true);
      }
    }

    const uint32_t now = millis();
    const uint32_t duration_ms = now - start_time;
    sample_period = duration_ms / 1000.f / static_cast<float>(request_samples_num);
    SERIAL_ECHOLNPAIR_F("Sample freq: ", 1.f / sample_period);
    if(klipper_mode) {
      SERIAL_ECHOLNPGM("freq,psd_x,psd_y,psd_z,psd_xyz,mzv");
    } else {
      SERIAL_ECHOLNPGM("frequency[Hz] excitation[m/s^2] X[m/s^2] Y[m/s^2] Z[m/s^2] X_gain Y_gain Z_gain");
    }
  }
  uint32_t sample_nr = 0;
  const uint32_t samples_to_collect = period * cycles / sample_period;
  bool enough_samples_collected = false;
  bool first_loop = true;
#else
  constexpr bool enough_samples_collected = true;
#endif
  Temporary_Report_Off stop_busy_messages;
#ifdef M958_OUTPUT_SAMPLES
  SERIAL_ECHOLN("Yraw  sinf cosf");
#endif

#if ENABLED(ACCELEROMETER)
  constexpr int num_axis = sizeof(PrusaAccelerometer::Acceleration::val) / sizeof(PrusaAccelerometer::Acceleration::val[0]);
  constexpr int cplx_indexes = 2;
#endif

  uint32_t step_nr = 0;
  GcodeSuite::reset_stepper_timeout();
  const uint32_t steps_to_do = generator.getStepsPerPeriod() * cycles;

  while ((step_nr < steps_to_do) || (!enough_samples_collected) || (step_nr % generator.getStepsPerPeriod() != 0))
  {
    StepDir::RetVal step_dir = stepDir.get();

    while (is_full()) {
      #if ENABLED(ACCELEROMETER)
        if(first_loop) {
          accelerometer.clear();
          first_loop = false;
        }
        PrusaAccelerometer::Acceleration measured_acceleration;
        const int samples = accelerometer.get_sample(measured_acceleration);
        if (samples && !enough_samples_collected && (step_nr > STEP_EVENT_QUEUE_SIZE)) {
          metric_record_custom(&accel, " x=%.4f,y=%.4f,z=%.4f", (double)measured_acceleration.val[0], (double)measured_acceleration.val[1], (double)measured_acceleration.val[2]);
          const float accelerometer_time_2pi_freq = freq_2pi * accelerometer_period_time;
          const float amplitude[cplx_indexes] = {sinf(accelerometer_time_2pi_freq), cosf(accelerometer_time_2pi_freq)};


          for(int axis = 0; axis < num_axis; ++axis) {
            for(int i = 0; i < cplx_indexes; ++i) {
                acumulator.val[axis][i] += amplitude[i] * measured_acceleration.val[axis];
            }
          }

          ++sample_nr;
          enough_samples_collected = sample_nr >= samples_to_collect;
          accelerometer_period_time += sample_period;
          if (accelerometer_period_time > period) {
              accelerometer_period_time -= period;
          }
        #ifdef M958_OUTPUT_SAMPLES
          char buff[40];
          snprintf(buff, 40, "%f %f %f\n", static_cast<double>(measured_acceleration.val[1]),  static_cast<double>(amplitude[0]), static_cast<double>(amplitude[1]));
          tud_cdc_n_write_str(0, buff);
          tud_cdc_write_flush();
        #endif
        }
      #else
        constexpr bool samples = false;
      #endif
      metric_record_float(&metric_excite_freq, frequency);

      if(!samples) {
        idle(true, true);
      }
    }

    enqueue_step(step_dir.step_us, step_dir.dir, axis_flag);
    ++step_nr;
  }

#if ENABLED(ACCELEROMETER)
  for(int axis = 0; axis < num_axis; ++axis) {
    for(int i = 0; i < cplx_indexes; ++i) {
        acumulator.val[axis][i] *= 2.;
        acumulator.val[axis][i] /= (sample_nr + 1);
    }
  }

  const float x_acceleration_amplitude = sqrt(sq(acumulator.val[0][0]) + sq(acumulator.val[0][1]));
  const float y_acceleration_amplitude = sqrt(sq(acumulator.val[1][0]) + sq(acumulator.val[1][1]));
  const float z_acceleration_amplitude = sqrt(sq(acumulator.val[2][0]) + sq(acumulator.val[2][1]));
  const float x_gain = x_acceleration_amplitude / acceleration;
  const float y_gain = y_acceleration_amplitude / acceleration;
  const float z_gain = z_acceleration_amplitude / acceleration;


  #ifdef M958_VERBOSE
    SERIAL_ECHO_START();
    SERIAL_ECHOPAIR_F("frequency ", frequency);
    SERIAL_ECHOPAIR_F(" Msampl ",(sample_nr + 1));
    SERIAL_ECHOPAIR_F(" Xsin ", acumulator.val[0][0], 5);
    SERIAL_ECHOPAIR_F(" Xcos ", acumulator.val[0][1], 5);
    SERIAL_ECHOPAIR_F(" Ysin ", acumulator.val[1][0], 5);
    SERIAL_ECHOPAIR_F(" Ycos ", acumulator.val[1][1], 5);
    SERIAL_ECHOPAIR_F(" Zsin ", acumulator.val[2][0], 5);
    SERIAL_ECHOPAIR_F(" Zcos ", acumulator.val[2][1], 5);
    SERIAL_ECHOPAIR_F(" X ", x_acceleration_amplitude, 5);
    SERIAL_ECHOPAIR_F(" Y ", y_acceleration_amplitude, 5);
    SERIAL_ECHOLNPAIR_F(" Z ", z_acceleration_amplitude, 5);
  #else
    SERIAL_ECHO(frequency);
    if(klipper_mode) {
      SERIAL_ECHOPAIR_F(",", sq(x_gain), 5);
      SERIAL_ECHOPAIR_F(",", sq(y_gain), 5);
      SERIAL_ECHOPAIR_F(",", sq(z_gain), 5);
      SERIAL_ECHOLNPAIR_F(",", sq(x_gain) + sq(y_gain) + sq(z_gain), 5);
    } else {
      SERIAL_ECHOPAIR_F(" ", acceleration);
      SERIAL_ECHOPAIR_F(" ", x_acceleration_amplitude, 5);
      SERIAL_ECHOPAIR_F(" ", y_acceleration_amplitude, 5);
      SERIAL_ECHOPAIR_F(" ", z_acceleration_amplitude, 5);
      SERIAL_ECHOPAIR_F(" ", x_gain, 5);
      SERIAL_ECHOPAIR_F(" ", y_gain, 5);
      SERIAL_ECHOLNPAIR_F(" ", z_gain, 5);
    }
  #endif
  FrequencyGain3D retval = {frequency, x_gain, y_gain, z_gain};
  return retval;
#endif
}

/**
 * @brief Parse motor codes, directions and enable motors accordingly
 *
 * @return step and direction flags - see StepEventFlag
 */
static StepEventFlag_t setup_axis() {
  StepEventFlag_t axis_flag = 0;
  enable_XY();
  if (parser.seen('X')) {
    stepper.microstep_mode(0,128);
    axis_flag |= StepEventFlag::STEP_EVENT_FLAG_STEP_X;
    if (parser.seenval('X') && (-1 == (parser.value_long())) ) {
      axis_flag |= StepEventFlag::STEP_EVENT_FLAG_X_DIR;
    }
  }
  if (parser.seen('Y')) {
    stepper.microstep_mode(1,128);
    axis_flag |= StepEventFlag::STEP_EVENT_FLAG_STEP_Y;
    if (parser.seenval('Y') && (-1 == (parser.value_long())) ) {
      axis_flag |= StepEventFlag::STEP_EVENT_FLAG_Y_DIR;
    }
  }
  if (0 == axis_flag) axis_flag = StepEventFlag::STEP_EVENT_FLAG_STEP_X;
  return axis_flag;
}

/**
 * @brief Get current step length
 *
 * Compute step length based on kinematic type, default steps per mm, default microstep resolution
 * current microstep resolution and number of active motors.
 *
 * @param axis_flag All active motors when generating harmonic vibrations
 * @return step length in meters
 */
static float get_step_len(StepEventFlag_t axis_flag) {
  constexpr float meters_in_mm = 0.001f;
  constexpr float default_steps_per_mm[] = DEFAULT_AXIS_STEPS_PER_UNIT;
  static_assert (default_steps_per_mm[0] == default_steps_per_mm[1], "Same steps per unit expected in both axes.");
  #ifndef X_MICROSTEPS
    #error "X_MICROSTEPS not defined"
  #endif
  static_assert (X_MICROSTEPS == Y_MICROSTEPS, "Same resolution expected in both axes.");
  constexpr float default_microsteps = X_MICROSTEPS;
  constexpr float default_step_len = (1.f / default_steps_per_mm[0]) * meters_in_mm; // in meters

  const unsigned num_motors = std::popcount(axis_flag & STEP_EVENT_FLAG_AXIS_MASK);

  const float current_microsteps = (axis_flag & STEP_EVENT_FLAG_STEP_X) ? stepperX.microsteps() : stepperY.microsteps();

  #if IS_CARTESIAN
    #if IS_CORE
      #if CORE_IS_XY
        switch (num_motors) {
        case 1:
          return sqrt(2.f) / 2.f * default_step_len * default_microsteps / current_microsteps;
        case 2:
          return default_step_len * default_microsteps / current_microsteps;
        default:
          bsod("Impossible num_motors.");
        }
      #else
        #error "Not implemented."
      #endif
    #else
      switch (num_motors) {
      case 1:
        return default_step_len * default_microsteps / current_microsteps;
      case 2:
        return sqrt(2.f) * default_step_len * default_microsteps / current_microsteps;
      default:
        bsod("Impossible num_motors.");
      }
    #endif
  #else
    #error "Not implemented."
  #endif
}

struct LogicalAxis {
    bool is_x;
    bool is_y;
};


/**
 * @brief Get logical axis from motor axis_flag
 *
 * @param axis_flag motors and initial directions flags see StepEventFlag
 * @retval true for single logical axis if vibrations are generated aligned for that particular single axis only
 * @retval false for all logical axis if the move is not parallel to single logical axis - e.g. diagonal movement or no movement
 */
LogicalAxis get_logical_axis(const uint16_t axis_flag) {
  const bool x_flag = axis_flag & STEP_EVENT_FLAG_STEP_X;
  const bool y_flag = axis_flag & STEP_EVENT_FLAG_STEP_Y;
  LogicalAxis logicalAxis = {false, false};
  #if IS_CARTESIAN
    #if IS_CORE
      #if CORE_IS_XY
        if (x_flag == y_flag) {
          const bool x_dir = axis_flag & STEP_EVENT_FLAG_X_DIR;
          const bool y_dir = axis_flag & STEP_EVENT_FLAG_Y_DIR;
          if (x_dir == y_dir) {
            logicalAxis.is_x = true;
          } else {
            logicalAxis.is_y = true;
          }
        }
      #else
        #error "Not implemented."
      #endif
    #else
      if(x_flag != y_flag) {
        logicalAxis.is_x = x_flag;
        logicalAxis.is_y = y_flag;
      }
    #endif
  #else
    #error "Not implemented."
  #endif
  return logicalAxis;
}

/**
 * @brief Excite harmonic vibration
 *
 * - X<direction> Vibrate with X motor, start in direction 1 or -1
 * - Y<direction> Vibrate with Y motor, start in direction 1 or -1
 * - F<Hz>     Frequency
 * - A<mm/s-2> Acceleration
 * - N<cycles> Number of full periods at desired frequency.
 *             In case there is no accelerometer measurement,
 *             exact number of periods is generated,
 *             in case there is accelerometer, it is
 *             number of periods of active measurement and
 *             some extra cycles can be generated.
 * - C         Calibrate accelerometer sample rate
 * - K         Klipper compatible report
 */
void GcodeSuite::M958() {
  MicrostepRestorer microstepRestorer;
  const StepEventFlag_t axis_flag = setup_axis();
  const float step_len = get_step_len(axis_flag);

  const bool klipper_mode = parser.seen('K');

  float frequency_requested = 35.f;
  if (parser.seenval('F')) {
    frequency_requested = abs(parser.value_float());
  }

  float acceleration_requested = 2.5f;

  if (parser.seenval('A')) {
    acceleration_requested = abs(parser.value_float()) * 0.001f;
  }

  uint32_t cycles = 50;
  if (parser.seenval('N')) {
    cycles = parser.value_ulong();
  }

  bool calibrate_accelerometer = parser.seen('C');

  vibrate_measure(axis_flag, klipper_mode, frequency_requested, acceleration_requested, step_len, cycles, calibrate_accelerometer);
}

#if ENABLED(ACCELEROMETER)

static constexpr float epsilon = 0.01f;
static constexpr double default_vibration_reduction = 20;
static constexpr double default_damping_ratio = .1;

/**
 * @brief ZV shaper tune, naive approach
 *
 * Sadly damping ratio computation is very probably not right here. In theory we could identify damping ratio from gain
 * at resonant frequency if the system would be excited by sine wave force.
 * But in reality we are exciting the system by sine wave displacement. We for sure can not tell, if the force is still sine wave
 * and what is the force - the force depends on motor load angle and belt stiffness and we don't know it.
 */
static void naive_zv_tune(StepEventFlag_t axis_flag, float start_frequency, float end_frequency, float frequency_increment, float acceleration_requested, const float step_len, uint32_t cycles) {
  FrequencyGain maxFrequencyGain = {0.f, 0.f};
  const LogicalAxis logicalAxis = get_logical_axis(axis_flag);
  bool calibrate_accelerometer = true;
  for (float frequency_requested = start_frequency; frequency_requested <= end_frequency + epsilon; frequency_requested += frequency_increment) {
    FrequencyGain3D frequencyGain3D = vibrate_measure(axis_flag, false, frequency_requested, acceleration_requested, step_len, cycles, calibrate_accelerometer);
    FrequencyGain frequencyGain = {frequencyGain3D.frequency, logicalAxis.is_x ? frequencyGain3D.gain[0] : frequencyGain3D.gain[1]};
    calibrate_accelerometer = false;
    if(frequencyGain.gain > maxFrequencyGain.gain) {
      maxFrequencyGain = frequencyGain;
    }
  }
  SERIAL_ECHOPAIR_F("Maximum resonant gain: ", maxFrequencyGain.gain);
  SERIAL_ECHOLNPAIR_F(" at frequency: ", maxFrequencyGain.frequency);

  if(logicalAxis.is_x || logicalAxis.is_y) {
    const float damping_ratio = get_zv_shaper_damping_ratio(maxFrequencyGain.gain);
    SERIAL_ECHOLN("ZV shaper selected");
    SERIAL_ECHOPAIR_F("Frequency: ", maxFrequencyGain.frequency);
    SERIAL_ECHOLNPAIR_F(" damping ratio: ", damping_ratio, 5);
    input_shaper::set(logicalAxis.is_x, logicalAxis.is_y, damping_ratio, maxFrequencyGain.frequency, 0.f, input_shaper::Type::zv);
  }
}

static float limit_end_frequency(const float start_frequency, float end_frequency, const float frequency_increment, const size_t max_samples) {
  const size_t requested_samples = (end_frequency - start_frequency + epsilon) / frequency_increment + 1;
  if (requested_samples > max_samples) {
    end_frequency = start_frequency + (max_samples - 1) * frequency_increment;
  }
  return end_frequency;
}

static double get_inv_D(const input_shaper::Shaper &shaper) {
  double d = 0.;
  for (int i = 0; i < shaper.num_pulses; ++i) {
    d += shaper.a[i];
  }
  return 1. / d;
}

/**
 * @brief Get vibration reduction
 *
 * called _estimate_shaper in klipper source
 *
 * @param shaper
 * @param system_damping_ratio
 * @param frequency
 * @return Vibration reduction for system with system_damping_ratio at frequency.
 */
static double vibration_reduction(const input_shaper::Shaper &shaper, float system_damping_ratio, float frequency) {
    const double inv_D = get_inv_D(shaper);
    const double omega = 2. * std::numbers::pi_v<double> * frequency;
    const double damping = system_damping_ratio * omega;
    const double omega_d = omega * sqrt(1. - sq(system_damping_ratio));

    double s = 0.;
    double c = 0.;

    for (int i = 0; i < shaper.num_pulses; ++i) {
      const double w = shaper.a[i] * exp(-damping * (shaper.t[shaper.num_pulses - 1] - shaper.t[i]));
      s += w * sin(omega_d * shaper.t[i]);
      c += w * cos(omega_d * shaper.t[i]);
    }
    return (sqrt(sq(s) + sq(c)) * inv_D);
}

/**
 * @brief Get remaining vibrations
 *
 * called _estimate_remaining_vibrations in klipper source
 *
 * @param shaper
 * @param system_damping_ratio
 * @param psd power spectrum density
 *
 * @return remaining vibrations
 */
static float remaining_vibrations(const input_shaper::Shaper &shaper, float system_damping_ratio, const Fl_Spectrum &psd) {
  float vibr_threshold = psd.max() / default_vibration_reduction;
  float remaining_vibrations_sum = 0.f;
  float all_vibrations_sum = 0.f;
  for (size_t i = 0; i < psd.size(); ++i) {
    const FrequencyGain fg = psd.get(i);
    all_vibrations_sum += max(fg.gain, 0.f);
    const float vibration = max(fg.gain * static_cast<float>(vibration_reduction(shaper, system_damping_ratio, fg.frequency)) - vibr_threshold, 0.f);
    remaining_vibrations_sum += vibration;
  }
  return (remaining_vibrations_sum / all_vibrations_sum);
}

/**
 * @brief Get shaper smoothing
 *
 * Called _get_shaper_smoothing in klipper source
 *
 * @param shaper
 * @return smoothing
 */
static float smoothing(const input_shaper::Shaper &shaper) {
  constexpr float accel = 5000.f;
  constexpr float scv = 5.f;
  constexpr float half_accel = accel / 2.f;

  const double inv_D = get_inv_D(shaper);

  double ts = 0.;
  for (int i = 0; i < shaper.num_pulses; ++i) {
    ts += shaper.a[i] * shaper.t[i];
  }
  ts *= inv_D;

  double offset_90 = 0.;
  double offset_180 = 0.;

  for (int i = 0; i < shaper.num_pulses; ++i) {
    if(shaper.t[i] >= ts) {
      /// Calculate offset for one of the axes
      offset_90 += shaper.a[i] * (scv + half_accel * (shaper.t[i] - ts)) * (shaper.t[i] - ts);
    }
    offset_180 += shaper.a[i] * half_accel * sq(shaper.t[i] - ts);
  }
  offset_90 *= (inv_D * sqrt(2.));
  offset_180 *= inv_D;

  return max(offset_90, offset_180);
}

namespace{
enum class Action {
    first,
    /// Find lowest vibrs, reverse order, return when maximum smoothing exceeded
    find_best_result = first,
    /// Try to find an 'optimal' shapper configuration: the one that is not
    /// much worse than the 'best' one, but gives much less smoothing
    select,
    last = select,
};
Action &operator++ (Action &action) {
    using IntType = typename std::underlying_type<Action>::type;
    action = static_cast<Action>( static_cast<IntType>(action) + 1 );
    return action;
  }
} //anonymous namespace

struct Shaper_result {
  float frequency;
  float score;
  float smoothing;
};
static Shaper_result fit_shaper(input_shaper::Type type, const Fl_Spectrum &psd, uint8_t &progress_percent, const Action final_action = Action::last) {
  constexpr float start_frequency = 5.f;
  constexpr float end_frequency = 150.f;
  constexpr float frequency_step = .2f;
  constexpr float max_smoothing = std::numeric_limits<float>::max();

  struct Result {
    float frequency;
    float score;
    float smoothing;
    float vibrs;
  };

  Result best_result = {0.f, 0.f, 0.f, std::numeric_limits<float>::max()};
  Result selected_result = {0.f, 0.f, 0.f, std::numeric_limits<float>::max()};

  for(Action action = Action::first; action <= final_action; ++action) {
    for(float frequency = end_frequency; frequency >= start_frequency - epsilon; frequency -= frequency_step) {
      input_shaper::Shaper shaper = input_shaper::get(default_damping_ratio, frequency, default_vibration_reduction, type);
      const float shaper_smoothing = smoothing(shaper);
      if (Action::find_best_result == action && best_result.frequency && shaper_smoothing > max_smoothing) {
        Shaper_result result = {.frequency = best_result.frequency, .score = best_result.score, .smoothing = best_result.smoothing};
        return result;
      }
      /// Exact damping ratio of the printer is unknown, pessimizing
      /// remaining vibrations over possible damping values
      float shaper_vibrations = 0.f;
      for(double damping_ratio = 0.05; damping_ratio <= 0.15 + epsilon; damping_ratio += 0.05) {
        double vibrations = remaining_vibrations(shaper, damping_ratio, psd);
        if (vibrations > shaper_vibrations) {
          shaper_vibrations = vibrations;
        }
      }
      /// todo max_accel = self.find_shaper_max_accel(shaper) (not needed to fit filter)

      /// The score trying to minimize vibrations, but also accounting
      /// the growth of smoothing. The formula itself does not have any
      /// special meaning, it simply shows good results on real user data
      const float shaper_score = shaper_smoothing * (pow(shaper_vibrations, 1.5) + shaper_vibrations * .2 + .01);

      if (Action::find_best_result == action && shaper_vibrations < best_result.vibrs) {
        Result result = {.frequency = frequency, .score = shaper_score, .smoothing = shaper_smoothing, .vibrs = shaper_vibrations};
        best_result = result;
        selected_result = best_result;
      }

      if (Action::select == action && (shaper_vibrations < (best_result.vibrs * 1.1f)) && shaper_score < selected_result.score) {
        Result result = {.frequency = frequency, .score = shaper_score, .smoothing = shaper_smoothing, .vibrs = shaper_vibrations};
        selected_result = result;
      }
      idle(true, true); ///< We have data to process, but it is not time critical so waiting = true.
    }
    progress_percent += 8;
    SERIAL_ECHO_START();
    SERIAL_ECHOPAIR("For shaper type: ", static_cast<int>(type));
    switch (action) {
    case Action::find_best_result:
      SERIAL_ECHOPAIR(" lowest vibration frequency: ", selected_result.frequency);
      break;
    case Action::select:
      SERIAL_ECHOPAIR(" selected frequency: ", selected_result.frequency);
      break;
    }
    SERIAL_ECHO(" with score: ");
    SERIAL_PRINT(selected_result.score, 6);
    SERIAL_ECHO(" remaining vibrations: ");
    SERIAL_PRINT(selected_result.vibrs, 8);
    SERIAL_ECHO(" and smoothing: ");
    SERIAL_PRINTLN(selected_result.smoothing, 4);
  }

  Shaper_result shaper_result = {.frequency = selected_result.frequency, .score = selected_result.score, .smoothing = selected_result.smoothing};
  return shaper_result;
}

struct Best_shaper {
  float frequency;
  input_shaper::Type type;
};
struct Best_score{
  Shaper_result result;
  input_shaper::Type type;
};
static Best_shaper find_best_shaper(const Fl_Spectrum &psd, const Action final_action) {
  uint8_t progress_percent = 0;
  Best_score best_shaper = {fit_shaper(input_shaper::Type::first, psd, progress_percent, final_action), input_shaper::Type::first};
  for (input_shaper::Type shaper_type = input_shaper::Type::second; shaper_type <= input_shaper::Type::last; ++shaper_type) {
    Shaper_result shaper = fit_shaper(shaper_type, psd, progress_percent, final_action);
    if (shaper.score * 1.2f < best_shaper.result.score
          || ((shaper.score * 1.05f < best_shaper.result.score) && (shaper.smoothing * 1.1f < best_shaper.result.smoothing))) {
      best_shaper.type = shaper_type;
      best_shaper.result = shaper;
    }
  }
  return {best_shaper.result.frequency, best_shaper.type};
}


/**
 * @brief
 *
 * To save memory we assume reached frequency was equal to requested, so frequency returned by vibrate_measure() is discarded.
 *
 * @param subtract_excitation
 * @param axis_flag
 * @param start_frequency
 * @param end_frequency
 * @param frequency_increment
 * @param acceleration_requested
 * @param cycles
 */
static void klipper_tune(const bool subtract_excitation, const StepEventFlag_t axis_flag, const float start_frequency, float end_frequency, const float frequency_increment, const float acceleration_requested, const float step_len, const uint32_t cycles) {
  // Power spectrum density
  Fl_Spectrum psd(start_frequency, frequency_increment);
  end_frequency = limit_end_frequency(start_frequency, end_frequency, frequency_increment, psd.max_size());
  LogicalAxis logicalAxis = get_logical_axis(axis_flag);


  bool calibrate_accelerometer = true;
  for (float frequency_requested = start_frequency; frequency_requested <= end_frequency + epsilon; frequency_requested += frequency_increment) {
    FrequencyGain3D frequencyGain3D = vibrate_measure(axis_flag, true, frequency_requested, acceleration_requested, step_len, cycles, calibrate_accelerometer);
    calibrate_accelerometer = false;
    if(subtract_excitation) {
      if(logicalAxis.is_x) {
        frequencyGain3D.gain[0] = max(frequencyGain3D.gain[0] - 1.f, 0.f);
      }
      if(logicalAxis.is_y) {
        frequencyGain3D.gain[1] = max(frequencyGain3D.gain[1] - 1.f, 0.f);
      }
    }
    const float psd_xyz = sq(frequencyGain3D.gain[0]) + sq(frequencyGain3D.gain[1]) + sq(frequencyGain3D.gain[2]);
    psd.put(psd_xyz);
  }

  Temporary_Report_Off stop_busy_messages;

  if(subtract_excitation) {
    SERIAL_ECHOLN("Excitation subtracted power spectrum density");
    SERIAL_ECHOLN("freq,psd_xyz");
    for(size_t i = 0; i < psd.size(); ++i) {
      FrequencyGain fg = psd.get(i);
      SERIAL_ECHO(fg.frequency);
      SERIAL_ECHOLNPAIR_F(",", fg.gain, 5);
    }
  }

  if(logicalAxis.is_x || logicalAxis.is_y) {
    Best_shaper best_shaper = find_best_shaper(psd, subtract_excitation ? Action::find_best_result : Action::last);
    input_shaper::set(logicalAxis.is_x, logicalAxis.is_y, default_damping_ratio, best_shaper.frequency, default_vibration_reduction, best_shaper.type);
    SERIAL_ECHO_START();
    SERIAL_ECHOPAIR_F("Activated default damping and vibr. reduction shaper type: ", static_cast<int>(best_shaper.type));
    SERIAL_ECHOLNPAIR_F(" frequency: ", best_shaper.frequency);
  }
}

/**
 * @brief Tune input shaper
 *
 * - X<direction> Vibrate with X motor, start in direction 1 or -1
 * - Y<direction> Vibrate with Y motor, start in direction 1 or -1
 * - K           select Klipper tune algorithm
 * - KM          select Klipper Marek modified tune algorithm
 * - F<Hz>       Start frequency
 * - G<Hz>       End frequency
 * - H<Hz>       Frequency step
 * - A<mm/s-2>   Acceleration
 * - N<cycles>   Number of excitation signal periods
 *               of active measurement.
 */
void GcodeSuite::M959() {
  MicrostepRestorer microstepRestorer;
  StepEventFlag_t axis_flag = setup_axis();
  const float step_len = get_step_len(axis_flag);
  const bool seen_m = parser.seen('M');

  float start_frequency = 5.f;
  if (parser.seenval('F')) {
    start_frequency = abs(parser.value_float());
  }
  float end_frequency = 150.f;
  if (parser.seenval('G')) {
    end_frequency = abs(parser.value_float());
  }
  float frequency_increment = 1.f;
  if (parser.seenval('H')) {
    frequency_increment = abs(parser.value_float());
  }
  float acceleration_requested = 2.5f;
  if (parser.seenval('A')) {
    acceleration_requested = abs(parser.value_float()) * 0.001f;
  }
  uint32_t cycles = 50;
  if (parser.seenval('N')) {
    cycles = parser.value_ulong();
  }
  if (parser.seen('K')) {
    klipper_tune(seen_m, axis_flag, start_frequency, end_frequency, frequency_increment, acceleration_requested, step_len, cycles);
  } else {
    naive_zv_tune(axis_flag, start_frequency, end_frequency, frequency_increment, acceleration_requested, step_len, cycles);
  }
}
#endif
