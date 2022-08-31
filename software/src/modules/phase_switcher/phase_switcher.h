/* phase switcher for warp-charger
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include "bindings/bricklet_industrial_quad_relay_v2.h"
#include "bindings/bricklet_industrial_digital_in_4_v2.h"

#include "config.h"
#include "bricklet.h"
#include "device_module.h"

#define EVSE_START_TIMEOUT 10000
#define EVSE_STOP_TIMEOUT 10000

#define QUICK_CHARGE_BUTTON_PRESSED_TIME 2000

#define MIN_POWER_ONE_PHASE (6 * 230)
#define MIN_POWER_TWO_PHASES (6 * 230 * 2)
#define MIN_POWER_THREE_PHASES (6 * 230 * 3)

#define PHASE_SWITCHER_HISTORY_HOURS 12
#define PHASE_SWITCHER_HISTORY_MINUTE_INTERVAL 1
#define PHASE_SWITCHER_RING_BUF_SIZE (PHASE_SWITCHER_HISTORY_HOURS * (60 / PHASE_SWITCHER_HISTORY_MINUTE_INTERVAL) + 1)

typedef Bricklet<TF_IndustrialQuadRelayV2, 
                tf_industrial_quad_relay_v2_create> QuadRelayBricklet;

typedef Bricklet<TF_IndustrialDigitalIn4V2, 
                tf_industrial_digital_in_4_v2_create> DigitalInBricklet;

class PhaseSwitcher{

public:
    PhaseSwitcher();
    void setup();
    void register_urls();
    void loop();
    bool initialized = false;

private:
    typedef enum {
        inactive = 0,
        standby = 1,
        cancelling_evse_start = 5,
        waiting_for_evse_start = 10,
        active = 20,
        quick_charging = 25,
        waiting_for_evse_stop = 30,
        pausing_while_switching = 40,
        stopped_by_evse = 50
    } PhaseSwitcherState;

    typedef enum {
        not_connected = 0,
        waiting_for_charge_release = 1,
        ready_for_charging = 2,
        charging = 3,
        error = 4
    } ChargerState;

    typedef enum {
        a_not_connected = 0,
        b_connected = 1,
        c_charging = 2,
        d_charging_with_ventilation = 3,
        ef_error = 4
    } IEC61851State;

    typedef enum {
        one_phase_static = 1,
        two_phases_static = 2,
        three_phases_static = 3,
        one_two_phases_dynamic = 12,
        one_three_phases_dynamic = 13,
        one_two_three_phases_dynamic = 123
    } PhaseSwitcherMode;

    bool setup_bricklets();

    uint8_t get_active_phases();
    void set_available_charging_power(uint16_t available_charging_power);
    void set_current(uint16_t available_charging_power, uint8_t phases);
    uint8_t get_phases_for_power(uint16_t available_charging_power);
    void start_quick_charging();

    void handle_button();
    void handle_evse();

    void sequencer_state_inactive();
    void sequencer_state_standby();
    void sequencer_state_cancelling_evse_start();
    void sequencer_state_waiting_for_evse_start();
    void sequencer_state_active();
    void sequencer_state_quick_charging();
    void sequencer_state_waiting_for_evse_stop();
    void sequencer_state_pausing_while_switching();
    void sequencer_state_stopped_by_evse();

    void write_outputs();
    void contactor_check();
    void update_all_data();
    void update_history();

    QuadRelayBricklet quad_relay_bricklet = QuadRelayBricklet(
            TF_INDUSTRIAL_QUAD_RELAY_V2_DEVICE_IDENTIFIER,
            "industrial quad relay bricklet",
            "phase switcher");

    DigitalInBricklet digital_in_bricklet = DigitalInBricklet(
            TF_INDUSTRIAL_DIGITAL_IN_4_V2_DEVICE_IDENTIFIER,
            "industrial digital in bricklet",
            "phase switcher");

// Alternative notations:    
    // QuadRelayBricklet quad_relay_bricklet{TF_INDUSTRIAL_QUAD_RELAY_V2_DEVICE_IDENTIFIER,
    //         "industrial digital in bricklet",
    //         "phase switcher"};

    // Bricklet<TF_IndustrialQuadRelayV2, tf_industrial_quad_relay_v2_create> quad_relay_bricklet =
    //     Bricklet<TF_IndustrialQuadRelayV2, tf_industrial_quad_relay_v2_create>(
    //         TF_INDUSTRIAL_QUAD_RELAY_V2_DEVICE_IDENTIFIER,
    //         "industrial digital in bricklet",
    //         "phase switcher");

    bool debug = false;

    ConfigRoot phase_switcher_state;
    ConfigRoot phase_switcher_available_charging_power;
    ConfigRoot phase_switcher_start_quick_charging;
    ConfigRoot phase_switcher_config;
    ConfigRoot phase_switcher_config_in_use;

    bool enabled, quick_charging_active;
    PhaseSwitcherMode operating_mode;
    uint8_t requested_phases, requested_phases_pending;
    uint16_t available_charging_power;
    PhaseSwitcherState sequencer_state;
    uint32_t last_phase_request_change, last_state_change;
    uint32_t last_one_phase_request, last_two_phases_request, last_three_phases_request;

    ChargerState charger_state;
    IEC61851State iec61851_state;
    uint8_t auto_start_charging;
    bool contactor_error;

    TF_Ringbuffer<int16_t,
                  PHASE_SWITCHER_RING_BUF_SIZE,
                  uint32_t,
#if defined(BOARD_HAS_PSRAM)
                  malloc_psram,
#else
                  malloc_32bit_addressed,
#endif
                  heap_caps_free> requested_power_history;


    TF_Ringbuffer<int16_t,
                  PHASE_SWITCHER_RING_BUF_SIZE,
                  uint32_t,
#if defined(BOARD_HAS_PSRAM)
                  malloc_psram,
#else
                  malloc_32bit_addressed,
#endif
                  heap_caps_free> charging_power_history;


    TF_Ringbuffer<int16_t,
                  PHASE_SWITCHER_RING_BUF_SIZE,
                  uint32_t,
#if defined(BOARD_HAS_PSRAM)
                  malloc_psram,
#else
                  malloc_32bit_addressed,
#endif
                  heap_caps_free> active_phases_history;


};
                                    
