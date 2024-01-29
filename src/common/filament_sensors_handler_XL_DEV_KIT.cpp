/**
 * @file filament_sensors_handler_XL.cpp
 * @brief this file contains code for filament sensor api with multi tool support
 */

#include "filament_sensors_handler.hpp"
#include "filament_sensor_adc.hpp"
#include "filament_sensor_adc_eval.hpp"
#include "filters/median_filter.hpp"
#include "marlin_client.hpp"
#include <common/freertos_mutex.hpp>
#include <mutex>
#include <puppies/Dwarf.hpp>
#include "src/module/prusa/toolchanger.h"

// Meyer's singleton
FSensorAdcExtruder *getExtruderFSensor(uint8_t index) {
    static std::array<FSensorAdcExtruder, EXTRUDERS> printer_sensors = { {
        { 0, false },
        { 1, false },
        { 2, false },
        { 3, false },
        { 4, false },
        { 5, false },
    } };

    return (index < 5 && prusa_toolchanger.is_tool_enabled(index)) ? &printer_sensors[index] : nullptr; // 6th sensor is not calibrated and causing errors
}

// Meyer's singleton
FSensorAdcSide *getSideFSensor(uint8_t index) {
    static std::array<FSensorAdcSide, EXTRUDERS> side_sensors = { {
        { 0, true },
        { 1, true },
        { 2, true },
        { 3, true },
        { 4, true },
        { 5, true },
    } };
    return (index < 5 && prusa_toolchanger.is_tool_enabled(index)) ? &side_sensors[index] : nullptr; // 6th sensor is not calibrated and causing errors
}

// function returning abstract sensor - used in higher level api
IFSensor *GetExtruderFSensor(uint8_t index) {
    return getExtruderFSensor(index);
}

// function returning abstract sensor - used in higher level api
IFSensor *GetSideFSensor(uint8_t index) {
    return getSideFSensor(index);
}

void FilamentSensors::SetToolIndex() {
    tool_index = prusa_toolchanger.get_active_tool_nr();
}

void FilamentSensors::configure_sensors() {
    logical_sensors.current_extruder = GetExtruderFSensor(tool_index);
    logical_sensors.current_side = GetSideFSensor(tool_index);

    logical_sensors.primary_runout = logical_sensors.current_side;
    logical_sensors.secondary_runout = logical_sensors.current_extruder;
    logical_sensors.autoload = logical_sensors.current_extruder;
}

void FilamentSensors::reconfigure_sensors_if_needed() {
    uint8_t current_tool = prusa_toolchanger.get_active_tool_nr();

    if (current_tool != tool_index) {
        tool_index = current_tool; // must be done before configure_sensors!!!
        // configure_sensors uses it
        configure_sensors();
    }
}

void FilamentSensors::AdcExtruder_FilteredIRQ(int32_t val, uint8_t tool_index) {
    FSensorADC *sensor = getExtruderFSensor(tool_index);
    if (sensor) {
        sensor->set_filtered_value_from_IRQ(val);
    } else {
        assert("wrong extruder index");
    }
}

void FilamentSensors::AdcSide_FilteredIRQ(int32_t val, uint8_t tool_index) {
    FSensorADC *sensor = getSideFSensor(tool_index);
    if (sensor) {
        sensor->set_filtered_value_from_IRQ(val);
    } else {
        assert("wrong extruder index");
    }
}

// IRQ - called from interruption
void fs_process_sample(int32_t fs_raw_value, uint8_t tool_index) {
    // does not need to be filtered (data from tool are already filtered)
    FSensors_instance().AdcExtruder_FilteredIRQ(fs_raw_value, tool_index);
}

void side_fs_process_sample(int32_t fs_raw_value, uint8_t tool_index) {
    static MedianFilter filter[HOTENDS];
    assert(tool_index < HOTENDS);

    FSensorADC *sensor = getSideFSensor(tool_index);
    if (sensor) {
        sensor->record_raw(fs_raw_value);
    }

    if (filter[tool_index].filter(fs_raw_value)) { // fs_raw_value is rewritten - passed by reference
        FSensors_instance().AdcSide_FilteredIRQ(fs_raw_value, tool_index);
    } else {
        FSensors_instance().AdcSide_FilteredIRQ(FSensorADCEval::filtered_value_not_ready, tool_index);
    }
}
