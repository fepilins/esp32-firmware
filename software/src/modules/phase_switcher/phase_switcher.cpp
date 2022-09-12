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
#include "phase_switcher.h"

#include "bindings/errors.h"

#include "api.h"
#include "event_log.h"
#include "task_scheduler.h"
#include "tools.h"
#include "web_server.h"
#include "modules.h"

extern EventLog logger;

extern TaskScheduler task_scheduler;
extern TF_HAL hal;
extern WebServer server;

extern API api;

PhaseSwitcher::PhaseSwitcher()
{
    phase_switcher_config = Config::Object({
        {"enabled", Config::Bool(false)},
        {"operating_mode", Config::Uint8(3)},
        {"delay_time_more_phases", Config::Uint(300, 10, 60 * 60)},
        {"delay_time_less_phases", Config::Uint(60, 10, 60 * 60)},
        {"minimum_duration", Config::Uint(15 * 60, 10, 60 * 60)},
        {"pause_time", Config::Uint(2 * 60, 10, 60 * 60)}
    });

    phase_switcher_state = Config::Object({
        {"available_charging_power", Config::Uint16(0)},
        {"requested_phases", Config::Uint8(0)},
        {"requested_phases_pending", Config::Uint8(0)},
        {"active_phases", Config::Uint8(1)}, // 0 - no phase active, 1 - one phase active, 2 - two phases active, 3 = three phases active
        {"sequencer_state", Config::Uint8(0)},
        {"time_since_state_change", Config::Uint32(0)},
        {"delay_time", Config::Uint32(0)},
        {"contactor_state", Config::Bool(false)}
    });

    phase_switcher_available_charging_power = Config::Object({
        {"power", Config::Uint16(0)}
    });

    phase_switcher_start_quick_charging = Config::Null();

}

void PhaseSwitcher::setup()
{
    if (!setup_bricklets()){
        return;
    }

    if (!modbus_meter.initialized){
        logger.printfln("Phase Switcher: Energy meter not available. Disabling phase switcher module.");
        return;
    }

    requested_power_history.setup();
    requested_power_history.clear();

    for (int i = 0; i < requested_power_history.size(); ++i) {
        // Use negative values to mark that these are pre-filled.
        requested_power_history.push(-1);
    }

    charging_power_history.setup();
    charging_power_history.clear();

    for (int i = 0; i < charging_power_history.size(); ++i) {
        // Use negative values to mark that these are pre-filled.
        charging_power_history.push(-1);
    }

    active_phases_history.setup();
    active_phases_history.clear();

    for (int i = 0; i < active_phases_history.size(); ++i) {
        // Use negative values to mark that these are pre-filled.
        active_phases_history.push(-1);
    }

    api.restorePersistentConfig("phase_switcher/config", &phase_switcher_config);
    phase_switcher_config_in_use = phase_switcher_config;

    enabled = phase_switcher_config.get("enabled")->asBool();
    operating_mode = PhaseSwitcherMode(phase_switcher_config_in_use.get("operating_mode")->asUint());

    api.addFeature("phase_switcher");

    task_scheduler.scheduleWithFixedDelay([this](){
        this->handle_button();
        this->handle_evse();
        this->write_outputs();
        this->contactor_check();
    }, 0, 250);

    task_scheduler.scheduleWithFixedDelay([this](){
        update_all_data();
    }, 10, 250);

    task_scheduler.scheduleWithFixedDelay([this](){
        update_history();
    }, 20, PHASE_SWITCHER_HISTORY_MINUTE_INTERVAL * 60 * 1000);

    initialized = true;
}

bool PhaseSwitcher::setup_bricklets()
{   
    bool ret_value[4];
    int result;

    // setup quad relay bricklet:
    if (!quad_relay_bricklet.setup_device()){
        return false;
    };
    result = tf_industrial_quad_relay_v2_get_value(&quad_relay_bricklet.device, ret_value);
    if (result != TF_E_OK) {
        logger.printfln("Industrial quad relay get value failed (rc %d). Disabling phase switcher support.", result);
        return false;
    }

    // setup digital in bricklet:
    if (!digital_in_bricklet.setup_device()){
        return false;
    };
    result = tf_industrial_digital_in_4_v2_get_value(&digital_in_bricklet.device, ret_value);
    if (result != TF_E_OK) {
        logger.printfln("Industrial digital in get value failed (rc %d). Disabling phase switcher support.", result);
        return false;
    }

    return true;
}

void PhaseSwitcher::register_urls()
{
    if (!initialized)
        return;

    api.addState("phase_switcher/state", &phase_switcher_state, {}, 1000);
 
    api.addCommand("phase_switcher/available_charging_power", &phase_switcher_available_charging_power, {}, [this](){
        if (enabled && !quick_charging_active){
            set_available_charging_power(phase_switcher_available_charging_power.get("power")->asUint());
        }
    }, false);

    api.addCommand("phase_switcher/start_quick_charging", &phase_switcher_start_quick_charging, {}, [this](){
        start_quick_charging();
    }, true);

    api.addPersistentConfig("phase_switcher/config", &phase_switcher_config, {}, 10000);


    server.on("/phase_switcher/requested_power_history", HTTP_GET, [this](WebServerRequest request) {
        if (!initialized) {
            return request.send(400, "text/html", "not initialized");
        }
        
        const size_t buf_size = PHASE_SWITCHER_RING_BUF_SIZE * 6 + 100;
        char buf[buf_size] = {0};
        size_t buf_written = 0;

        int16_t val;
        
        requested_power_history.peek(&val);
        // Negative values are prefilled, because the ESP was booted less than 48 hours ago.
        if (val < 0)
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%s", "[null");
        else
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "[%d", (int)val);

        for (int i = 1; i < requested_power_history.used() && requested_power_history.peek_offset(&val, i) && buf_written < buf_size; ++i) {
            // Negative values are prefilled, because the ESP was booted less than 48 hours ago.
            if (val < 0)
                buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%s", ",null");
            else
                buf_written += snprintf(buf + buf_written, buf_size - buf_written, ",%d", (int)val);
        }

        if (buf_written < buf_size)
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%c", ']');
        
        return request.send(200, "application/json; charset=utf-8", buf, buf_written);
    });

    server.on("/phase_switcher/charging_power_history", HTTP_GET, [this](WebServerRequest request) {
        if (!initialized) {
            return request.send(400, "text/html", "not initialized");
        }

        const size_t buf_size = PHASE_SWITCHER_RING_BUF_SIZE * 6 + 100;
        char buf[buf_size] = {0};
        size_t buf_written = 0;

        int16_t val;
        charging_power_history.peek(&val);
        // Negative values are prefilled, because the ESP was booted less than 48 hours ago.
        if (val < 0)
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%s", "[null");
        else
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "[%d", (int)val);

        for (int i = 1; i < charging_power_history.used() && charging_power_history.peek_offset(&val, i) && buf_written < buf_size; ++i) {
            // Negative values are prefilled, because the ESP was booted less than 48 hours ago.
            if (val < 0)
                buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%s", ",null");
            else
                buf_written += snprintf(buf + buf_written, buf_size - buf_written, ",%d", (int)val);
        }

        if (buf_written < buf_size)
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%c", ']');

        return request.send(200, "application/json; charset=utf-8", buf, buf_written);
    });

    server.on("/phase_switcher/requested_phases_history", HTTP_GET, [this](WebServerRequest request) {
        if (!initialized) {
            return request.send(400, "text/html", "not initialized");
        }

        const size_t buf_size = PHASE_SWITCHER_RING_BUF_SIZE * 6 + 100;
        char buf[buf_size] = {0};
        size_t buf_written = 0;

        int16_t val;
        active_phases_history.peek(&val);
        // Negative values are prefilled, because the ESP was booted less than 48 hours ago.
        if (val < 0)
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%s", "[null");
        else
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "[%d", (int)val);

        for (int i = 1; i < active_phases_history.used() && active_phases_history.peek_offset(&val, i) && buf_written < buf_size; ++i) {
            // Negative values are prefilled, because the ESP was booted less than 48 hours ago.
            if (val < 0)
                buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%s", ",null");
            else
                buf_written += snprintf(buf + buf_written, buf_size - buf_written, ",%d", (int)val);
        }

        if (buf_written < buf_size)
            buf_written += snprintf(buf + buf_written, buf_size - buf_written, "%c", ']');

        return request.send(200, "application/json; charset=utf-8", buf, buf_written);
    });

    server.on("/phase_switcher/start_debug", HTTP_GET, [this](WebServerRequest request) {
        task_scheduler.scheduleOnce([this](){
            logger.printfln("Phase switcher: Enabling debug mode");
            debug = true;
            this->update_history();
        }, 0);
        return request.send(200);
    });

    server.on("/phase_switcher/stop_debug", HTTP_GET, [this](WebServerRequest request){
        task_scheduler.scheduleOnce([this](){
            logger.printfln("Phase switcher: Disabling debug mode");
            debug = false;
        }, 0);
        return request.send(200);
    });

}

void PhaseSwitcher::loop(){}

uint8_t PhaseSwitcher::get_active_phases()
{
    static Config *evse_state = api.getState("evse/state", false);

    if (evse_state == nullptr) 
        return 0;

    // phase 1 is monitored via the EVSE bricklet, not via digital in bricklet
    bool channel_state_1 = (evse_state->get("contactor_state")->asUint() == 3);

    bool channel_state[4];

    int retval = tf_industrial_digital_in_4_v2_get_value(&digital_in_bricklet.device, channel_state);
    if (retval != TF_E_OK) {
        logger.printfln("Industrial digital in relay get value failed (rc %d).", retval);
        return 0;
    }

    if (channel_state_1 && channel_state[2] && channel_state[3]){
        return 3;
    } else if (channel_state_1 && channel_state[2]){
        return 2;
    } else if (channel_state_1){
        return 1;
    } else {
        return 0;
    }
}

uint8_t PhaseSwitcher::get_phases_for_power(uint16_t available_charging_power)
{
    if (debug){
        logger.printfln("  Phase switcher: get_phases_for_power w/ available_charging_power %d", available_charging_power);
        logger.printfln("  Phase switcher: get_phases_for_power w/ MIN_POWER_ONE_PHASE %d, MIN_POWER_TWO_PHASES %d, MIN_POWER_THREE_PHASES %d", MIN_POWER_ONE_PHASE, MIN_POWER_TWO_PHASES, MIN_POWER_THREE_PHASES);
        logger.printfln("  Phase switcher: get_phases_for_power w/ MAX_POWER_ONE_PHASE %d, MAX_POWER_TWO_PHASES %d", MAX_POWER_ONE_PHASE, MAX_POWER_TWO_PHASES);
        logger.printfln("  Phase switcher: get_phases_for_power w/ operating_mode %d", operating_mode);
    }    
    switch(operating_mode){
        case one_phase_static:
            if (debug) logger.printfln("    Phase switcher: get_phases_for_power one phase static");
            if (available_charging_power >= MIN_POWER_ONE_PHASE){
                return 1;
            } else {
                return 0;
            }

        case two_phases_static:
            if (debug) logger.printfln("    Phase switcher: get_phases_for_power two phases static");
            if (available_charging_power >= MIN_POWER_TWO_PHASES){
                return 2;
            } else {
                return 0;
            }

        case three_phases_static:
            if (debug) logger.printfln("    Phase switcher: get_phases_for_power three phases static");
            if (available_charging_power >= MIN_POWER_THREE_PHASES){
                return 3;
            } else {
                return 0;
            }

        case one_two_phases_dynamic:
            if (debug) logger.printfln("    Phase switcher: get_phases_for_power one/two phases dynamic");
            // Phasenwechsel nach Möglichkeit vermeiden
            // Wenn wir auf zwei Phasen laufen
            if (requested_phases == 2) {
                if (available_charging_power >= MIN_POWER_TWO_PHASES) {
                    return 2;
                } else if(available_charging_power >= MIN_POWER_ONE_PHASE) {
                    return 1;
                } else {
                    return 0;
                }
            } else {
                if (available_charging_power >= MAX_POWER_ONE_PHASE){
                    return 2;
                } else if (available_charging_power >= MIN_POWER_ONE_PHASE){
                    return 1;
                } else {
                    return 0;
                }
            }

        case one_three_phases_dynamic:
            if (debug) logger.printfln("    Phase switcher: get_phases_for_power one/three phases dynamic");
            if (available_charging_power >= MIN_POWER_THREE_PHASES){
                return 3;
            } else if (available_charging_power >= MIN_POWER_ONE_PHASE){
                return 1;
            } else {
                return 0;
            }

        case one_two_three_phases_dynamic:
            if (debug) logger.printfln("    Phase switcher: get_phases_for_power one/two/three phases dynamic");
            if (available_charging_power >= MIN_POWER_THREE_PHASES){
                return 3;
            } else if (available_charging_power >= MIN_POWER_TWO_PHASES){
                return 2;
            } else if (available_charging_power >= MIN_POWER_ONE_PHASE){
                return 1;
            } else {
                return 0;
            }
        
        default:
            if (debug) logger.printfln("    Phase switcher: get_phases_for_power default");
            return 0;
    }
}

void PhaseSwitcher::set_available_charging_power(uint16_t available_charging_power)
{
    static uint8_t last_requested_phases_pending = 0;

    PhaseSwitcher::available_charging_power = available_charging_power;
    requested_phases_pending = get_phases_for_power(available_charging_power);
    if (debug) logger.printfln("  Phase switcher: set_available_charging_power w/ requested_phases_pending %d, last_requested_phases_pending %d, requested_phases %d", requested_phases_pending, last_requested_phases_pending, requested_phases);

    // check if number of phases needs to be changed to reach the requested charging power:
    if (requested_phases_pending != last_requested_phases_pending){
        if (requested_phases_pending != requested_phases){
            last_phase_request_change = millis();
            if (debug) logger.printfln("Phase switcher: Available charging power %d W received. Requesting %d phase(s) to be used.", available_charging_power, requested_phases_pending);

            
            switch(requested_phases_pending){
                case 0:

                    break;
                case 1:
                    break;

                case 2:
                    break;

                case 3:
                    break;

                default:
                    break;
            }

        }
        last_requested_phases_pending = requested_phases_pending;
    } 

    set_current(available_charging_power, requested_phases);
}

void PhaseSwitcher::set_current(uint16_t available_charging_power, uint8_t phases)
{
    uint32_t requested_current;
    
    if (phases != 0){
        requested_current = max(min(available_charging_power * 1000 / 230 / phases, 32000), 6000);
    } else {
        requested_current = 0;    
    }

    api.callCommand("evse/external_current_update", Config::ConfUpdateObject{{
        {"current", requested_current}
    }});
    if (debug) logger.printfln("Phase switcher: Setting current for %d W charging power at %d phases to %.2f A", available_charging_power, phases, ((float)requested_current)/1000);
}

void PhaseSwitcher::start_quick_charging()
{
    if (!enabled){
        return;
    }

    if (sequencer_state == standby || sequencer_state == stopped_by_evse){
        logger.printfln("Phase switcher: Quick charging requested");
        quick_charging_active = true;
        requested_phases_pending = 3;
        api.callCommand("evse/external_current_update", Config::ConfUpdateObject{{
            {"current", 32000}
        }});
    } else {
        logger.printfln("Phase switcher: Quick charging request ignored because sequencer is not in standby state");
    }

}

void PhaseSwitcher::handle_button()
{
    static Config *evse_low_level_state = api.getState("evse/low_level_state", false);
    if (evse_low_level_state == nullptr) return;

    bool button_state = evse_low_level_state->get("gpio")->get(0)->asBool();
    static uint32_t button_pressed_time;
    static bool quick_charging_requested = false;

    if (!button_state){
        button_pressed_time = millis();
        quick_charging_requested = false;
    } 

    if (deadline_elapsed(button_pressed_time + QUICK_CHARGE_BUTTON_PRESSED_TIME) && !quick_charging_requested){
        start_quick_charging();
        quick_charging_requested = true;
    } 


}

void PhaseSwitcher::handle_evse()
{
    static Config *evse_state = api.getState("evse/state", false);

    if (evse_state == nullptr) {
        if (debug) logger.printfln("Phase switcher handle_evse: Failed to get API 'evse/state'");
        return;
    }

    static Config *evse_auto_start_charging = api.getState("evse/auto_start_charging", false);

    if (evse_auto_start_charging == nullptr)
        return;

    charger_state = ChargerState(evse_state->get("charger_state")->asUint());
    iec61851_state = IEC61851State(evse_state->get("iec61851_state")->asUint());
    auto_start_charging = evse_auto_start_charging->get("auto_start_charging")->asBool();

    if (!enabled || charger_state == not_connected || charger_state == error){
        sequencer_state = inactive;
        quick_charging_active = false;
        requested_phases = 0;
        return;
    }

    switch(sequencer_state){
        case inactive:                  sequencer_state_inactive(); break;
        case standby:                   sequencer_state_standby(); break;
        case cancelling_evse_start:     sequencer_state_cancelling_evse_start(); break;
        case waiting_for_evse_start:    sequencer_state_waiting_for_evse_start(); break;
        case active:                    sequencer_state_active(); break;
        case quick_charging:            sequencer_state_quick_charging(); break;
        case waiting_for_evse_stop:     sequencer_state_waiting_for_evse_stop(); break;
        case pausing_while_switching:   sequencer_state_pausing_while_switching(); break;
        case stopped_by_evse:           sequencer_state_stopped_by_evse(); break;
    }

    static PhaseSwitcherState last_sequencer_state = inactive;
    if (last_sequencer_state != sequencer_state){
        if (debug) logger.printfln("  Phase switcher sequencer state changed to: %d", sequencer_state);
        last_state_change = millis();
        last_sequencer_state = sequencer_state;
    } 
}

void PhaseSwitcher::sequencer_state_inactive()
{
    if (charger_state == waiting_for_charge_release && (auto_start_charging || iec61851_state == b_connected) && !contactor_error){
        logger.printfln("Phase switcher: Vehicle connected, changing to standby state.");
        sequencer_state = standby;
    } else if (charger_state == ready_for_charging || charger_state == charging){
        logger.printfln("Phase switcher: Charging initiated by EVSE but requested power is not sufficient. Requesting EVSE to stop charging.");
        sequencer_state = cancelling_evse_start;
    } 
}

void PhaseSwitcher::sequencer_state_standby()
{
    if (deadline_elapsed(last_phase_request_change + phase_switcher_config_in_use.get("delay_time_more_phases")->asUint() * 1000)){
        if (requested_phases_pending > 0) {
            logger.printfln("Phase switcher: Requesting EVSE to start charging.");
            if (!quick_charging_active) set_current(phase_switcher_available_charging_power.get("power")->asUint(), requested_phases_pending);
            sequencer_state = waiting_for_evse_start;
        }
        requested_phases = requested_phases_pending;
    } else if (charger_state == ready_for_charging || charger_state == charging){
        logger.printfln("Phase switcher: Charging initiated by EVSE but requested power is not sufficient. Requesting EVSE to stop charging.");
        sequencer_state = cancelling_evse_start;
    }
}

void PhaseSwitcher::sequencer_state_cancelling_evse_start()
{
    static uint32_t watchdog_start = 0;

    if (deadline_elapsed(watchdog_start + EVSE_STOP_TIMEOUT)){
        logger.printfln("Phase switcher: Sending stop API request to EVSE.");
        api.callCommand("evse/stop_charging", nullptr);
        watchdog_start = millis();
    }

    if (charger_state != ready_for_charging && charger_state != charging){
        logger.printfln("Phase switcher: Charging stopped by EVSE, changing to standby state.");
        watchdog_start = 0;
        sequencer_state = standby;
    }

}

void PhaseSwitcher::sequencer_state_waiting_for_evse_start()
{
    static uint32_t watchdog_start = 0;

    if (deadline_elapsed(watchdog_start + EVSE_START_TIMEOUT)){
        logger.printfln("Phase switcher: Sending start API request to EVSE.");
        api.callCommand("evse/start_charging", nullptr);
        watchdog_start = millis();
    }

    if (charger_state == charging){
        if (quick_charging_active){
            logger.printfln("Phase switcher: Charging started by EVSE, changing to quick charging active state.");
            sequencer_state = quick_charging;
        } else {
            logger.printfln("Phase switcher: Charging started by EVSE, changing to active state.");
            sequencer_state = active;
        }
        watchdog_start = 0;
    } 

}

void PhaseSwitcher::sequencer_state_active()
{
    bool more_phases_requested = requested_phases_pending > requested_phases;
    bool less_phases_requested = requested_phases_pending < requested_phases;

    bool delay_for_more_phases_elapsed = deadline_elapsed(last_phase_request_change + phase_switcher_config_in_use.get("delay_time_more_phases")->asUint() * 1000);
    bool delay_for_less_phases_elapsed = deadline_elapsed(last_phase_request_change + phase_switcher_config_in_use.get("delay_time_less_phases")->asUint() * 1000);
    
    bool minimum_duration_elapsed = deadline_elapsed(last_state_change + phase_switcher_config_in_use.get("minimum_duration")->asUint() * 1000);

    if (((more_phases_requested && delay_for_more_phases_elapsed) || (less_phases_requested && delay_for_less_phases_elapsed)) && minimum_duration_elapsed){
        logger.printfln("Phase switcher: Change to %d phase charging requested while charging with %d phases. Requesting EVSE to stop charging.", requested_phases_pending, requested_phases);
        sequencer_state = waiting_for_evse_stop;
    } else if (charger_state != charging){
        logger.printfln("Phase switcher: Charging stopped by EVSE. Waiting either for disconnect or quick charge request.");
        sequencer_state = stopped_by_evse;
        quick_charging_active = false;
    }
}

void PhaseSwitcher::sequencer_state_quick_charging()
{
    if (charger_state != charging){
        logger.printfln("Phase switcher: Charging stopped by EVSE. Waiting either for disconnect or quick charge request.");
        set_available_charging_power(phase_switcher_available_charging_power.get("power")->asUint());
        sequencer_state = stopped_by_evse;
        quick_charging_active = false;
    }
}

void PhaseSwitcher::sequencer_state_waiting_for_evse_stop()
{
    static uint32_t watchdog_start = 0;

    if (deadline_elapsed(watchdog_start + EVSE_STOP_TIMEOUT)){
        logger.printfln("Phase switcher: Sending stop API request to EVSE.");
        api.callCommand("evse/stop_charging", nullptr);
        watchdog_start = millis();
    }

    if (charger_state != charging){
        if (requested_phases_pending != 0 && !contactor_error){
            logger.printfln("Phase switcher: EVSE stopped charging, waiting for pause time to elapse.");
            sequencer_state = pausing_while_switching;
        } else {
            logger.printfln("Phase switcher: EVSE stopped charging, waiting for car to be disconnected.");
            requested_phases = requested_phases_pending;
            sequencer_state = standby;
        }
    } 
}

void PhaseSwitcher::sequencer_state_pausing_while_switching()
{
    if (deadline_elapsed(last_state_change + phase_switcher_config_in_use.get("pause_time")->asUint() * 1000)){
        logger.printfln("Phase switcher: Pause time elapsed, restarting charging with %d phases.", requested_phases);
        logger.printfln("Phase switcher: Waiting for EVSE to start charging.");
        requested_phases = requested_phases_pending;
        set_current(phase_switcher_available_charging_power.get("power")->asUint(), requested_phases);
        sequencer_state = waiting_for_evse_start;
    }
}

void PhaseSwitcher::sequencer_state_stopped_by_evse()
{
    // reset to inactive state is initiated by initializing clause above the sequencer
    if (quick_charging_active){
        logger.printfln("Phase switcher: Quick charging initiated, changing to standby state.");
        sequencer_state = standby;
    } else if (charger_state == charging){
        logger.printfln("Phase switcher: Charging started by EVSE, changing to active state.");
        sequencer_state = active;
    }
    requested_phases = requested_phases_pending;
}

void PhaseSwitcher::write_outputs()
{
    static Config *evse_low_level_state = api.getState("evse/low_level_state", false);

    if (evse_low_level_state == nullptr)
        return;

    bool evse_relay_output = evse_low_level_state->get("gpio")->get(3)->asBool();
    bool channel_request[4] = {false, false, false, false};

    if (debug) {
        static bool last_evse_relay_output = false;
        if (last_evse_relay_output != evse_relay_output){
            logger.printfln("Phase switcher: EVSE relay output changed to %d", evse_relay_output);
            last_evse_relay_output = evse_relay_output;
        }
    }

    if (evse_relay_output && !contactor_error){
        if (enabled){
            switch (requested_phases)
            {
            case 0:
                break;
            case 1:
                channel_request[1] = true;
                break;
            
            case 2:
                channel_request[1] = true;
                channel_request[2] = true;
                break;

            default:
                channel_request[1] = true;
                channel_request[2] = true;
                channel_request[3] = true;
                break;
            }
        } else if (!enabled){
            channel_request[1] = true;
            channel_request[2] = true;
            channel_request[3] = true;
        } 
    }

    int retval;
    // retval = tf_industrial_quad_relay_v2_set_value(&quad_relay_bricklet.device, channel_request);
    // if (retval != TF_E_OK) {
    //     logger.printfln("Industrial quad relay set value failed (rc %d).", retval);
    // }

    for (int channel = 0; channel <= 3; channel++) {
        if (channel_request[channel])
            retval = tf_industrial_quad_relay_v2_set_monoflop(&quad_relay_bricklet.device, channel, true, 2000);
        else
            retval  = tf_industrial_quad_relay_v2_set_selected_value(&quad_relay_bricklet.device, channel, false);

        if (retval != TF_E_OK) {
            logger.printfln("Industrial quad relay set monoflop or value failed for channel %d (rc %d).", channel, retval);
            return;
        }
    }
}

void PhaseSwitcher::contactor_check()
{
    static Config *evse_state = api.getState("evse/state", false);

    if (evse_state == nullptr)
        return;

    bool input_phase[4], output_phase[4], value[4];
    bool contactor_error[4];
    int retval;

    retval = tf_industrial_digital_in_4_v2_get_value(&digital_in_bricklet.device, value);
    if (retval != TF_E_OK) {
        logger.printfln("Industrial digital in relay get value failed (rc %d).", retval);
        return;
    }
    input_phase[1] = (evse_state->get("contactor_state")->asUint() == 3);
    input_phase[2] = value[2];
    input_phase[3] = value[3];

    retval = tf_industrial_quad_relay_v2_get_value(&quad_relay_bricklet.device, value);
    if (retval != TF_E_OK) {
        logger.printfln("Industrial quad relay get value failed (rc %d).", retval);
        return;
    }
    output_phase[1] = value[1];
    output_phase[2] = value[2];
    output_phase[3] = value[3];

    static uint32_t watchdog_start[4];

    for (int i = 1; i <= 3; i++){
        if (input_phase[i] == output_phase[i]) watchdog_start[i] = millis();
        if (deadline_elapsed(watchdog_start[i] + 2000) && !this->contactor_error){
            logger.printfln("Phase switcher: Contactor error phase %d", i);
            contactor_error[i] = true;
            this->contactor_error = true;
        } else {
            contactor_error[i] = false;
        }
    }

    if (this->contactor_error){
        switch(sequencer_state){
            case waiting_for_evse_start:
            case active:                    
            case quick_charging:
                logger.printfln("Phase switcher: Requesting EVSE to stop charging.");
                sequencer_state = waiting_for_evse_stop;
                break;
            case waiting_for_evse_stop:
                break;
            default:
                sequencer_state = inactive; 
                break;
        }
    }

    if (charger_state == not_connected && !contactor_error[0] && !contactor_error[1] && !contactor_error[2])
        this->contactor_error = false;
}

void PhaseSwitcher::update_all_data()
{
    phase_switcher_state.get("available_charging_power")->updateUint(phase_switcher_available_charging_power.get("power")->asUint());
    phase_switcher_state.get("requested_phases")->updateUint(requested_phases);
    phase_switcher_state.get("requested_phases_pending")->updateUint(requested_phases_pending);
    phase_switcher_state.get("active_phases")->updateUint(get_active_phases());
    phase_switcher_state.get("sequencer_state")->updateUint(uint8_t(sequencer_state));
    phase_switcher_state.get("time_since_state_change")->updateUint((millis() - last_state_change) / 1000);
    if (requested_phases_pending > requested_phases){
        phase_switcher_state.get("delay_time")->updateUint(min((int)(millis() - last_phase_request_change) / 1000, (int)phase_switcher_config_in_use.get("delay_time_more_phases")->asUint()));
    } else if (requested_phases_pending < requested_phases){
        phase_switcher_state.get("delay_time")->updateUint(min((int)(millis() - last_phase_request_change) / 1000, (int)phase_switcher_config_in_use.get("delay_time_less_phases")->asUint()));
    } else {
        phase_switcher_state.get("delay_time")->updateUint(0);
    }
    phase_switcher_state.get("contactor_state")->updateBool(contactor_error);
    
}

void PhaseSwitcher::update_history()
{
    int16_t actual_charging_power = -1;
    if (modbus_meter.initialized){
        static Config *meter_values = api.getState("meter/values", false);

        if (meter_values != nullptr)
            actual_charging_power = meter_values->get("power")->asFloat();
    }

    requested_power_history.push((int16_t)available_charging_power);
    charging_power_history.push((int16_t)(actual_charging_power));
    active_phases_history.push((int16_t)(requested_phases * 230 * 6));
}
