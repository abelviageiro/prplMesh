/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Copyright (c) 2016-2019 Intel Corporation
 *
 * This code is subject to the terms of the BSD+Patent license.
 * See LICENSE file for more details.
 */
#include "son_slave_thread.h"

#include "../monitor/monitor_thread.h"

#include <beerocks/bcl/beerocks_utils.h>
#include <beerocks/bcl/beerocks_version.h>
#include <beerocks/bcl/network/network_utils.h>
#include <beerocks/bcl/son/son_wireless_utils.h>
#include <easylogging++.h>

#include <beerocks/tlvf/beerocks_message.h>
#include <beerocks/tlvf/beerocks_message_apmanager.h>
#include <beerocks/tlvf/beerocks_message_backhaul.h>
#include <beerocks/tlvf/beerocks_message_control.h>
#include <beerocks/tlvf/beerocks_message_monitor.h>
#include <beerocks/tlvf/beerocks_message_platform.h>
#include <beerocks/tlvf/beerocks_wsc.h>

#include <tlvf/ieee_1905_1/tlvWscM1.h>
#include <tlvf/ieee_1905_1/tlvWscM2.h>
#include <tlvf/wfa_map/tlvApRadioBasicCapabilities.h>
#include <tlvf/wfa_map/tlvApRadioIdentifier.h>
#include <tlvf/wfa_map/tlvChannelPreference.h>

// BPL Error Codes
#include <bpl/bpl_cfg.h>
#include <bpl/bpl_err.h>

//////////////////////////////////////////////////////////////////////////////
/////////////////////////// Local Module Functions ///////////////////////////
//////////////////////////////////////////////////////////////////////////////

// TODO: Should be moved somewhere else?
static bwl::WiFiSec platform_to_bwl_security(const std::string &sec)
{
    if (!sec.compare("None")) {
        return bwl::WiFiSec::None;
    } else if (!sec.compare("WEP-64")) {
        return bwl::WiFiSec::WEP_64;
    } else if (!sec.compare("WEP-128")) {
        return bwl::WiFiSec::WEP_128;
    } else if (!sec.compare("WPA-Personal")) {
        return bwl::WiFiSec::WPA_PSK;
    } else if (!sec.compare("WPA2-Personal")) {
        return bwl::WiFiSec::WPA2_PSK;
    } else if (!sec.compare("WPA-WPA2-Personal")) {
        return bwl::WiFiSec::WPA_WPA2_PSK;
    } else {
        return bwl::WiFiSec::Invalid;
    }
}

//////////////////////////////////////////////////////////////////////////////
/////////////////////////////// Implementation ///////////////////////////////
//////////////////////////////////////////////////////////////////////////////

#define SLAVE_STATE_CONTINUE() call_slave_select = false

using namespace beerocks;
using namespace net;
using namespace son;

slave_thread::slave_thread(sSlaveConfig conf, beerocks::logging &logger_)
    : socket_thread(conf.temp_path + std::string(BEEROCKS_SLAVE_UDS) + +"_" + conf.hostap_iface),
      config(conf), logger(logger_)
{
    thread_name = "son_slave_" + conf.hostap_iface;
    slave_uds   = conf.temp_path + std::string(BEEROCKS_SLAVE_UDS) + "_" + conf.hostap_iface;
    backhaul_manager_uds    = conf.temp_path + std::string(BEEROCKS_BACKHAUL_MGR_UDS);
    platform_manager_uds    = conf.temp_path + std::string(BEEROCKS_PLAT_MGR_UDS);
    ap_manager              = nullptr;
    backhaul_manager_socket = nullptr;
    master_socket           = nullptr;
    monitor_socket          = nullptr;
    ap_manager_socket       = nullptr;
    platform_manager_socket = nullptr;
    configuration_stop_on_failure_attempts = conf.stop_on_failure_attempts;
    stop_on_failure_attempts               = configuration_stop_on_failure_attempts;

    slave_state = STATE_INIT;
    set_select_timeout(SELECT_TIMEOUT_MSEC);
}

slave_thread::~slave_thread()
{
    LOG(DEBUG) << "destructor - slave_reset()";
    stop_slave_thread();
}

bool slave_thread::init()
{
    LOG(INFO) << "Slave Info:";
    LOG(INFO) << "hostap_iface=" << config.hostap_iface;
    LOG(INFO) << "hostap_iface_type=" << config.hostap_iface_type;
    LOG(INFO) << "platform=" << int(config.platform);
    LOG(INFO) << "ruid=" << config.radio_identifier;

    if (config.hostap_iface_type == beerocks::IFACE_TYPE_UNSUPPORTED) {
        LOG(ERROR) << "hostap_iface_type '" << config.hostap_iface_type << "' UNSUPPORTED!";
        return false;
    }

    return socket_thread::init();
}

void slave_thread::stop_slave_thread()
{
    LOG(DEBUG) << "stop_slave_thread()";
    slave_reset();
    should_stop = true;
}

void slave_thread::slave_reset()
{
    slave_resets_counter++;
    LOG(DEBUG) << "slave_reset() #" << slave_resets_counter << " - start";
    if (!detach_on_conf_change) {
        backhaul_manager_stop();
    }
    platform_manager_stop();
    hostap_services_off();
    ap_manager_stop();
    monitor_stop();
    pending_iface_actions.clear();
    is_backhaul_manager            = false;
    iface_status_operational_state = false;
    detach_on_conf_change          = false;

    if (configuration_stop_on_failure_attempts && !stop_on_failure_attempts) {
        LOG(ERROR) << "Reached to max stop on failure attempts!";
        stopped = true;
    }

    if ((stopped) && (!is_credentials_changed_on_db) && (slave_state != STATE_INIT)) {
        platform_notify_error(BPL_ERR_SLAVE_STOPPED, "");
        LOG(DEBUG) << "goto STATE_STOPPED";
        slave_state = STATE_STOPPED;
    } else if (is_credentials_changed_on_db || is_backhaul_disconnected) {
        slave_state_timer =
            std::chrono::steady_clock::now() + std::chrono::seconds(SLAVE_INIT_DELAY_SEC);
        LOG(DEBUG) << "goto STATE_WAIT_BEFORE_INIT";
        slave_state = STATE_WAIT_BERFORE_INIT;
    } else {
        LOG(DEBUG) << "goto STATE_INIT";
        slave_state = STATE_INIT;
    }

    is_slave_reset = true;
    LOG(DEBUG) << "slave_reset() #" << slave_resets_counter << " - done";
}

void slave_thread::platform_notify_error(int code, const std::string &error_data)
{
    if (platform_manager_socket == nullptr) {
        LOG(ERROR) << "Invalid Platform Manager socket!";
        return;
    }

    auto error =
        message_com::create_vs_message<beerocks_message::cACTION_PLATFORM_ERROR_NOTIFICATION>(
            cmdu_tx);

    if (error == nullptr) {
        LOG(ERROR) << "Failed building message!";
        return;
    }

    error->code() = code;
    string_utils::copy_string(error->data(0), error_data.c_str(),
                              message::PLATFORM_ERROR_DATA_SIZE);

    // Send the message
    message_com::send_cmdu(platform_manager_socket, cmdu_tx);
}

void slave_thread::on_thread_stop() { stop_slave_thread(); }

bool slave_thread::socket_disconnected(Socket *sd)
{
    if (slave_state == STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE ||
        slave_state == STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE ||
        slave_state == STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE) {
        LOG(DEBUG) << "WIFI_CONFIGURATION_UPDATE is in progress, ignoring";
        detach_on_conf_change = true;
        if (sd == ap_manager_socket || sd == monitor_socket) {
            ap_manager_stop();
            monitor_stop();
            return false;
        }
        return true;
    }

    auto ap_manager_err_code_to_bpl_err_code = [](int code) -> int {
        if (code == ap_manager_thread::eThreadErrors::APMANAGER_THREAD_ERROR_HOSTAP_DISABLED) {
            return BPL_ERR_AP_MANAGER_HOSTAP_DISABLED;
        } else if (code == ap_manager_thread::eThreadErrors::APMANAGER_THREAD_ERROR_ATTACH_FAIL) {
            return BPL_ERR_AP_MANAGER_ATTACH_FAIL;
        } else if (code == ap_manager_thread::eThreadErrors::APMANAGER_THREAD_ERROR_SUDDEN_DETACH) {
            return BPL_ERR_AP_MANAGER_SUDDEN_DETACH;
        } else if (code ==
                   ap_manager_thread::eThreadErrors::APMANAGER_THREAD_ERROR_HAL_DISCONNECTED) {
            return BPL_ERR_AP_MANAGER_HAL_DISCONNECTED;
        } else if (code == ap_manager_thread::eThreadErrors::APMANAGER_THREAD_ERROR_CAC_TIMEOUT) {
            return BPL_ERR_AP_MANAGER_CAC_TIMEOUT;
        } else {
            return BPL_ERR_AP_MANAGER_DISCONNECTED;
        }
    };

    if (sd == backhaul_manager_socket) {
        LOG(DEBUG) << "backhaul manager & master socket disconnected! - slave_reset()";
        platform_notify_error(BPL_ERR_SLAVE_SLAVE_BACKHAUL_MANAGER_DISCONNECTED, "");
        stop_slave_thread();
        return false;
    } else if (sd == platform_manager_socket) {
        LOG(DEBUG) << "platform_manager disconnected! - slave_reset()";
        stop_slave_thread();
        return false;
    } else if (sd == ap_manager_socket || sd == monitor_socket) { //TODO WLANRTSYS-9119
        // if both ap_manager and monitor disconnected, but monitor disconnection got first
        auto err_code = ap_manager->get_thread_last_error_code();
        if (sd == ap_manager_socket ||
            err_code != ap_manager_thread::eThreadErrors::APMANAGER_THREAD_ERROR_NO_ERROR) {
            LOG(DEBUG) << "ap_manager socket disconnected, last error code " << int(err_code)
                       << "  - slave_reset()";
            if (!platform_settings.passive_mode_enabled) {
                stop_on_failure_attempts--;
                platform_notify_error(ap_manager_err_code_to_bpl_err_code(err_code), "");
            }
            slave_reset();
        } else {
            // only monitor disconnected
            LOG(DEBUG) << "monitor socket disconnected! - slave_reset()";
            if (!platform_settings.passive_mode_enabled) {
                stop_on_failure_attempts--;
                platform_notify_error(BPL_ERR_MONITOR_DISCONNECTED, "");
            }
            slave_reset();
        }

        return false;
    }

    return true;
}

std::string slave_thread::print_cmdu_types(const message::sUdsHeader *cmdu_header)
{
    return message_com::print_cmdu_types(cmdu_header);
}

bool slave_thread::work()
{
    bool call_slave_select = true;

    if (!monitor_heartbeat_check() || !ap_manager_heartbeat_check()) {
        slave_reset();
    }
    /*
     * wait for all pending iface actions to complete
     * otherwise, continue to FSM
     * no FSM until all actions are successful
     */
    if (!pending_iface_actions.empty()) {
        auto now = std::chrono::steady_clock::now();
        for (auto pending_action : pending_iface_actions) {
            std::string iface = pending_action.first;
            auto action       = pending_action.second;

            int time_elapsed_secs =
                std::chrono::duration_cast<std::chrono::seconds>(now - action.timestamp).count();
            if (time_elapsed_secs > IFACE_ACTION_TIMEOUT_SEC) {
                LOG(ERROR) << "iface " << iface << " operation: " << action.operation
                           << " timed out! " << time_elapsed_secs << " seconds passed";

                auto operation_to_err_code = [&](int8_t operation) -> int {
                    if (WIFI_IFACE_OPER_DISABLE == operation) {
                        return BPL_ERR_SLAVE_TIMEOUT_IFACE_ENABLE_REQUEST;
                    } else if (WIFI_IFACE_OPER_ENABLE == operation) {
                        return BPL_ERR_SLAVE_TIMEOUT_IFACE_DISABLE_REQUEST;
                    } else if (WIFI_IFACE_OPER_RESTORE == operation) {
                        return BPL_ERR_SLAVE_TIMEOUT_IFACE_RESTORE_REQUEST;
                    } else if (WIFI_IFACE_OPER_RESTART == operation) {
                        return BPL_ERR_SLAVE_TIMEOUT_IFACE_RESTART_REQUEST;
                    } else {
                        LOG(ERROR) << "ERROR: Unexpected operation:" << operation;
                        return BPL_ERR_NONE;
                    }
                };

                if (operation_to_err_code(action.operation) != BPL_ERR_NONE) {
                    platform_notify_error(operation_to_err_code(action.operation), iface.c_str());
                }

                LOG(DEBUG) << "reset slave";
                stop_on_failure_attempts--;
                slave_reset();
                break;
            }
        }
    } else {
        if (!slave_fsm(call_slave_select)) {
            return false;
        }
        if (config.enable_bpl_iface_status_notifications && platform_manager_socket &&
            !platform_settings.onboarding) {
            send_iface_status();
        }
    }
    if (call_slave_select) {
        if (!socket_thread::work()) {
            return false;
        }
    }
    return true;
}

void slave_thread::process_keep_alive()
{
    if (!config.enable_keep_alive || !son_config.slave_keep_alive_retries) {
        return;
    }

    if (master_socket == nullptr) {
        LOG(ERROR) << "process_keep_alive(): master_socket is nullptr!";
        return;
    }

    auto now = std::chrono::steady_clock::now();
    int keep_alive_time_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - master_last_seen).count();
    if (keep_alive_time_elapsed_ms >= beerocks::KEEP_ALIVE_INTERVAL_MSC) {
        if (keep_alive_retries >= son_config.slave_keep_alive_retries) {
            LOG(DEBUG) << "exceeded keep_alive_retries " << keep_alive_retries
                       << " - slave_reset()";

            platform_notify_error(BPL_ERR_SLAVE_MASTER_KEEP_ALIVE_TIMEOUT,
                                  "Reached master keep-alive retries limit: " +
                                      std::to_string(keep_alive_retries));

            stop_on_failure_attempts--;
            slave_reset();
        } else {
            LOG(DEBUG) << "time elapsed since last master message: " << keep_alive_time_elapsed_ms
                       << "ms, sending PING_MSG_REQUEST, tries=" << keep_alive_retries;
            auto request = message_com::create_vs_message<
                beerocks_message::cACTION_CONTROL_AGENT_PING_REQUEST>(cmdu_tx);
            if (request == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return;
            }

            request->total() = 1;
            request->seq()   = 0;
            request->size()  = 0;

            send_cmdu_to_controller(cmdu_tx);
            keep_alive_retries++;
            master_last_seen = now;
        }
    }
}

void slave_thread::update_iface_status(bool is_ap, int8_t iface_status)
{
    if (iface_status == 1) {
        if (is_ap) {
            iface_status_ap = beerocks::eRadioStatus::AP_OK;
        } else { //BH
            iface_status_bh = beerocks::eRadioStatus::BH_SCAN;
        }
    } else if (iface_status == 0) {
        iface_status_bh = beerocks::eRadioStatus::OFF;
        iface_status_ap = beerocks::eRadioStatus::OFF;
    }
}

void slave_thread::send_iface_status()
{
    // LOG(DEBUG) << std::endl
    //            << "iface_status_ap_prev=" << int(iface_status_ap_prev) << ", iface_status_ap=" << int(iface_status_ap) << std::endl
    //            << "iface_status_bh_prev=" << int(iface_status_bh_prev) << ", iface_status_bh=" << int(iface_status_bh) << std::endl
    //            << "iface_status_bh_wired_prev=" << int(iface_status_bh_wired_prev) << ", iface_status_bh_wired_prev=" << int(iface_status_bh_wired_prev);

    if (iface_status_ap_prev != iface_status_ap || iface_status_bh_prev != iface_status_bh ||
        iface_status_bh_wired_prev != iface_status_bh_wired ||
        iface_status_operational_state_prev != iface_status_operational_state) {

        send_platform_iface_status_notif(iface_status_ap, iface_status_operational_state);
    }
}

bool slave_thread::handle_cmdu(Socket *sd, ieee1905_1::CmduMessageRx &cmdu_rx)
{
    if (cmdu_rx.getMessageType() == ieee1905_1::eMessageType::VENDOR_SPECIFIC_MESSAGE) {

        auto beerocks_header = message_com::parse_intel_vs_message(cmdu_rx);

        if (!beerocks_header) {
            LOG(ERROR) << "Not a vendor specific message";
            return false;
        }

        switch (beerocks_header->action()) {
        case beerocks_message::ACTION_CONTROL: {
            return handle_cmdu_control_message(sd, beerocks_header, cmdu_rx);
        } break;
        case beerocks_message::ACTION_BACKHAUL: {
            return handle_cmdu_backhaul_manager_message(sd, beerocks_header, cmdu_rx);
        } break;
        case beerocks_message::ACTION_PLATFORM: {
            return handle_cmdu_platform_manager_message(sd, beerocks_header, cmdu_rx);
        } break;
        case beerocks_message::ACTION_APMANAGER: {
            return handle_cmdu_ap_manager_message(sd, beerocks_header, cmdu_rx);
        } break;
        case beerocks_message::ACTION_MONITOR: {
            return handle_cmdu_monitor_message(sd, beerocks_header, cmdu_rx);
        } break;
        default: {
            LOG(ERROR) << "Unknown message, action: " << int(beerocks_header->action());
        }
        }
    } else { // IEEE 1905.1 message
        return handle_cmdu_control_ieee1905_1_message(sd, cmdu_rx);
    }
    return true;
}

////////////////////////////////////////////////////////////////////////
////////////////////////// HANDLE CMDU ACTIONS /////////////////////////
////////////////////////////////////////////////////////////////////////

bool slave_thread::handle_cmdu_control_ieee1905_1_message(Socket *sd,
                                                          ieee1905_1::CmduMessageRx &cmdu_rx)
{
    auto cmdu_message_type = cmdu_rx.getMessageType();

    if (master_socket == nullptr) {
        // LOG(WARNING) << "master_socket == nullptr";
        return true;
    } else if (master_socket != sd) {
        LOG(WARNING) << "Unknown socket, cmdu message type: " << int(cmdu_message_type);
        return true;
    }

    if (slave_state == STATE_STOPPED) {
        return true;
    }

    master_last_seen   = std::chrono::steady_clock::now();
    keep_alive_retries = 0;

    switch (cmdu_message_type) {
    case ieee1905_1::eMessageType::AP_AUTOCONFIGURATION_WSC_MESSAGE:
        return handle_autoconfiguration_wsc(sd, cmdu_rx);
    case ieee1905_1::eMessageType::CHANNEL_PREFERENCE_QUERY_MESSAGE:
        return handle_channel_preference_query(sd, cmdu_rx);
    default:
        LOG(ERROR) << "Unknown CMDU message type: " << std::hex << int(cmdu_message_type);
        return false;
    }

    return true;
}

bool slave_thread::handle_cmdu_control_message(
    Socket *sd, std::shared_ptr<beerocks_message::cACTION_HEADER> beerocks_header,
    ieee1905_1::CmduMessageRx &cmdu_rx)
{
    // LOG(DEBUG) << "handle_cmdu_control_message(), INTEL_VS: action=" + std::to_string(beerocks_header->action()) + ", action_op=" + std::to_string(beerocks_header->action_op());
    // LOG(DEBUG) << "received radio_mac=" << network_utils::mac_to_string(beerocks_header->radio_mac()) << ", local radio_mac=" << network_utils::mac_to_string(hostap_params.iface_mac);

    // to me or not to me, this is the question...
    if (beerocks_header->radio_mac() != hostap_params.iface_mac) {
        return true;
    }

    if (beerocks_header->direction() == beerocks::BEEROCKS_DIRECTION_CONTROLLER) {
        return true;
    }

    if (master_socket == nullptr) {
        // LOG(WARNING) << "master_socket == nullptr";
        return true;
    } else if (master_socket != sd) {
        LOG(WARNING) << "Unknown socket, ACTION_CONTROL action_op: "
                     << int(beerocks_header->action_op());
        return true;
    }

    if (slave_state == STATE_STOPPED) {
        return true;
    }

    master_last_seen   = std::chrono::steady_clock::now();
    keep_alive_retries = 0;

    switch (beerocks_header->action_op()) {
    case beerocks_message::ACTION_CONTROL_ARP_QUERY_REQUEST: {
        LOG(TRACE) << "ACTION_CONTROL_ARP_QUERY_REQUEST";
        auto request_in = cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_ARP_QUERY_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_ARP_QUERY_REQUEST failed";
            return false;
        }
        auto request_out =
            message_com::create_vs_message<beerocks_message::cACTION_PLATFORM_ARP_QUERY_REQUEST>(
                cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        // notify platform manager
        request_out->params() = request_in->params();
        message_com::send_cmdu(platform_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_SON_CONFIG_UPDATE: {
        LOG(DEBUG) << "received ACTION_CONTROL_SON_CONFIG_UPDATE";
        auto update = cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_SON_CONFIG_UPDATE>();
        if (update == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_SON_CONFIG_UPDATE failed";
            return false;
        }
        son_config = update->config();
        log_son_config();
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_REQUEST: {
        LOG(DEBUG) << "received ACTION_CONTROL_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_REQUEST";

        auto request_in = cmdu_rx.addClass<
            beerocks_message::cACTION_CONTROL_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR)
                << "addClass cACTION_CONTROL_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_REQUEST>(
            cmdu_tx);
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        LOG(DEBUG) << "send ACTION_APMANAGER_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_REQUEST";
        request_out->params() = request_in->params();
        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_CHANNEL_SWITCH_ACS_START: {
        LOG(DEBUG) << "received ACTION_CONTROL_HOSTAP_CHANNEL_SWITCH_ACS_START";
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_HOSTAP_CHANNEL_SWITCH_ACS_START>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_HOSTAP_CHANNEL_SWITCH_ACS_START failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_HOSTAP_CHANNEL_SWITCH_ACS_START>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        LOG(DEBUG) << "send cACTION_APMANAGER_HOSTAP_CHANNEL_SWITCH_ACS_START";
        request_out->cs_params() = request_in->cs_params();
        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_START_MONITORING_REQUEST: {
        LOG(DEBUG) << "received ACTION_CONTROL_CLIENT_START_MONITORING_REQUEST";
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_START_MONITORING_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_START_MONITORING_REQUEST failed";
            return false;
        }

        std::string client_mac = network_utils::mac_to_string(request_in->params().mac);
        std::string client_bridge_4addr_mac =
            network_utils::mac_to_string(request_in->params().bridge_4addr_mac);
        std::string client_ip = network_utils::ipv4_to_string(request_in->params().ipv4);

        LOG(DEBUG) << "START_MONITORING_REQUEST: mac=" << client_mac << " ip=" << client_ip
                   << " bridge_4addr_mac=" << client_bridge_4addr_mac;

        if (request_in->params().is_ire) {
            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_APMANAGER_CLIENT_IRE_CONNECTED_NOTIFICATION>(cmdu_tx);
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building ACTION_APMANAGER_CLIENT_IRE_CONNECTED_NOTIFICATION "
                              "message!";
                return false;
            }
            request_out->mac() = request_in->params().mac;
            message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        }

        //notify monitor
        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_CLIENT_START_MONITORING_REQUEST>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_MONITOR_CLIENT_START_MONITORING_REQUEST message!";
            return false;
        }
        request_out->params() = request_in->params();
        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }

    case beerocks_message::ACTION_CONTROL_CLIENT_STOP_MONITORING_REQUEST: {
        LOG(DEBUG) << "received ACTION_CONTROL_CLIENT_STOP_MONITORING_REQUEST";
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_STOP_MONITORING_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_STOP_MONITORING_REQUEST failed";
            return false;
        }
        std::string client_mac = network_utils::mac_to_string(request_in->mac());

        LOG(DEBUG) << "STOP_MONITORING_REQUEST: mac=" << client_mac;

        //notify monitor
        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_CLIENT_STOP_MONITORING_REQUEST>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_MONITOR_CLIENT_STOP_MONITORING_REQUEST message!";
            return false;
        }

        request_out->mac() = request_in->mac();
        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }

    case beerocks_message::ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_REQUEST: {
        LOG(DEBUG) << "received ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_REQUEST";

        auto request_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_REQUEST failed";
            return false;
        }
        auto hostap_mac = network_utils::mac_to_string(request_in->params().mac);
        bool forbackhaul =
            (is_backhaul_manager && backhaul_params.backhaul_is_wireless) ? true : false;

        if (request_in->params().cross && (request_in->params().ipv4.oct[0] == 0) &&
            forbackhaul) { //if backhaul manager and wireless send to backhaul else front.
            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_REQUEST>(
                cmdu_tx, beerocks_header->id());
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_REQUEST "
                              "message!";
                return false;
            }

            request_out->params() = request_in->params();
            message_com::send_cmdu(backhaul_manager_socket, cmdu_tx);
        } else if (request_in->params().cross &&
                   (request_in->params().ipv4.oct[0] ==
                    0)) { // unconnected client cross --> send to ap_manager
            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_REQUEST>(
                cmdu_tx, beerocks_header->id());
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building ACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_REQUEST "
                              "message!";
                return false;
            }
            request_out->params() = request_in->params();
            message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        } else {
            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_REQUEST>(
                cmdu_tx, beerocks_header->id());
            if (request_out == nullptr) {
                LOG(ERROR)
                    << "Failed building ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_REQUEST message!";
                return false;
            }
            request_out->params() = request_in->params();
            message_com::send_cmdu(monitor_socket, cmdu_tx);
        }

        LOG(INFO) << "rx_rssi measurement request for client mac="
                  << network_utils::mac_to_string(request_in->params().mac)
                  << " ip=" << network_utils::ipv4_to_string(request_in->params().ipv4)
                  << " channel=" << int(request_in->params().channel) << " bandwidth="
                  << utils::convert_bandwidth_to_int(
                         (beerocks::eWiFiBandwidth)request_in->params().bandwidth)
                  << " cross=" << int(request_in->params().cross)
                  << " id=" << int(beerocks_header->id());
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_DISALLOW_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_DISALLOW_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_DISALLOW_REQUEST failed";
            return false;
        }

        std::string mac = network_utils::mac_to_string(request_in->mac());
        LOG(INFO) << "CLIENT_DISALLOW mac " << mac
                  << ", reject_sta=" << int(request_in->reject_sta());

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_CLIENT_DISALLOW_REQUEST>(cmdu_tx,
                                                                         beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_APMANAGER_CLIENT_DISALLOW_REQUEST message!";
            return false;
        }
        request_out->mac()        = request_in->mac();
        request_out->reject_sta() = request_in->reject_sta();
        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_ALLOW_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_ALLOW_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_ALLOW_REQUEST failed";
            return false;
        }

        std::string sta_mac = network_utils::mac_to_string(request_in->mac());
        LOG(DEBUG) << "CLIENT_ALLOW, mac = " << sta_mac
                   << ", ip = " << network_utils::ipv4_to_string(request_in->ipv4());

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_CLIENT_ALLOW_REQUEST>(cmdu_tx,
                                                                      beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_APMANAGER_CLIENT_ALLOW_REQUEST message!";
            return false;
        }
        request_out->mac()  = request_in->mac();
        request_out->ipv4() = request_in->ipv4();
        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_DISCONNECT_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_DISCONNECT_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_DISCONNECT_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_CLIENT_DISCONNECT_REQUEST>(cmdu_tx,
                                                                           beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_APMANAGER_CLIENT_DISCONNECT_REQUEST message!";
            return false;
        }

        request_out->mac()    = request_in->mac();
        request_out->vap_id() = request_in->vap_id();
        request_out->type()   = request_in->type();
        request_out->reason() = request_in->reason();

        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_BSS_STEER_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_BSS_STEER_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_BSS_STEER_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_CLIENT_BSS_STEER_REQUEST>(cmdu_tx,
                                                                          beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_APMANAGER_CLIENT_BSS_STEER_REQUEST message!";
            return false;
        }
        request_out->params() = request_in->params();
        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CONTROLLER_PING_REQUEST: {
        LOG(DEBUG) << "received ACTION_CONTROL_CONTROLLER_PING_REQUEST";
        auto request =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CONTROLLER_PING_REQUEST>();
        if (request == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_CONTROLLER_PING_REQUEST failed";
            return false;
        }

        auto response = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CONTROLLER_PING_RESPONSE>(cmdu_tx);
        if (response == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        response->total() = request->total();
        response->seq()   = request->seq();
        response->size()  = request->size();

        if (response->size()) {
            if (!response->alloc_data(request->size())) {
                LOG(ERROR) << "Failed buffer allocation to size=" << int(request->size());
                break;
            }
            auto data_tuple = response->data(0);
            memset(&std::get<1>(data_tuple), 0, response->size());
        }
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_AGENT_PING_RESPONSE: {
        LOG(DEBUG) << "received ACTION_CONTROL_AGENT_PING_RESPONSE";
        auto response = cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_AGENT_PING_RESPONSE>();
        if (response == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_AGENT_PING_RESPONSE failed";
            return false;
        }
        if (response->seq() < (response->total() - 1)) { //send next ping request
            auto request = message_com::create_vs_message<
                beerocks_message::cACTION_CONTROL_AGENT_PING_REQUEST>(cmdu_tx);
            if (request == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            request->total() = response->total();
            request->seq()   = response->seq() + 1;
            request->size()  = response->size();
            if (request->size()) {
                if (!request->alloc_data(request->size())) {
                    LOG(ERROR) << "Failed buffer allocation to size=" << int(request->size());
                    break;
                }
                auto data_tuple = request->data(0);
                memset(&std::get<1>(data_tuple), 0, request->size());
            }
            send_cmdu_to_controller(cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_CONTROL_CHANGE_MODULE_LOGGING_LEVEL: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CHANGE_MODULE_LOGGING_LEVEL>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_CHANGE_MODULE_LOGGING_LEVEL failed";
            return false;
        }
        bool all = false;
        if (request_in->params().module_name == beerocks::BEEROCKS_PROCESS_ALL) {
            all = true;
        }
        if (all || request_in->params().module_name == beerocks::BEEROCKS_PROCESS_SLAVE) {
            logger.set_log_level_state((eLogLevel)request_in->params().log_level,
                                       request_in->params().enable);
        }
        if (all || request_in->params().module_name == beerocks::BEEROCKS_PROCESS_MONITOR) {
            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_MONITOR_CHANGE_MODULE_LOGGING_LEVEL>(cmdu_tx);
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }
            request_out->params() = request_in->params();
            message_com::send_cmdu(monitor_socket, cmdu_tx);
        }
        if (all || request_in->params().module_name == beerocks::BEEROCKS_PROCESS_PLATFORM) {
            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_PLATFORM_CHANGE_MODULE_LOGGING_LEVEL>(cmdu_tx);
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }
            request_out->params() = request_in->params();
            message_com::send_cmdu(platform_manager_socket, cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_CONTROL_BACKHAUL_ROAM_REQUEST: {
        LOG(TRACE) << "received ACTION_CONTROL_BACKHAUL_ROAM_REQUEST";
        if (is_backhaul_manager && backhaul_params.backhaul_is_wireless) {
            auto request_in =
                cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_BACKHAUL_ROAM_REQUEST>();
            if (request_in == nullptr) {
                LOG(ERROR) << "addClass cACTION_CONTROL_BACKHAUL_ROAM_REQUEST failed";
                return false;
            }
            auto bssid = network_utils::mac_to_string(request_in->params().bssid);
            LOG(DEBUG) << "reconfigure wpa_supplicant to bssid " << bssid
                       << " channel=" << int(request_in->params().channel);

            auto request_out =
                message_com::create_vs_message<beerocks_message::cACTION_BACKHAUL_ROAM_REQUEST>(
                    cmdu_tx, beerocks_header->id());
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }
            request_out->params() = request_in->params();
            message_com::send_cmdu(backhaul_manager_socket, cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_CONTROL_BACKHAUL_RESET: {
        LOG(TRACE) << "received ACTION_CONTROL_BACKHAUL_RESET";
        auto request =
            message_com::create_vs_message<beerocks_message::cACTION_BACKHAUL_RESET>(cmdu_tx);
        if (request == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        message_com::send_cmdu(backhaul_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_TX_ON_REQUEST: {
        LOG(TRACE) << "received ACTION_CONTROL_HOSTAP_TX_ON_REQUEST";
        set_radio_tx_enable(config.hostap_iface, true);
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_TX_OFF_REQUEST: {
        LOG(TRACE) << "received ACTION_CONTROL_HOSTAP_TX_OFF_REQUEST";
        set_radio_tx_enable(config.hostap_iface, false);
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_STATS_MEASUREMENT_REQUEST: {
        if (monitor_socket) {
            // LOG(TRACE) << "received ACTION_CONTROL_HOSTAP_STATS_MEASUREMENT_REQUEST"; // floods the log
            auto request_in =
                cmdu_rx
                    .addClass<beerocks_message::cACTION_CONTROL_HOSTAP_STATS_MEASUREMENT_REQUEST>();
            if (request_in == nullptr) {
                LOG(ERROR) << "addClass cACTION_CONTROL_HOSTAP_STATS_MEASUREMENT_REQUEST failed";
                return false;
            }

            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_MONITOR_HOSTAP_STATS_MEASUREMENT_REQUEST>(
                cmdu_tx, beerocks_header->id());
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }
            request_out->sync() = request_in->sync();
            message_com::send_cmdu(monitor_socket, cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_SET_NEIGHBOR_11K_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_HOSTAP_SET_NEIGHBOR_11K_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_HOSTAP_SET_NEIGHBOR_11K_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_HOSTAP_SET_NEIGHBOR_11K_REQUEST>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        request_out->params() = request_in->params();
        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_REMOVE_NEIGHBOR_11K_REQUEST: {
        auto request_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_CONTROL_HOSTAP_REMOVE_NEIGHBOR_11K_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_HOSTAP_REMOVE_NEIGHBOR_11K_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_HOSTAP_REMOVE_NEIGHBOR_11K_REQUEST>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        request_out->params() = request_in->params();
        message_com::send_cmdu(ap_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_BEACON_11K_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_BEACON_11K_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_BEACON_11K_REQUEST failed";
            return false;
        }
        //LOG(DEBUG) << "ACTION_CONTROL_CLIENT_BEACON_11K_REQUEST";
        // override ssid in case of:
        if (request_in->params().use_optional_ssid &&
            std::string((char *)request_in->params().ssid).empty()) {
            //LOG(DEBUG) << "ssid field is empty! using slave ssid -> " << config.ssid;
            string_utils::copy_string((char *)request_in->params().ssid,
                                      platform_settings.front_ssid, message::WIFI_SSID_MAX_LENGTH);
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_CLIENT_BEACON_11K_REQUEST>(cmdu_tx,
                                                                         beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_MONITOR_CLIENT_BEACON_11K_REQUEST message!";
            return false;
        }
        request_out->params() = request_in->params();
        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_CHANNEL_LOAD_11K_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_CHANNEL_LOAD_11K_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_CHANNEL_LOAD_11K_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_CLIENT_CHANNEL_LOAD_11K_REQUEST>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_MONITOR_CLIENT_CHANNEL_LOAD_11K_REQUEST message!";
            return false;
        }

        request_out->params() = request_in->params();
        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_STATISTICS_11K_REQUEST: {
        auto request_in =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_CLIENT_STATISTICS_11K_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_STATISTICS_11K_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_CLIENT_STATISTICS_11K_REQUEST>(cmdu_tx,
                                                                             beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_MONITOR_CLIENT_STATISTICS_11K_REQUEST message!";
            return false;
        }

        request_out->params() = request_in->params();
        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_CLIENT_LINK_MEASUREMENT_11K_REQUEST: {
        auto request_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_CONTROL_CLIENT_LINK_MEASUREMENT_11K_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_CONTROL_CLIENT_LINK_MEASUREMENT_11K_REQUEST failed";
            return false;
        }

        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_CLIENT_LINK_MEASUREMENT_11K_REQUEST>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_MONITOR_CLIENT_LINK_MEASUREMENT_11K_REQUEST message!";
            return false;
        }

        request_out->mac() = request_in->mac();
        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }

    case beerocks_message::ACTION_CONTROL_HOSTAP_UPDATE_STOP_ON_FAILURE_ATTEMPTS_REQUEST: {
        auto request_in = cmdu_rx.addClass<
            beerocks_message::cACTION_CONTROL_HOSTAP_UPDATE_STOP_ON_FAILURE_ATTEMPTS_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR)
                << "addClass cACTION_CONTROL_HOSTAP_UPDATE_STOP_ON_FAILURE_ATTEMPTS_REQUEST failed";
            return false;
        }
        configuration_stop_on_failure_attempts = request_in->attempts();
        LOG(DEBUG) << "stop_on_failure_attempts new value: "
                   << configuration_stop_on_failure_attempts;

        if (is_backhaul_manager) {
            auto request_out = message_com::create_vs_message<
                beerocks_message::cACTION_BACKHAUL_UPDATE_STOP_ON_FAILURE_ATTEMPTS_REQUEST>(
                cmdu_tx);
            if (request_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            request_out->attempts() = request_in->attempts();
            message_com::send_cmdu(backhaul_manager_socket, cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_CONTROL_HOSTAP_DISABLED_BY_MASTER: {
        LOG(DEBUG) << "ACTION_CONTROL_HOSTAP_DISABLED_BY_MASTER, marking slave as operational!";
        iface_status_operational_state = true;
        break;
    }
    case beerocks_message::ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PREPARE_REQUEST: {
        LOG(TRACE) << "ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PREPARE_REQUEST - ID: "
                   << beerocks_header->id();
        auto request_in = cmdu_rx.addClass<
            beerocks_message::cACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PREPARE_REQUEST>();
        if (request_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PREPARE_REQUEST failed";
            return false;
        }
        new_credentials  = request_in->params();
        auto request_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PREPARE_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (request_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PRE_COMMIT_REQUEST: {
        LOG(TRACE) << "ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PRE_COMMIT_REQUEST - ID: "
                   << beerocks_header->id();
        if (new_credentials.ssid[0] == '\0') {
            LOG(ERROR) << "New sWifiCredentials is not valid";
        } else {
            auto bpl_request = message_com::create_vs_message<
                beerocks_message::cACTION_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_REQUEST>(
                cmdu_tx, beerocks_header->id());
            if (bpl_request == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }
            bpl_request->params() = new_credentials;
            LOG(INFO) << "Sending WiFi credentials update request to platform manager";
            message_com::send_cmdu(platform_manager_socket, cmdu_tx);

            // response
            auto response = message_com::create_vs_message<
                beerocks_message::cACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_PRE_COMMIT_RESPONSE>(
                cmdu_tx, beerocks_header->id());
            if (response == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            send_cmdu_to_controller(cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_COMMIT_REQUEST: {
        LOG(TRACE) << "ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_COMMIT_REQUEST";

        slave_state_timer =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(
                STATE_WAIT_FOR_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE_TIMEOUT_SEC);
        LOG(DEBUG) << "goto STATE_WAIT_FOR_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE";
        slave_state = STATE_WAIT_FOR_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE;
        break;
    }
    case beerocks_message::ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_ABORT_REQUEST: {
        LOG(TRACE) << "ACTION_CONTROL_WIFI_CREDENTIALS_UPDATE_ABORT_REQUEST";
        if (is_credentials_changed_on_db) {
            auto bpl_request = message_com::create_vs_message<
                beerocks_message::cACTION_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_REQUEST>(
                cmdu_tx, beerocks_header->id());
            if (bpl_request == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }
            string_utils::copy_string(new_credentials.ssid, platform_settings.front_ssid,
                                      sizeof(new_credentials.ssid));
            string_utils::copy_string(new_credentials.pass, platform_settings.front_pass,
                                      sizeof(new_credentials.pass));
            std::string sec_str(platform_settings.front_security_type);

            if (sec_str == BPL_WLAN_SEC_NONE_STR) {
                new_credentials.sec = beerocks_message::eWiFiSec_None;
            } else if (sec_str == BPL_WLAN_SEC_WEP64_STR) {
                new_credentials.sec = beerocks_message::eWiFiSec_WEP64;
            } else if (sec_str == BPL_WLAN_SEC_WEP128_STR) {
                new_credentials.sec = beerocks_message::eWiFiSec_WEP128;
            } else if (sec_str == BPL_WLAN_SEC_WPA_PSK_STR) {
                new_credentials.sec = beerocks_message::eWiFiSec_WPA_PSK;
            } else if (sec_str == BPL_WLAN_SEC_WPA2_PSK_STR) {
                new_credentials.sec = beerocks_message::eWiFiSec_WPA2_PSK;
            } else if (sec_str == BPL_WLAN_SEC_WPA_WPA2_PSK_STR) {
                new_credentials.sec = beerocks_message::eWiFiSec_WPA_WPA2_PSK;
            } else {
                LOG(WARNING) << "Unsupported Wi-Fi Security: " << sec_str
                             << " credentials rollover failed!";
                break;
            }

            bpl_request->params() = new_credentials;
            LOG(INFO) << "Sending WiFi credentials update request to platform manager";
            message_com::send_cmdu(platform_manager_socket, cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_CONTROL_VERSION_MISMATCH_NOTIFICATION: {
        LOG(TRACE) << "ACTION_CONTROL_VERSION_MISMATCH_NOTIFICATION";
        auto notification =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_VERSION_MISMATCH_NOTIFICATION>();
        if (notification == nullptr) {
            LOG(ERROR) << "addClass failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_PLATFORM_VERSION_MISMATCH_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());

        if (notification_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_REQUES message!";
            break;
        }

        string_utils::copy_string(notification_out->versions().master_version,
                                  notification->versions().master_version,
                                  sizeof(notification_out->versions().master_version));
        string_utils::copy_string(notification_out->versions().slave_version,
                                  notification->versions().slave_version,
                                  sizeof(notification_out->versions().slave_version));

        message_com::send_cmdu(platform_manager_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_STEERING_CLIENT_SET_GROUP_REQUEST: {
        LOG(TRACE) << "ACTION_CONTROL_STEERING_CLIENT_SET_GROUP_REQUEST";
        auto update =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_STEERING_CLIENT_SET_GROUP_REQUEST>();
        if (update == nullptr) {
            LOG(ERROR) << "addClass failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_STEERING_CLIENT_SET_GROUP_REQUEST>(
            cmdu_tx, beerocks_header->id());

        if (notification_out == nullptr) {
            LOG(ERROR)
                << "Failed building cACTION_MONITOR_STEERING_CLIENT_SET_GROUP_REQUEST message!";
            break;
        }
        notification_out->params() = update->params();

        LOG(DEBUG) << std::endl
                   << "remove = " << int(update->params().remove) << std::endl
                   << "steeringGroupIndex = " << update->params().steeringGroupIndex << std::endl
                   << "bssid = " << network_utils::mac_to_string(update->params().cfg.bssid)
                   << std::endl
                   << "utilCheckIntervalSec = " << update->params().cfg.utilCheckIntervalSec
                   << std::endl
                   << "utilAvgCount = " << update->params().cfg.utilAvgCount << std::endl
                   << "inactCheckIntervalSec = " << update->params().cfg.inactCheckIntervalSec
                   << std::endl
                   << "inactCheckThresholdSec = " << update->params().cfg.inactCheckThresholdSec
                   << std::endl;

        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_CONTROL_STEERING_CLIENT_SET_REQUEST: {
        LOG(TRACE) << "ACTION_CONTROL_STEERING_CLIENT_SET_REQUEST";
        auto update =
            cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_STEERING_CLIENT_SET_REQUEST>();
        if (update == nullptr) {
            LOG(ERROR) << "addClass failed";
            return false;
        }

        // send to Monitor
        auto notification_mon_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_STEERING_CLIENT_SET_REQUEST>(cmdu_tx,
                                                                           beerocks_header->id());

        if (notification_mon_out == nullptr) {
            LOG(ERROR) << "Failed building cACTION_MONITOR_STEERING_CLIENT_SET_REQUEST message!";
            break;
        }

        notification_mon_out->params() = update->params();

        message_com::send_cmdu(monitor_socket, cmdu_tx);

        // send to AP MANAGER
        auto notification_ap_out = message_com::create_vs_message<
            beerocks_message::cACTION_APMANAGER_STEERING_CLIENT_SET_REQUEST>(cmdu_tx,
                                                                             beerocks_header->id());

        if (notification_ap_out == nullptr) {
            LOG(ERROR) << "Failed building cACTION_APMANAGER_STEERING_CLIENT_SET_REQUEST message!";
            break;
        }

        notification_ap_out->params() = update->params();

        message_com::send_cmdu(ap_manager_socket, cmdu_tx);

        LOG(DEBUG) << std::endl
                   << "remove = " << notification_ap_out->params().remove << std::endl
                   << "steeringGroupIndex = " << notification_ap_out->params().steeringGroupIndex
                   << std::endl
                   << "client_mac = "
                   << network_utils::mac_to_string(notification_ap_out->params().client_mac)
                   << std::endl
                   << "bssid = " << network_utils::mac_to_string(update->params().bssid)
                   << std::endl
                   << "config.snrProbeHWM = " << notification_ap_out->params().config.snrProbeHWM
                   << std::endl
                   << "config.snrProbeLWM = " << notification_ap_out->params().config.snrProbeLWM
                   << std::endl
                   << "config.snrAuthHWM = " << notification_ap_out->params().config.snrAuthHWM
                   << std::endl
                   << "config.snrAuthLWM = " << notification_ap_out->params().config.snrAuthLWM
                   << std::endl
                   << "config.snrInactXing = " << notification_ap_out->params().config.snrInactXing
                   << std::endl
                   << "config.snrHighXing = " << notification_ap_out->params().config.snrHighXing
                   << std::endl
                   << "config.snrLowXing = " << notification_ap_out->params().config.snrLowXing
                   << std::endl
                   << "config.authRejectReason = "
                   << notification_ap_out->params().config.authRejectReason << std::endl;

        break;
    }
    default: {
        LOG(ERROR) << "Unknown CONTROL message, action_op: " << int(beerocks_header->action_op());
        return false;
    }
    }

    return true;
}

bool slave_thread::handle_cmdu_backhaul_manager_message(
    Socket *sd, std::shared_ptr<beerocks_message::cACTION_HEADER> beerocks_header,
    ieee1905_1::CmduMessageRx &cmdu_rx)
{
    if (backhaul_manager_socket == nullptr) {
        LOG(ERROR) << "backhaul_socket == nullptr";
        return true;
    } else if (backhaul_manager_socket != sd) {
        LOG(ERROR) << "Unknown socket, ACTION_BACKHAUL action_op: "
                   << int(beerocks_header->action_op());
        return true;
    }

    switch (beerocks_header->action_op()) {
    case beerocks_message::ACTION_BACKHAUL_REGISTER_RESPONSE: {
        LOG(DEBUG) << "ACTION_BACKHAUL_REGISTER_RESPONSE";
        if (slave_state == STATE_WAIT_FOR_BACKHAUL_MANAGER_REGISTER_RESPONSE) {
            auto response =
                cmdu_rx.addClass<beerocks_message::cACTION_BACKHAUL_REGISTER_RESPONSE>();
            if (!response) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }
            LOG(DEBUG) << "goto STATE_JOIN_INIT";
            slave_state = STATE_JOIN_INIT;
        } else {
            LOG(ERROR) << "slave_state != STATE_WAIT_FOR_BACKHAUL_MANAGER_REGISTER_RESPONSE";
        }
        break;
    }

    case beerocks_message::ACTION_BACKHAUL_CONNECTED_NOTIFICATION: {

        auto notification =
            cmdu_rx.addClass<beerocks_message::cACTION_BACKHAUL_CONNECTED_NOTIFICATION>();
        if (!notification) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        LOG(DEBUG) << "ACTION_BACKHAUL_CONNECTED_NOTIFICATION";

        if (slave_state >= STATE_WAIT_FOR_BACKHAUL_MANAGER_CONNECTED_NOTIFICATION &&
            slave_state <= STATE_OPERATIONAL) {

            // Already sent join_master request, mark as reconfiguration
            if (slave_state >= STATE_WAIT_FOR_JOINED_RESPONSE && slave_state <= STATE_OPERATIONAL)
                is_backhual_reconf = true;

            is_backhaul_manager = (bool)notification->params().is_backhaul_manager;
            LOG_IF(is_backhaul_manager, DEBUG) << "Selected as backhaul manager";

            backhaul_params.gw_ipv4 = network_utils::ipv4_to_string(notification->params().gw_ipv4);
            backhaul_params.gw_bridge_mac =
                network_utils::mac_to_string(notification->params().gw_bridge_mac);
            backhaul_params.controller_bridge_mac =
                network_utils::mac_to_string(notification->params().controller_bridge_mac);
            backhaul_params.bridge_mac =
                network_utils::mac_to_string(notification->params().bridge_mac);
            backhaul_params.bridge_ipv4 =
                network_utils::ipv4_to_string(notification->params().bridge_ipv4);
            backhaul_params.backhaul_mac =
                network_utils::mac_to_string(notification->params().backhaul_mac);
            backhaul_params.backhaul_ipv4 =
                network_utils::ipv4_to_string(notification->params().backhaul_ipv4);
            backhaul_params.backhaul_bssid =
                network_utils::mac_to_string(notification->params().backhaul_bssid);
            // backhaul_params.backhaul_freq        = notification->params.backhaul_freq; // HACK temp disabled because of a bug on endian converter
            backhaul_params.backhaul_channel     = notification->params().backhaul_channel;
            backhaul_params.backhaul_is_wireless = notification->params().backhaul_is_wireless;
            backhaul_params.backhaul_iface_type  = notification->params().backhaul_iface_type;

            std::copy_n(notification->params().backhaul_scan_measurement_list,
                        beerocks::message::BACKHAUL_SCAN_MEASUREMENT_MAX_LENGTH,
                        backhaul_params.backhaul_scan_measurement_list);

            for (unsigned int i = 0; i < message::BACKHAUL_SCAN_MEASUREMENT_MAX_LENGTH; i++) {
                if (backhaul_params.backhaul_scan_measurement_list[i].channel > 0) {
                    LOG(DEBUG) << "mac = "
                               << network_utils::mac_to_string(
                                      backhaul_params.backhaul_scan_measurement_list[i].mac)
                               << " channel = "
                               << int(backhaul_params.backhaul_scan_measurement_list[i].channel)
                               << " rssi = "
                               << int(backhaul_params.backhaul_scan_measurement_list[i].rssi);
                }
            }

            if (notification->params().backhaul_is_wireless) {
                backhaul_params.backhaul_iface = config.backhaul_wireless_iface;
            } else {
                backhaul_params.backhaul_iface = config.backhaul_wire_iface;
            }
            //Radio status
            if (is_backhaul_manager) {
                if (notification->params().backhaul_is_wireless) {
                    iface_status_bh =
                        eRadioStatus::BH_SIGNAL_OK; //TODO - send according to the RSSI
                    iface_status_bh_wired = eRadioStatus::OFF;
                } else {
                    iface_status_bh       = eRadioStatus::OFF;
                    iface_status_bh_wired = eRadioStatus::BH_WIRED;
                }
            } else {
                iface_status_bh       = eRadioStatus::OFF;
                iface_status_bh_wired = eRadioStatus::OFF;
            }
            LOG(DEBUG) << "goto STATE_BACKHAUL_MANAGER_CONNECTED";
            slave_state = STATE_BACKHAUL_MANAGER_CONNECTED;

        } else {
            LOG(WARNING) << "slave_state != STATE_WAIT_FOR_BACKHAUL_CONNECTED_NOTIFICATION";
        }
        break;
    }
    case beerocks_message::ACTION_BACKHAUL_BUSY_NOTIFICATION: {
        if (slave_state != STATE_WAIT_FOR_BACKHAUL_MANAGER_CONNECTED_NOTIFICATION) {
            LOG(WARNING) << "slave_state != STATE_WAIT_FOR_BACKHAUL_CONNECTED_NOTIFICATION";
            break;
        }

        slave_state_timer = std::chrono::steady_clock::now() +
                            std::chrono::seconds(WAIT_BEFORE_SEND_BH_ENABLE_NOTIFICATION_SEC);

        LOG(DEBUG) << "goto STATE_WAIT_BACKHAUL_MANAGER_BUSY";
        slave_state = STATE_WAIT_BACKHAUL_MANAGER_BUSY;

        break;
    }
    case beerocks_message::ACTION_BACKHAUL_DISCONNECTED_NOTIFICATION: {

        if (is_slave_reset)
            break;

        LOG(DEBUG) << "ACTION_BACKHAUL_DISCONNECTED_NOTIFICATION";

        auto notification =
            cmdu_rx.addClass<beerocks_message::cACTION_BACKHAUL_DISCONNECTED_NOTIFICATION>();
        if (!notification) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        stopped |= bool(notification->stopped());

        is_backhaul_disconnected       = true;
        iface_status_operational_state = false;
        update_iface_status(false, false);

        slave_state_timer =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(beerocks::IRE_MAX_WIRELESS_RECONNECTION_TIME_MSC);

        master_socket = nullptr;

        if (slave_state == STATE_WAIT_FOR_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE) {
            break;
        }

        slave_reset();
        break;
    }
    case beerocks_message::ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE: {
        LOG(DEBUG) << "ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE";

        auto response_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE>();
        if (!response_in) {
            LOG(ERROR)
                << "Failed building ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE message!";
            return false;
        }

        LOG(DEBUG) << "ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE mac="
                   << network_utils::mac_to_string(response_in->params().result.mac)
                   << " rx_rssi=" << int(response_in->params().rx_rssi)
                   << " id=" << int(beerocks_header->id());

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE>(
            cmdu_tx, beerocks_header->id());

        if (response_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE message!";
            break;
        }

        response_out->params()            = response_in->params();
        response_out->params().src_module = beerocks::BEEROCKS_ENTITY_BACKHAUL_MANAGER;
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE: {
        LOG(DEBUG) << "ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE";
        auto response_in = cmdu_rx.addClass<
            beerocks_message::cACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE>();
        if (!response_in) {
            LOG(ERROR) << "Failed building ACTION_BACKHAUL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE "
                          "message!";
            return false;
        }

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE>(
            cmdu_tx, beerocks_header->id());

        if (!response_out) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE "
                          "message!";
            break;
        }
        response_out->mac() = response_in->mac();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }

    case beerocks_message::ACTION_BACKHAUL_DL_RSSI_REPORT_NOTIFICATION: {
        LOG(DEBUG) << "ACTION_BACKHAUL_DL_RSSI_REPORT_NOTIFICATION";
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_BACKHAUL_DL_RSSI_REPORT_NOTIFICATION>();
        if (!notification_in) {
            LOG(ERROR) << "Failed building ACTION_BACKHAUL_DL_RSSI_REPORT_NOTIFICATION message!";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_BACKHAUL_DL_RSSI_REPORT_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());

        if (!notification_out) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_BACKHAUL_DL_RSSI_REPORT_NOTIFICATION message!";
            break;
        }

        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);

        int rssi = notification_in->params().rssi;

        if (abs(last_reported_backhaul_rssi - rssi) >= BH_SIGNAL_RSSI_THRESHOLD_HYSTERESIS) {
            last_reported_backhaul_rssi = rssi;

            if (rssi < BH_SIGNAL_RSSI_THRESHOLD_LOW) {
                iface_status_bh = eRadioStatus::BH_SIGNAL_TOO_LOW;
            } else if (rssi >= BH_SIGNAL_RSSI_THRESHOLD_LOW &&
                       rssi < BH_SIGNAL_RSSI_THRESHOLD_HIGH) {
                iface_status_bh = eRadioStatus::BH_SIGNAL_OK;
            } else {
                iface_status_bh = eRadioStatus::BH_SIGNAL_TOO_HIGH;
            }
        }

        break;
    }
    default: {
        LOG(ERROR) << "Unknown BACKHAUL_MANAGER message, action_op: "
                   << int(beerocks_header->action_op());
        return false;
    }
    }

    return true;
}

bool slave_thread::handle_cmdu_platform_manager_message(
    Socket *sd, std::shared_ptr<beerocks_message::cACTION_HEADER> beerocks_header,
    ieee1905_1::CmduMessageRx &cmdu_rx)
{
    if (platform_manager_socket != sd) {
        LOG(ERROR) << "Unknown socket, ACTION_PLATFORM_MANAGER action_op: "
                   << int(beerocks_header->action_op());
        return true;
    }

    switch (beerocks_header->action_op()) {
    case beerocks_message::ACTION_PLATFORM_ADVERTISE_SSID_FLAG_UPDATE_RESPONSE: {
        auto response =
            cmdu_rx
                .addClass<beerocks_message::cACTION_PLATFORM_ADVERTISE_SSID_FLAG_UPDATE_RESPONSE>();
        if (response == nullptr) {
            LOG(ERROR) << "addClass cACTION_PLATFORM_ADVERTISE_SSID_FLAG_UPDATE_RESPONSE failed";
            return false;
        }
        bool success = response->result();
        LOG(DEBUG) << "received ACTION_PLATFORM_SET_ADVERTISE_SSID_FLAG_UPDATE_RESPONSE "
                   << (success ? "success" : "failure");
        break;
    }
    case beerocks_message::ACTION_PLATFORM_SON_SLAVE_REGISTER_RESPONSE: {
        LOG(TRACE) << "ACTION_PLATFORM_SON_SLAVE_REGISTER_RESPONSE";
        if (slave_state == STATE_WAIT_FOR_PLATFORM_MANAGER_REGISTER_RESPONSE) {
            auto response =
                cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_SON_SLAVE_REGISTER_RESPONSE>();
            if (response == nullptr) {
                LOG(ERROR) << "addClass cACTION_PLATFORM_SON_SLAVE_REGISTER_RESPONSE failed";
                return false;
            }
            // Configuration is invalid
            if (response->valid() == 0) {
                LOG(ERROR) << "response->valid == 0";
                platform_notify_error(BPL_ERR_CONFIG_PLATFORM_REPORTED_INVALID_CONFIGURATION, "");
                stop_on_failure_attempts--;
                slave_reset();
                return true;
            }

            platform_settings = response->platform_settings();
            wlan_settings     = response->wlan_settings();

            LOG(INFO) << "local_master=" << (int)platform_settings.local_master;
            LOG(INFO) << "local_gw=" << (int)platform_settings.local_gw;

            // check if wlan wifi credentials are same as beerocks wifi credentials
            if (!strncmp(wlan_settings.ssid, platform_settings.front_ssid,
                         message::WIFI_SSID_MAX_LENGTH) &&
                !strncmp(wlan_settings.pass, platform_settings.front_pass,
                         message::WIFI_PASS_MAX_LENGTH) &&
                !strncmp(wlan_settings.security_type, platform_settings.front_security_type,
                         message::WIFI_SECURITY_TYPE_MAX_LENGTH)) {

                LOG(DEBUG) << "wlan credentials unification is not required";
                is_wlan_credentials_unified = true;

            } else {
                LOG(DEBUG) << "wlan credentials unification is required:";
                LOG(DEBUG) << "wlan ssid:" << wlan_settings.ssid
                           << ", platform front ssid:" << platform_settings.front_ssid;
                LOG(DEBUG) << "wlan security type:" << wlan_settings.security_type
                           << ", platform front security type:"
                           << platform_settings.front_security_type;
                if (config.enable_credentials_automatic_unify) {
                    is_wlan_credentials_unified = false;
                } else {
                    LOG(DEBUG) << "wlan credentials unification SKIPPED - "
                                  "enable_credentials_automatic_unify is set to disable in "
                                  "slave-config file";
                }
            }

            LOG(TRACE) << "goto STATE_CONNECT_TO_BACKHAUL_MANAGER";
            slave_state = STATE_CONNECT_TO_BACKHAUL_MANAGER;
        } else {
            LOG(ERROR) << "slave_state != STATE_WAIT_FOR_PLATFORM_MANAGER_REGISTER_RESPONSE";
        }
        break;
    }

    case beerocks_message::ACTION_PLATFORM_GET_WLAN_READY_STATUS_RESPONSE: {
        LOG(TRACE) << "received ACTION_PLATFORM_GET_WLAN_READY_STATUS_RESPONSE";
        if (slave_state == STATE_WAIT_FOR_WLAN_READY_STATUS_RESPONSE) {
            auto response =
                cmdu_rx
                    .addClass<beerocks_message::cACTION_PLATFORM_GET_WLAN_READY_STATUS_RESPONSE>();
            if (response == nullptr) {
                LOG(ERROR) << "addClass cACTION_PLATFORM_GET_WLAN_READY_STATUS_RESPONSE failed";
                return false;
            }
            bool success = (1 == response->result()) ? true : false;

            LOG(DEBUG) << "received ACTION_PLATFORM_GET_WLAN_READY_STATUS_RESPONSE, result="
                       << (success ? "success" : "failure");

            if (success) {
                LOG(TRACE) << "goto STATE_JOIN_INIT_BRING_UP_INTERFACES";
                slave_state = STATE_JOIN_INIT_BRING_UP_INTERFACES;
            } else {
                // LOG(TRACE) << "goto STATE_GET_WLAN_READY_STATUS";
                slave_state = STATE_GET_WLAN_READY_STATUS;
            }
        } else {
            LOG(ERROR) << "slave_state != STATE_WAIT_FOR_WLAN_READY_STATUS_RESPONSE";
        }
        break;
    }

    case beerocks_message::ACTION_PLATFORM_WIFI_SET_IFACE_STATE_RESPONSE: {
        auto response =
            cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_WIFI_SET_IFACE_STATE_RESPONSE>();
        if (response == nullptr) {
            LOG(ERROR) << "addClass cACTION_PLATFORM_WIFI_SET_IFACE_STATE_RESPONSE failed";
            return false;
        }
        std::string iface = response->iface_name(message::IFACE_NAME_LENGTH);

        auto operation_to_string = [&](int8_t operation) -> std::string {
            if (WIFI_IFACE_OPER_NO_CHANGE == operation) {
                return "not change";
            } else if (WIFI_IFACE_OPER_DISABLE == operation) {
                return "disable";
            } else if (WIFI_IFACE_OPER_ENABLE == operation) {
                return "enable";
            } else if (WIFI_IFACE_OPER_RESTORE == operation) {
                return "restore";
            } else if (WIFI_IFACE_OPER_RESTART == operation) {
                return "restart";
            } else {
                return "ERROR! unknown operation!";
            }
        };

        bool success = (response->success() != 0);
        LOG(DEBUG) << "received ACTION_PLATFORM_WIFI_SET_IFACE_STATE_RESPONSE for iface=" << iface
                   << ", operation:" << operation_to_string(response->iface_operation()) << ", "
                   << (success ? "success" : "failure");

        if (success) {
            pending_iface_actions.erase(iface);
            if (WIFI_IFACE_OPER_NO_CHANGE != response->iface_operation()) {
                update_iface_status(
                    (ap_manager_socket != nullptr),
                    ((WIFI_IFACE_OPER_DISABLE == response->iface_operation()) ? false : true));
            }
        } else {
            platform_notify_error(BPL_ERR_SLAVE_IFACE_CHANGE_STATE_FAILED, iface.c_str());
            stop_on_failure_attempts--;
            slave_reset();
        }
        break;
    }

    case beerocks_message::ACTION_PLATFORM_WIFI_CREDENTIALS_SET_RESPONSE: {
        LOG(TRACE) << "received ACTION_PLATFORM_WIFI_CREDENTIALS_SET_RESPONSE";
        if (slave_state == STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE) {
            auto response =
                cmdu_rx
                    .addClass<beerocks_message::cACTION_PLATFORM_WIFI_CREDENTIALS_SET_RESPONSE>();
            if (response == nullptr) {
                LOG(ERROR) << "addClass cACTION_PLATFORM_WIFI_CREDENTIALS_SET_RESPONSE failed";
                return false;
            }
            std::string iface = response->iface_name(message::IFACE_NAME_LENGTH);
            bool success      = (1 == response->success()) ? true : false;

            LOG(DEBUG) << "set wifi credentials result=" << (success ? "success" : "failure");

            is_wlan_credentials_unified = (success) ? true : false;

            if (!success) {
                platform_notify_error(BPL_ERR_SLAVE_WIFI_CREDENTIALS_SET_FAILED, iface.c_str());
                stop_on_failure_attempts--;
                LOG(DEBUG) << "set wifi credentials failed, slave reset!";
                slave_reset();
            } else {
                if (detach_on_conf_change) {
                    LOG(DEBUG) << "detach occurred on wifi conf change, slave reset!";
                    slave_reset();
                } else {
                    LOG(DEBUG) << "credentials set finished successfully";
                    LOG(DEBUG) << "goto STATE_START_MONITOR";
                    slave_state = STATE_START_MONITOR;
                }
            }
        } else {
            LOG(DEBUG) << "slave_state != STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE";
        }
        break;
    }

    case beerocks_message::ACTION_PLATFORM_POST_INIT_CONFIG_RESPONSE: {
        LOG(TRACE) << "received ACTION_PLATFORM_POST_INIT_CONFIG_RESPONSE";
        //make sure slave reset didn't occur while we performed post init configurations
        if (slave_state == STATE_OPERATIONAL) {
            auto response =
                cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_POST_INIT_CONFIG_RESPONSE>();
            if (response == nullptr) {
                LOG(ERROR) << "addClass cACTION_PLATFORM_POST_INIT_CONFIG_RESPONSE failed";
                return false;
            }
            bool success = (1 == response->result()) ? true : false;

            LOG(DEBUG) << "post init config result=" << (success ? "success" : "failure");

            if (!success) {
                platform_notify_error(BPL_ERR_SLAVE_POST_INIT_CONFIG_FAILED,
                                      config.hostap_iface.c_str());
                stop_on_failure_attempts--;
                LOG(DEBUG) << "post init configurations failed, slave reset!";
                slave_reset();
            }
        } else {
            LOG(DEBUG) << "slave_state != STATE_OPERATIONAL";
        }
    } break;

    case beerocks_message::ACTION_PLATFORM_WIFI_SET_RADIO_TX_STATE_RESPONSE: {
        auto response =
            cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_WIFI_SET_RADIO_TX_STATE_RESPONSE>();
        if (response == nullptr) {
            LOG(ERROR) << "addClass cACTION_PLATFORM_WIFI_SET_RADIO_TX_STATE_RESPONSE failed";
            return false;
        }
        LOG(DEBUG) << "received ACTION_PLATFORM_WIFI_SET_RADIO_TX_STATE_RESPONSE iface="
                   << response->iface_name(message::IFACE_NAME_LENGTH)
                   << (response->enable() ? " enable" : " disable")
                   << (response->success() ? " success" : " failure");

        if (!response->success()) {
            LOG(ERROR) << "slave reset, RADIO_TX_STATE fail";
            stop_on_failure_attempts--;
            platform_notify_error(BPL_ERR_SLAVE_TX_CHANGE_STATE_FAILED,
                                  response->iface_name(message::IFACE_NAME_LENGTH));
            slave_reset();
        } else {
            update_iface_status((ap_manager_socket != nullptr), response->enable());

            if (master_socket && response->enable()) {
                auto notification = message_com::create_vs_message<
                    beerocks_message::cACTION_CONTROL_HOSTAP_TX_ON_RESPONSE>(cmdu_tx);

                if (notification == nullptr) {
                    LOG(ERROR) << "Failed building message!";
                    return false;
                }

                send_cmdu_to_controller(cmdu_tx);
            }
        }
        break;
    }
    case beerocks_message::ACTION_PLATFORM_ARP_MONITOR_NOTIFICATION: {
        // LOG(TRACE) << "ACTION_PLATFORM_ARP_MONITOR_NOTIFICATION";
        if (master_socket) {
            auto notification_in =
                cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_ARP_MONITOR_NOTIFICATION>();
            if (notification_in == nullptr) {
                LOG(ERROR) << "addClass cACTION_PLATFORM_ARP_MONITOR_NOTIFICATION failed";
                return false;
            }

            auto notification_out = message_com::create_vs_message<
                beerocks_message::cACTION_CONTROL_CLIENT_ARP_MONITOR_NOTIFICATION>(cmdu_tx);
            if (notification_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            notification_out->params() = notification_in->params();
            send_cmdu_to_controller(cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_PLATFORM_WLAN_PARAMS_CHANGED_NOTIFICATION: {
        LOG(TRACE) << "ACTION_PLATFORM_WLAN_PARAMS_CHANGED_NOTIFICATION";

        auto notification =
            cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_WLAN_PARAMS_CHANGED_NOTIFICATION>();
        if (notification == nullptr) {
            LOG(ERROR) << "addClass cACTION_PLATFORM_WLAN_PARAMS_CHANGED_NOTIFICATION failed";
            return false;
        }

        // slave only reacts to band_enabled change
        if (wlan_settings.band_enabled != notification->wlan_settings().band_enabled) {
            LOG(DEBUG) << "band_enabled changed - performing slave_reset()";
            slave_reset();
        }
        break;
    }
    case beerocks_message::ACTION_PLATFORM_OPERATIONAL_NOTIFICATION: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_OPERATIONAL_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_PLATFORM_OPERATIONAL_NOTIFICATION failed";
            return false;
        }

        LOG(DEBUG) << "sending master operational notification, new_oper_state="
                   << int(notification_in->operational())
                   << " bridge_mac=" << backhaul_params.bridge_mac;

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_PLATFORM_OPERATIONAL_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification_out->operational() = notification_in->operational();
        notification_out->bridge_mac() = network_utils::mac_from_string(backhaul_params.bridge_mac);
        if (master_socket) {
            send_cmdu_to_controller(cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_PLATFORM_DHCP_MONITOR_NOTIFICATION: {
        auto notification =
            cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_DHCP_MONITOR_NOTIFICATION>();
        if (notification == nullptr) {
            LOG(ERROR) << "addClass ACTION_PLATFORM_DHCP_MONITOR_NOTIFICATION failed";
            return false;
        }

        if (notification->op() == beerocks_message::eDHCPOp_Add ||
            notification->op() == beerocks_message::eDHCPOp_Old) {
            std::string client_mac = network_utils::mac_to_string(notification->mac());
            std::string client_ip  = network_utils::ipv4_to_string(notification->ipv4());

            LOG(DEBUG) << "ACTION_DHCP_LEASE_ADDED_NOTIFICATION mac " << client_mac
                       << " ip = " << client_ip << " name="
                       << std::string(notification->hostname(message::NODE_NAME_LENGTH));

            // notify master
            if (master_socket) {
                auto master_notification = message_com::create_vs_message<
                    beerocks_message::cACTION_CONTROL_CLIENT_DHCP_COMPLETE_NOTIFICATION>(cmdu_tx);
                if (master_notification == nullptr) {
                    LOG(ERROR) << "Failed building message!";
                    return false;
                }

                master_notification->mac()  = notification->mac();
                master_notification->ipv4() = notification->ipv4();
                string_utils::copy_string(master_notification->name(message::NODE_NAME_LENGTH),
                                          notification->hostname(message::NODE_NAME_LENGTH),
                                          message::NODE_NAME_LENGTH);
                send_cmdu_to_controller(cmdu_tx);
            }

        } else {
            LOG(DEBUG) << "ACTION_PLATFORM_DHCP_MONITOR_NOTIFICATION op " << notification->op()
                       << " mac " << network_utils::mac_to_string(notification->mac())
                       << " ip = " << network_utils::ipv4_to_string(notification->ipv4());
        }
        break;
    }
    case beerocks_message::ACTION_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE: {
        LOG(TRACE) << "ACTION_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE";
        auto response = cmdu_rx.addClass<
            beerocks_message::cACTION_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE>();
        if (response == nullptr) {
            LOG(ERROR) << "addClass ACTION_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE failed";
            return false;
        }
        if (response->result()) {
            is_credentials_changed_on_db = true;
        } else {
            LOG(ERROR) << "platform manager failed to update wifi credentials on DB!!!";
            is_credentials_changed_on_db = false;
            platform_notify_error(BPL_ERR_SLAVE_UPDATE_CREDENTIALS_FAILED, "");
            stop_on_failure_attempts--;
            slave_reset();
        }
        break;
    }
    case beerocks_message::ACTION_PLATFORM_WIFI_CONFIGURATION_UPDATE_REQUEST: {
        auto response =
            cmdu_rx
                .addClass<beerocks_message::cACTION_PLATFORM_WIFI_CONFIGURATION_UPDATE_REQUEST>();
        if (response == nullptr) {
            LOG(ERROR) << "addClass cACTION_PLATFORM_WIFI_CONFIGURATION_UPDATE_REQUEST failed";
            return false;
        }
        LOG(INFO) << "ACTION_PLATFORM_WIFI_CONFIGURATION_UPDATE_REQUEST config_start="
                  << int(response->config_start());

        if (slave_state == STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE) {
            LOG(DEBUG) << "slave wifi credentials set in progress - ignore wifi configuration "
                          "notification";
        } else if (slave_state != STATE_OPERATIONAL &&
                   slave_state != STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE &&
                   slave_state != STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE) {
            LOG(DEBUG) << "invalid slave state - ignore wifi configuration notification";
        } else if (!response->config_start()) {
            LOG(DEBUG) << "WIFI_CONFIGURATION_UPDATE_COMPLETE";
            if (detach_on_conf_change) {
                LOG(DEBUG) << "detach occurred on wifi conf change, slave reset!";
                slave_reset();
            } else if (
                master_socket) { // if backhaul disconnects before we get WIFI_CONFIGURATION_UPDATE_COMPLETE, so the slave will continue on its current state
                LOG(DEBUG) << "WIFI_CONFIGURATION_UPDATE_COMPLETE! goto STATE_OPERATIONAL";
                slave_state = STATE_OPERATIONAL;
            }

        } else if (slave_state == STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE) {
            // response->config_start == 1, but prev conf change is not finished yet
            slave_state_timer =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(beerocks::SON_SLAVE_WAIT_AFTER_WIFI_CONFIG_UPDATE_SEC);
            LOG(DEBUG) << "goto STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE";
            slave_state = STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE;

        } else { // response->config_start == 1, new conf update request
            slave_state_timer =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE_TIMEOUT_SEC);
            LOG(DEBUG) << "goto STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE";
            slave_state = STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE;
        }
        break;
    }
    case beerocks_message::ACTION_PLATFORM_ARP_QUERY_RESPONSE: {
        LOG(TRACE) << "ACTION_PLATFORM_ARP_QUERY_RESPONSE";
        if (master_socket) {
            auto response =
                cmdu_rx.addClass<beerocks_message::cACTION_PLATFORM_ARP_QUERY_RESPONSE>();
            if (response == nullptr) {
                LOG(ERROR) << "addClass cACTION_PLATFORM_ARP_QUERY_RESPONSE failed";
                return false;
            }

            auto response_out = message_com::create_vs_message<
                beerocks_message::cACTION_CONTROL_ARP_QUERY_RESPONSE>(cmdu_tx,
                                                                      beerocks_header->id());
            if (response_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            response_out->params() = response->params();
            send_cmdu_to_controller(cmdu_tx);
        }
        break;
    }

    default: {
        LOG(ERROR) << "Unknown PLATFORM_MANAGER message, action_op: "
                   << int(beerocks_header->action_op());
        return false;
    }
    }

    return true;
}

bool slave_thread::handle_cmdu_ap_manager_message(
    Socket *sd, std::shared_ptr<beerocks_message::cACTION_HEADER> beerocks_header,
    ieee1905_1::CmduMessageRx &cmdu_rx)
{
    if (ap_manager_socket == nullptr) {
        if (beerocks_header->action_op() !=
            beerocks_message::ACTION_APMANAGER_INIT_DONE_NOTIFICATION) {
            LOG(ERROR) << "Not ACTION_APMANAGER_INIT_DONE_NOTIFICATION, action_op: "
                       << int(beerocks_header->action_op());
            return true;
        }
    } else if (ap_manager_socket != sd) {
        LOG(ERROR) << "Unknown socket, ACTION_APMANAGER action_op: "
                   << int(beerocks_header->action_op())
                   << ", ap_manager_socket=" << intptr_t(ap_manager_socket)
                   << ", incoming sd=" << intptr_t(sd);
        return true;
    } else if (beerocks_header->action_op() ==
               beerocks_message::ACTION_APMANAGER_HEARTBEAT_NOTIFICATION) {
        ap_manager_last_seen       = std::chrono::steady_clock::now();
        ap_manager_retries_counter = 0;
        return true;
    } else if (slave_state > STATE_BACKHAUL_MANAGER_CONNECTED && master_socket == nullptr) {
        LOG(ERROR) << "master_socket == nullptr ACTION_APMANAGER action_op: "
                   << int(beerocks_header->action_op());
    }

    switch (beerocks_header->action_op()) {
    case beerocks_message::ACTION_APMANAGER_INIT_DONE_NOTIFICATION: {
        LOG(INFO) << "received ACTION_APMANAGER_INIT_DONE_NOTIFICATION from sd=" << intptr_t(sd);
        ap_manager_socket = sd;
        slave_state       = STATE_WAIT_FOR_AP_MANAGER_JOINED;
        break;
    }
    case beerocks_message::ACTION_APMANAGER_JOINED_NOTIFICATION: {
        LOG(INFO) << "received ACTION_APMANAGER_JOINED_NOTIFICATION";
        auto notification =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_JOINED_NOTIFICATION>();
        if (notification == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_JOINED_NOTIFICATION failed";
            return false;
        }
        hostap_params    = notification->params();
        hostap_cs_params = notification->cs_params();
        if (slave_state == STATE_WAIT_FOR_AP_MANAGER_JOINED) {
            slave_state = STATE_AP_MANAGER_JOINED;
        } else {
            LOG(ERROR) << "ACTION_APMANAGER_JOINED_NOTIFICATION, slave_state != "
                          "STATE_WAIT_FOR_AP_MANAGER_JOINED";
        }
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_RESPONSE: {
        auto response_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass "
                          "cACTION_APMANAGER_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_RESPONSE "
                          "failed";
            return false;
        }
        LOG(INFO) << "received ACTION_APMANAGER_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_RESPONSE";

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_SET_RESTRICTED_FAILSAFE_CHANNEL_RESPONSE>(
            cmdu_tx);
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        response_out->success() = response_in->success();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_AP_DISABLED_NOTIFICATION: {
        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_HOSTAP_AP_DISABLED_NOTIFICATION>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_HOSTAP_AP_DISABLED_NOTIFICATION failed";
            return false;
        }
        LOG(INFO) << "received ACTION_APMANAGER_HOSTAP_AP_DISABLED_NOTIFICATION on vap_id="
                  << int(response_in->vap_id());
        if (response_in->vap_id() == beerocks::IFACE_RADIO_ID) {
            LOG(WARNING) << __FUNCTION__ << "AP_Disabled on radio, slave reset";
            if (slave_state == STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE ||
                slave_state == STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE ||
                slave_state == STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE) {
                LOG(INFO) << "WIFI_CONFIGURATION_UPDATE is in progress, ignoring";
                detach_on_conf_change = true;
            } else if (platform_settings.passive_mode_enabled == 0) {
                stop_on_failure_attempts--;
                platform_notify_error(BPL_ERR_AP_MANAGER_HOSTAP_DISABLED, config.hostap_iface);
            }
            slave_reset();
        } else {
            auto response_out = message_com::create_vs_message<
                beerocks_message::cACTION_CONTROL_HOSTAP_AP_DISABLED_NOTIFICATION>(cmdu_tx);
            if (response_out == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            response_out->vap_id() = response_in->vap_id();
            send_cmdu_to_controller(cmdu_tx);
        }
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_AP_ENABLED_NOTIFICATION: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_HOSTAP_AP_ENABLED_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_HOSTAP_AP_ENABLED_NOTIFICATION failed";
            return false;
        }
        LOG(INFO) << "received ACTION_APMANAGER_HOSTAP_AP_ENABLED_NOTIFICATION vap_id="
                  << int(notification_in->vap_id());

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_AP_ENABLED_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        notification_out->vap_id()   = notification_in->vap_id();
        notification_out->vap_info() = notification_in->vap_info();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_VAPS_LIST_UPDATE_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_HOSTAP_VAPS_LIST_UPDATE_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_HOSTAP_VAPS_LIST_UPDATE_NOTIFICATION failed";
            return false;
        }

        LOG(INFO) << "received ACTION_APMANAGER_HOSTAP_VAPS_LIST_UPDATE_NOTIFICATION";

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_VAPS_LIST_UPDATE_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_ACS_NOTIFICATION: {
        LOG(INFO) << "ACTION_APMANAGER_HOSTAP_ACS_NOTIFICATION";
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_HOSTAP_ACS_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_HOSTAP_CSA_ERROR_NOTIFICATION failed";
            return false;
        }
        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_ACS_NOTIFICATION>(cmdu_tx,
                                                                       beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification_out->cs_params()     = notification_in->cs_params();
        auto tuple_in_supported_channels  = notification_in->supported_channels_list(0);
        auto tuple_out_supported_channels = notification_out->supported_channels(0);
        std::copy_n(&std::get<1>(tuple_in_supported_channels), message::SUPPORTED_CHANNELS_LENGTH,
                    &std::get<1>(tuple_out_supported_channels));
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_CSA_NOTIFICATION: {
        LOG(INFO) << "ACTION_APMANAGER_HOSTAP_CSA_NOTIFICATION";

        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_HOSTAP_CSA_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_HOSTAP_CSA_ERROR_NOTIFICATION failed";
            return false;
        }
        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_CSA_NOTIFICATION>(cmdu_tx,
                                                                       beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        notification_out->cs_params() = notification_in->cs_params();
        send_cmdu_to_controller(cmdu_tx);

        if (wireless_utils::is_dfs_channel(hostap_cs_params.channel)) {
            LOG(INFO) << "AP is in DFS channel: " << (int)hostap_cs_params.channel;
            iface_status_ap = beerocks::eRadioStatus::AP_DFS_CAC;
        } else {
            iface_status_ap = beerocks::eRadioStatus::AP_OK;
        }

        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_CSA_ERROR_NOTIFICATION: {
        LOG(INFO) << "received ACTION_APMANAGER_HOSTAP_CSA_ERROR_NOTIFICATION";
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_HOSTAP_CSA_ERROR_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_HOSTAP_CSA_ERROR_NOTIFICATION failed";
            return false;
        }
        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_CSA_ERROR_NOTIFICATION>(cmdu_tx,
                                                                             beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification_out->cs_params() = notification_in->cs_params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE: {
        auto response_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE failed";
            return false;
        }
        LOG(INFO) << "APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE mac="
                  << network_utils::mac_to_string(response_in->params().result.mac)
                  << " rx_rssi=" << int(response_in->params().rx_rssi)
                  << " id=" << int(beerocks_header->id());

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE>(
            cmdu_tx, beerocks_header->id());

        if (response_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE message!";
            break;
        }

        response_out->params()            = response_in->params();
        response_out->params().src_module = beerocks::BEEROCKS_ENTITY_AP_MANAGER;
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_CLIENT_DISCONNECTED_NOTIFICATION: {
        auto notification_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_APMANAGER_CLIENT_DISCONNECTED_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_APMANAGER_CLIENT_DISCONNECTED_NOTIFICATION failed";
            return false;
        }

        std::string client_mac = network_utils::mac_to_string(notification_in->params().mac);
        LOG(INFO) << "client disconnected sta_mac=" << client_mac;

        //notify monitor
        {
            auto notification_out = message_com::create_vs_message<
                beerocks_message::cACTION_MONITOR_CLIENT_STOP_MONITORING_REQUEST>(
                cmdu_tx, beerocks_header->id());

            if (notification_out == nullptr) {
                LOG(ERROR)
                    << "Failed building cACTION_MONITOR_CLIENT_STOP_MONITORING_REQUEST message!";
                break;
            }
            notification_out->mac() = notification_in->params().mac;
            message_com::send_cmdu(monitor_socket, cmdu_tx);
        }
        //notify master
        // if the master is not connected, remove element from pending_client_association
        if (master_socket) {
            auto notification_out = message_com::create_vs_message<
                beerocks_message::cACTION_CONTROL_CLIENT_DISCONNECTED_NOTIFICATION>(
                cmdu_tx, beerocks_header->id());

            if (notification_out == nullptr) {
                LOG(ERROR)
                    << "Failed building ACTION_CONTROL_CLIENT_DISCONNECTED_NOTIFICATION message!";
                break;
            }
            notification_out->params() = notification_in->params();
            send_cmdu_to_controller(cmdu_tx);

        } else {
            pending_client_association_cmdu.erase(client_mac);
        }

        break;
    }
    case beerocks_message::ACTION_APMANAGER_CLIENT_BSS_STEER_RESPONSE: {
        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_CLIENT_BSS_STEER_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_APMANAGER_CLIENT_BSS_STEER_RESPONSE failed";
            return false;
        }
        LOG(INFO) << "ACTION_APMANAGER_CLIENT_BSS_STEER_RESPONSE, rep_mode="
                  << int(response_in->params().status_code);

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_BSS_STEER_RESPONSE>(cmdu_tx,
                                                                         beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_BSS_STEER_RESPONSE message!";
            break;
        }
        response_out->params() = response_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE: {
        auto response_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR)
                << "addClass ACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE failed";
            return false;
        }

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE "
                          "message!";
            break;
        }
        LOG(INFO) << "ACTION_APMANAGER_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE";
        response_out->mac() = response_in->mac();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_DFS_CAC_COMPLETED_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_HOSTAP_DFS_CAC_COMPLETED_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass sACTION_APMANAGER_HOSTAP_DFS_CAC_COMPLETED_NOTIFICATION failed";
            return false;
        }
        LOG(TRACE) << "received ACTION_APMANAGER_HOSTAP_DFS_CAC_COMPLETED_NOTIFICATION";

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_DFS_CAC_COMPLETED_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        iface_status_ap = beerocks::eRadioStatus::AP_OK;
        break;
    }
    case beerocks_message::ACTION_APMANAGER_HOSTAP_DFS_CHANNEL_AVAILABLE_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_HOSTAP_DFS_CHANNEL_AVAILABLE_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR)
                << "addClass cACTION_APMANAGER_HOSTAP_DFS_CHANNEL_AVAILABLE_NOTIFICATION failed";
            return false;
        }
        LOG(TRACE) << "received ACTION_APMANAGER_HOSTAP_DFS_CHANNEL_AVAILABLE_NOTIFICATION";

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_DFS_CHANNEL_AVAILABLE_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_CLIENT_ASSOCIATED_NOTIFICATION: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_CLIENT_ASSOCIATED_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_CLIENT_ASSOCIATED_NOTIFICATION failed";
            return false;
        }
        LOG(TRACE) << "received ACTION_APMANAGER_CLIENT_ASSOCIATED_NOTIFICATION";

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_ASSOCIATED_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        std::string client_mac = network_utils::mac_to_string(notification_in->params().mac);
        LOG(INFO) << "client associated sta_mac=" << client_mac;

        notification_out->params() = notification_in->params();

        // if the master is not connected, save the association notification
        if (master_socket) {
            send_cmdu_to_controller(cmdu_tx);
        } else {
            pending_client_association_cmdu[client_mac] = notification_out->params();
        }
        break;
    }
    case beerocks_message::ACTION_APMANAGER_STEERING_EVENT_PROBE_REQ_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_STEERING_EVENT_PROBE_REQ_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_STEERING_EVENT_PROBE_REQ_NOTIFICATION failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_STEERING_EVENT_PROBE_REQ_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }

    case beerocks_message::ACTION_APMANAGER_STEERING_EVENT_AUTH_FAIL_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_APMANAGER_STEERING_EVENT_AUTH_FAIL_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass "
                          "cACTION_APMANAGER_CLIENT_ScACTION_APMANAGER_STEERING_EVENT_AUTH_FAIL_"
                          "NOTIFICATIONOFTBLOCK_NOTIFICATION failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_STEERING_EVENT_AUTH_FAIL_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_CLIENT_DISCONNECT_RESPONSE: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_CLIENT_DISCONNECT_RESPONSE>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_CLIENT_DISCONNECT_RESPONSE failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_DISCONNECT_RESPONSE>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_APMANAGER_STEERING_CLIENT_SET_RESPONSE: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_APMANAGER_STEERING_CLIENT_SET_RESPONSE>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_CLIENT_DISCONNECT_RESPONSE failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_STEERING_CLIENT_SET_RESPONSE>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    default: {
        LOG(ERROR) << "Unknown AP_MANAGER message, action_op: "
                   << int(beerocks_header->action_op());
        return false;
    }
    }

    return true;
}

bool slave_thread::handle_cmdu_monitor_message(
    Socket *sd, std::shared_ptr<beerocks_message::cACTION_HEADER> beerocks_header,
    ieee1905_1::CmduMessageRx &cmdu_rx)
{
    if (monitor_socket == nullptr) {
        if (beerocks_header->action_op() != beerocks_message::ACTION_MONITOR_JOINED_NOTIFICATION) {
            LOG(ERROR) << "Not MONITOR_JOINED_NOTIFICATION, action_op: "
                       << int(beerocks_header->action_op());
            return true;
        }
    } else if (monitor_socket != sd) {
        LOG(WARNING) << "Unknown socket, ACTION_MONITOR action_op: "
                     << int(beerocks_header->action_op());
        return true;
    } else if (beerocks_header->action_op() ==
               beerocks_message::ACTION_MONITOR_HEARTBEAT_NOTIFICATION) {
        monitor_last_seen       = std::chrono::steady_clock::now();
        monitor_retries_counter = 0;
        return true;
    } else if (master_socket == nullptr) {
        LOG(WARNING) << "master_socket == nullptr, MONITOR action_op: "
                     << int(beerocks_header->action_op());
    }

    switch (beerocks_header->action_op()) {
    case beerocks_message::ACTION_MONITOR_JOINED_NOTIFICATION: {
        if (slave_state == STATE_WAIT_FOR_MONITOR_JOINED) {
            LOG(INFO) << "ACTION_MONITOR_JOINED_NOTIFICATION";
            monitor_socket = sd;
            LOG(INFO) << "goto STATE_BACKHAUL_ENABLE ";
            slave_state = STATE_BACKHAUL_ENABLE;
        } else {
            LOG(ERROR) << "ACTION_MONITOR_JOINED_NOTIFICATION, but slave_state != "
                          "STATE_WAIT_FOR_MONITOR_JOINED";
        }
        break;
    }
    case beerocks_message::ACTION_MONITOR_HOSTAP_AP_DISABLED_NOTIFICATION: {
        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_HOSTAP_AP_DISABLED_NOTIFICATION>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_HOSTAP_AP_DISABLED_NOTIFICATION failed";
            return false;
        }
        LOG(INFO) << "received ACTION_MONITOR_HOSTAP_AP_DISABLED_NOTIFICATION";
        if (response_in->vap_id() == beerocks::IFACE_RADIO_ID) {
            LOG(WARNING) << __FUNCTION__ << "AP_Disabled on radio, slave reset";
            if (platform_settings.passive_mode_enabled == 0) {
                stop_on_failure_attempts--;
                platform_notify_error(BPL_ERR_MONITOR_HOSTAP_DISABLED, config.hostap_iface);
            }
            slave_reset();
        }
        break;
    }
    case beerocks_message::ACTION_MONITOR_HOSTAP_STATUS_CHANGED_NOTIFICATION: {
        auto notification_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_MONITOR_HOSTAP_STATUS_CHANGED_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_APMANAGER_HOSTAP_STATUS_CHANGED_NOTIFICATION failed";
            return false;
        }
        std::stringstream print_str;
        if (notification_in->new_tx_state() != -1) {
            print_str << " new tx state: "
                      << std::string(notification_in->new_tx_state() ? "on" : "off");
        }
        if (notification_in->new_hostap_enabled_state() != -1) {
            print_str << " | new hostap_enabled state: "
                      << std::string(notification_in->new_hostap_enabled_state() ? "on" : "off");
        }
        LOG(INFO) << "ACTION_MONITOR_HOSTAP_STATUS_CHANGED_NOTIFICATION" << print_str.str();

        if (slave_state == STATE_OPERATIONAL && notification_in->new_tx_state() == 1 &&
            notification_in->new_hostap_enabled_state() == 1) {
            // post init configurations
            auto request = message_com::create_vs_message<
                beerocks_message::cACTION_PLATFORM_POST_INIT_CONFIG_REQUEST>(cmdu_tx);
            if (request == nullptr) {
                LOG(ERROR) << "Failed building cACTION_PLATFORM_POST_INIT_CONFIG_REQUEST message!";
                return false;
            }

            string_utils::copy_string(request->iface_name(message::IFACE_NAME_LENGTH),
                                      config.hostap_iface.c_str(), message::IFACE_NAME_LENGTH);
            message_com::send_cmdu(platform_manager_socket, cmdu_tx);

            // marking slave as operational if it is on operational state with tx on and hostap is enabled
            iface_status_operational_state = true;
            slave_resets_counter           = 0;
        } else {
            iface_status_operational_state = false;
        }

        if (slave_state == STATE_OPERATIONAL && notification_in->new_tx_state() == 0 &&
            notification_in->new_hostap_enabled_state() == 1) {
            if (!set_wifi_iface_state(config.hostap_iface, WIFI_IFACE_OPER_ENABLE)) {
                LOG(ERROR) << "error enabling hostap tx --> slave_reset();";
                platform_notify_error(BPL_ERR_SLAVE_IFACE_CHANGE_STATE_FAILED,
                                      config.hostap_iface.c_str());
                stop_on_failure_attempts--;
                slave_reset();
                break;
            }
        }
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE: {
        auto response_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE failed";
            break;
        }
        LOG(INFO) << "ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE mac="
                  << network_utils::mac_to_string(response_in->params().result.mac)
                  << " rx_rssi=" << int(response_in->params().rx_rssi)
                  << " id=" << int(beerocks_header->id());

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE message!";
            break;
        }

        response_out->params()            = response_in->params();
        response_out->params().src_module = beerocks::BEEROCKS_ENTITY_MONITOR;
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_START_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_START_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR)
                << "addClass ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_START_NOTIFICATION failed";
            break;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_START_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_RESPONSE message!";
            break;
        }
        notification_out->mac() = notification_in->mac();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_HOSTAP_STATS_MEASUREMENT_RESPONSE: {
        /*
             * the following code will break if the structure of
             * message::sACTION_MONITOR_HOSTAP_STATS_MEASUREMENT_RESPONSE
             * will be different from
             * message::sACTION_CONTROL_HOSTAP_STATS_MEASUREMENT_RESPONSE
             */

        // LOG(DEBUG) << "Received ACTION_MONITOR_HOSTAP_STATS_MEASUREMENT_RESPONSE"; // the print is flooding the log

        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_HOSTAP_STATS_MEASUREMENT_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_HOSTAP_STATS_MEASUREMENT_RESPONSE failed";
            return false;
        }

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_STATS_MEASUREMENT_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        response_out->ap_stats() = response_in->ap_stats();
        auto sta_stats_size      = response_in->sta_stats_size();
        if (sta_stats_size > 0) {
            if (!response_out->alloc_sta_stats(sta_stats_size)) {
                LOG(ERROR) << "Failed buffer allocation to size=" << int(sta_stats_size);
                break;
            }
            auto sta_stats_tuple_in  = response_in->sta_stats(0);
            auto sta_stats_tuple_out = response_out->sta_stats(0);
            std::copy_n(&std::get<1>(sta_stats_tuple_in), sta_stats_size,
                        &std::get<1>(sta_stats_tuple_out));
        }

        // LOG(DEBUG) << "send ACTION_CONTROL_HOSTAP_STATS_MEASUREMENT_RESPONSE"; // the print is flooding the log

        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_NO_RESPONSE_NOTIFICATION: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_CLIENT_NO_RESPONSE_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_NO_RESPONSE_NOTIFICATION failed";
            break;
        }
        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_NO_RESPONSE_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_NO_RESPONSE_NOTIFICATION message!";
            break;
        }
        notification_out->mac() = notification_in->mac();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_BEACON_11K_RESPONSE: {
        LOG(TRACE) << "ACTION_MONITOR_CLIENT_BEACON_11K_RESPONSE id=" << beerocks_header->id();
        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_CLIENT_BEACON_11K_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_BEACON_11K_RESPONSE failed";
            break;
        }
        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_BEACON_11K_RESPONSE>(cmdu_tx,
                                                                          beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_BEACON_11K_RESPONSE message!";
            break;
        }
        response_out->params() = response_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_CHANNEL_LOAD_11K_RESPONSE: {
        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_CLIENT_CHANNEL_LOAD_11K_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_CHANNEL_LOAD_11K_RESPONSE failed";
            break;
        }
        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_CHANNEL_LOAD_11K_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_CLIENT_CHANNEL_LOAD_11K_RESPONSE message!";
            break;
        }
        response_out->params() = response_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_STATISTICS_11K_RESPONSE: {
        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_CLIENT_STATISTICS_11K_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_STATISTICS_11K_RESPONSE failed";
            break;
        }
        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_STATISTICS_11K_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_STATISTICS_11K_RESPONSE message!";
            break;
        }
        response_out->params() = response_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_LINK_MEASUREMENTS_11K_RESPONSE: {
        auto response_in = cmdu_rx.addClass<
            beerocks_message::cACTION_MONITOR_CLIENT_LINK_MEASUREMENTS_11K_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_LINK_MEASUREMENTS_11K_RESPONSE failed";
            break;
        }
        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_LINK_MEASUREMENTS_11K_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR)
                << "Failed building ACTION_CONTROL_CLIENT_LINK_MEASUREMENTS_11K_RESPONSE message!";
            break;
        }
        response_out->params() = response_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE: {
        LOG(INFO) << "ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE: action_op: "
                  << int(beerocks_header->action_op());
        auto response_in = cmdu_rx.addClass<
            beerocks_message::cACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE failed";
            break;
        }
        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_CMD_RESPONSE "
                          "message!";
            break;
        }
        response_out->mac() = response_in->mac();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_NO_ACTIVITY_NOTIFICATION: {
        auto response_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_CLIENT_NO_ACTIVITY_NOTIFICATION>();
        if (response_in == nullptr) {
            LOG(ERROR) << "addClass ACTION_MONITOR_CLIENT_NO_ACTIVITY_NOTIFICATION failed";
            break;
        }
        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_NO_ACTIVITY_NOTIFICATION>(
            cmdu_tx, beerocks_header->id());
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building ACTION_CONTROL_CLIENT_NO_ACTIVITY_NOTIFICATION message!";
            break;
        }
        // Only mac id is the part of notification now, if this changes in future this message will break
        response_out->mac() = response_in->mac();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_HOSTAP_ACTIVITY_NOTIFICATION: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_HOSTAP_ACTIVITY_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_HOSTAP_ACTIVITY_NOTIFICATION failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_HOSTAP_ACTIVITY_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_ERROR_NOTIFICATION: {
        auto notification =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_ERROR_NOTIFICATION>();
        if (notification == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_ERROR_NOTIFICATION failed";
            return false;
        }
        LOG(INFO) << "ACTION_MONITOR_ERROR_NOTIFICATION, error_code="
                  << int(notification->error_code());

        if (slave_state == STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE ||
            slave_state == STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE ||
            slave_state == STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE) {
            LOG(INFO) << "WIFI_CONFIGURATION_UPDATE is in progress, ignoring";
            detach_on_conf_change = true;
            break;
        }

        monitor_thread::eThreadErrors err_code =
            monitor_thread::eThreadErrors(notification->error_code());
        if (err_code == monitor_thread::eThreadErrors::MONITOR_THREAD_ERROR_HOSTAP_DISABLED) {
            platform_notify_error(BPL_ERR_MONITOR_HOSTAP_DISABLED, "");
        } else if (err_code == monitor_thread::eThreadErrors::MONITOR_THREAD_ERROR_ATTACH_FAIL) {
            platform_notify_error(BPL_ERR_MONITOR_ATTACH_FAIL, "");
        } else if (err_code == monitor_thread::eThreadErrors::MONITOR_THREAD_ERROR_SUDDEN_DETACH) {
            platform_notify_error(BPL_ERR_MONITOR_SUDDEN_DETACH, "");
        } else if (err_code ==
                   monitor_thread::eThreadErrors::MONITOR_THREAD_ERROR_HAL_DISCONNECTED) {
            platform_notify_error(BPL_ERR_MONITOR_HAL_DISCONNECTED, "");
        } else if (err_code ==
                   monitor_thread::eThreadErrors::MONITOR_THREAD_ERROR_REPORT_PROCESS_FAIL) {
            platform_notify_error(BPL_ERR_MONITOR_REPORT_PROCESS_FAIL, "");
        }

        auto response_out = message_com::create_vs_message<
            beerocks_message::cACTION_MONITOR_ERROR_NOTIFICATION_ACK>(cmdu_tx);
        if (response_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            break;
        }

        message_com::send_cmdu(monitor_socket, cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_CLIENT_RX_RSSI_MEASUREMENT_NOTIFICATION failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_RX_RSSI_MEASUREMENT_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_STEERING_EVENT_CLIENT_ACTIVITY_NOTIFICATION: {
        auto notification_in = cmdu_rx.addClass<
            beerocks_message::cACTION_MONITOR_STEERING_EVENT_CLIENT_ACTIVITY_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR)
                << "addClass cACTION_MONITOR_STEERING_EVENT_CLIENT_ACTIVITY_NOTIFICATION failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_STEERING_EVENT_CLIENT_ACTIVITY_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building  "
                          "cACTION_CONTROL_STEERING_EVENT_CLIENT_ACTIVITY_NOTIFICATION message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_STEERING_EVENT_SNR_XING_NOTIFICATION: {
        auto notification_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_MONITOR_STEERING_EVENT_SNR_XING_NOTIFICATION>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_STEERING_EVENT_SNR_XING_NOTIFICATION failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_STEERING_EVENT_SNR_XING_NOTIFICATION>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR)
                << "Failed building cACTION_CONTROL_STEERING_EVENT_SNR_XING_NOTIFICATION message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_STEERING_CLIENT_SET_GROUP_RESPONSE: {
        auto notification_in =
            cmdu_rx
                .addClass<beerocks_message::cACTION_MONITOR_STEERING_CLIENT_SET_GROUP_RESPONSE>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_STEERING_CLIENT_SET_GROUP_RESPONSE failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_STEERING_CLIENT_SET_GROUP_RESPONSE>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR)
                << "Failed building cACTION_CONTROL_STEERING_CLIENT_SET_GROUP_RESPONSE message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    case beerocks_message::ACTION_MONITOR_STEERING_CLIENT_SET_RESPONSE: {
        auto notification_in =
            cmdu_rx.addClass<beerocks_message::cACTION_MONITOR_STEERING_CLIENT_SET_RESPONSE>();
        if (notification_in == nullptr) {
            LOG(ERROR) << "addClass cACTION_MONITOR_STEERING_CLIENT_SET_RESPONSE failed";
            return false;
        }

        auto notification_out = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_STEERING_CLIENT_SET_RESPONSE>(cmdu_tx);
        if (notification_out == nullptr) {
            LOG(ERROR) << "Failed building cACTION_CONTROL_STEERING_CLIENT_SET_RESPONSE message!";
            return false;
        }
        notification_out->params() = notification_in->params();
        send_cmdu_to_controller(cmdu_tx);
        break;
    }
    default: {
        LOG(ERROR) << "Unknown MONITOR message, action_op: " << int(beerocks_header->action_op());
        return false;
    }
    }

    return true;
}

bool slave_thread::slave_fsm(bool &call_slave_select)
{
    bool slave_ok = true;

    switch (slave_state) {
    case STATE_WAIT_BERFORE_INIT: {
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            is_backhaul_disconnected     = false;
            is_credentials_changed_on_db = false;
            LOG(TRACE) << "goto STATE_INIT";
            slave_state = STATE_INIT;
        }
        break;
    }
    case STATE_INIT: {
        LOG(INFO) << "STATE_INIT";
        slave_state = STATE_CONNECT_TO_PLATFORM_MANAGER;
        break;
    }
    case STATE_CONNECT_TO_PLATFORM_MANAGER: {
        platform_manager_socket = new SocketClient(platform_manager_uds);
        std::string err         = platform_manager_socket->getError();
        if (!err.empty()) {
            delete platform_manager_socket;
            platform_manager_socket = nullptr;

            LOG(WARNING) << "Unable to connect to Platform Manager: " << err;
            if (++connect_platform_retry_counter >= CONNECT_PLATFORM_RETRY_COUNT_MAX) {
                LOG(ERROR) << "Failed connecting to Platform Manager! Resetting...";
                platform_notify_error(BPL_ERR_SLAVE_FAILED_CONNECT_TO_PLATFORM_MANAGER, "");
                stop_on_failure_attempts--;
                slave_reset();
                connect_platform_retry_counter = 0;
            } else {
                LOG(INFO) << "Retrying in " << CONNECT_PLATFORM_RETRY_SLEEP << " milliseconds...";
                UTILS_SLEEP_MSEC(CONNECT_PLATFORM_RETRY_SLEEP);
                break;
            }

        } else {
            add_socket(platform_manager_socket);

            // CMDU Message
            auto request = message_com::create_vs_message<
                beerocks_message::cACTION_PLATFORM_SON_SLAVE_REGISTER_REQUEST>(cmdu_tx);

            if (request == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            string_utils::copy_string(request->iface_name(message::IFACE_NAME_LENGTH),
                                      config.hostap_iface.c_str(), message::IFACE_NAME_LENGTH);
            message_com::send_cmdu(platform_manager_socket, cmdu_tx);

            LOG(TRACE) << "send ACTION_PLATFORM_SON_SLAVE_REGISTER_REQUEST";
            LOG(TRACE) << "goto STATE_WAIT_FOR_PLATFORM_MANAGER_REGISTER_RESPONSE";
            slave_state_timer =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(WAIT_FOR_PLATFORM_MANAGER_REGISTER_RESPONSE_TIMEOUT_SEC);
            slave_state = STATE_WAIT_FOR_PLATFORM_MANAGER_REGISTER_RESPONSE;
        }
        break;
    }
    case STATE_WAIT_FOR_PLATFORM_MANAGER_CREDENTIALS_UPDATE_RESPONSE: {
        break;
    }
    case STATE_WAIT_FOR_PLATFORM_MANAGER_REGISTER_RESPONSE: {
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(ERROR) << "STATE_WAIT_FOR_PLATFORM_MANAGER_REGISTER_RESPONSE timeout!";
            platform_notify_error(BPL_ERR_SLAVE_PLATFORM_MANAGER_REGISTER_TIMEOUT, "");
            stop_on_failure_attempts--;
            slave_reset();
        }
        break;
    }
    case STATE_CONNECT_TO_BACKHAUL_MANAGER: {
        if (backhaul_manager_socket) {
            remove_socket(backhaul_manager_socket);
            delete backhaul_manager_socket;
            backhaul_manager_socket = nullptr;
        }
        backhaul_manager_socket = new SocketClient(backhaul_manager_uds);
        std::string err         = backhaul_manager_socket->getError();
        if (!err.empty()) {
            delete backhaul_manager_socket;
            backhaul_manager_socket = nullptr;
            LOG(ERROR) << "backhaul_manager_socket: " << err;
            platform_notify_error(BPL_ERR_SLAVE_CONNECTING_TO_BACKHAUL_MANAGER,
                                  "iface=" + config.backhaul_wireless_iface);
            stop_on_failure_attempts--;
            slave_reset();
        } else {
            add_socket(backhaul_manager_socket);

            // CMDU Message
            auto request =
                message_com::create_vs_message<beerocks_message::cACTION_BACKHAUL_REGISTER_REQUEST>(
                    cmdu_tx);

            if (request == nullptr) {
                LOG(ERROR) << "Failed building message!";
                break;
            }

            if (platform_settings.local_gw || config.backhaul_wireless_iface.empty()) {
                memset(request->sta_iface(message::IFACE_NAME_LENGTH), 0,
                       message::IFACE_NAME_LENGTH);
            } else {
                string_utils::copy_string(request->sta_iface(message::IFACE_NAME_LENGTH),
                                          config.backhaul_wireless_iface.c_str(),
                                          message::IFACE_NAME_LENGTH);
            }
            string_utils::copy_string(request->hostap_iface(message::IFACE_NAME_LENGTH),
                                      config.hostap_iface.c_str(), message::IFACE_NAME_LENGTH);

            request->local_master()         = platform_settings.local_master;
            request->local_gw()             = platform_settings.local_gw;
            request->sta_iface_filter_low() = config.backhaul_wireless_iface_filter_low;
            request->onboarding()           = platform_settings.onboarding;
            LOG(INFO) << "ACTION_BACKHAUL_REGISTER_REQUEST local_master="
                      << int(platform_settings.local_master)
                      << " local_gw=" << int(platform_settings.local_gw)
                      << " hostap_iface=" << request->hostap_iface(message::IFACE_NAME_LENGTH)
                      << " sta_iface=" << request->sta_iface(message::IFACE_NAME_LENGTH)
                      << " onboarding=" << int(request->onboarding());

            message_com::send_cmdu(backhaul_manager_socket, cmdu_tx);
            LOG(TRACE) << "send ACTION_BACKHAUL_REGISTER_REQUEST";
            LOG(TRACE) << "goto STATE_WAIT_FOR_BACKHAUL_MANAGER_REGISTER_RESPONSE";
            slave_state = STATE_WAIT_FOR_BACKHAUL_MANAGER_REGISTER_RESPONSE;
        }
        break;
    }
    case STATE_WAIT_RETRY_CONNECT_TO_BACKHAUL_MANAGER: {
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(DEBUG) << "retrying to connect connecting to backhaul manager";
            LOG(TRACE) << "goto STATE_CONNECT_TO_BACKHAUL_MANAGER";
            slave_state = STATE_CONNECT_TO_BACKHAUL_MANAGER;
        }
        break;
    }
    case STATE_WAIT_FOR_BACKHAUL_MANAGER_REGISTER_RESPONSE: {
        break;
    }
    case STATE_JOIN_INIT: {

        LOG(DEBUG) << "onboarding: " << int(platform_settings.onboarding);
        if (platform_settings.onboarding) {
            LOG(TRACE) << "goto STATE_ONBOARDING";
            slave_state = STATE_ONBOARDING;
        } else if (!wlan_settings.band_enabled) {
            LOG(DEBUG) << "wlan_settings.band_enabled=false";
            LOG(TRACE) << "goto STATE_BACKHAUL_ENABLE";
            slave_state = STATE_BACKHAUL_ENABLE;
        } else {
            if (is_slave_reset) {
                LOG(DEBUG) << "performing performing WIFI_IFACE_OPER_RESTORE, iface="
                           << config.hostap_iface;

                // restore interface to a state where is ready for interface enable
                if (!set_wifi_iface_state(config.hostap_iface, WIFI_IFACE_OPER_RESTORE)) {
                    LOG(ERROR) << "error changing iface state --> slave_reset();";
                    platform_notify_error(BPL_ERR_SLAVE_IFACE_CHANGE_STATE_FAILED,
                                          config.hostap_iface.c_str());
                    stop_on_failure_attempts--;
                    slave_reset();
                    break;
                }

                if (!config.backhaul_wireless_iface.empty() && !platform_settings.local_gw) {
                    LOG(DEBUG) << "slave reset: performing wireless backhaul "
                                  "WIFI_IFACE_OPER_RESTORE, iface="
                               << config.hostap_iface;
                    if (!set_wifi_iface_state(config.backhaul_wireless_iface,
                                              WIFI_IFACE_OPER_RESTORE)) {
                        LOG(ERROR)
                            << "error changing backhaul wireless iface state --> slave_reset();";
                        platform_notify_error(BPL_ERR_SLAVE_IFACE_CHANGE_STATE_FAILED,
                                              config.backhaul_wireless_iface.c_str());
                        stop_on_failure_attempts--;
                        slave_reset();
                        break;
                    }
                }
            }

            if (!platform_settings.local_gw) {
                is_backhaul_manager   = false;
                iface_status_bh_wired = eRadioStatus::OFF;
            }

            // mark slave as in non operational state
            iface_status_operational_state = false;

            LOG(TRACE) << "goto STATE_GET_WLAN_READY_STATUS";
            slave_state = STATE_GET_WLAN_READY_STATUS;
        }

        break;
    }
    case STATE_GET_WLAN_READY_STATUS: {

        auto request = message_com::create_vs_message<
            beerocks_message::cACTION_PLATFORM_GET_WLAN_READY_STATUS_REQUEST>(cmdu_tx);

        if (request == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        //LOG(DEBUG) << "Sending cACTION_PLATFORM_GET_WLAN_READY_STATUS_REQUEST";

        if (!message_com::send_cmdu(platform_manager_socket, cmdu_tx)) {
            LOG(ERROR) << "can't send message to platform manager!";
            return false;
        }

        slave_state_timer =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(STATE_WAIT_FOR_WLAN_READY_STATUS_RESPONSE_TIMEOUT_SEC);

        //LOG(DEBUG) << "goto STATE_WAIT_FOR_WLAN_READY_STATUS_RESPONSE";
        slave_state = STATE_WAIT_FOR_WLAN_READY_STATUS_RESPONSE;

    } break;
    case STATE_WAIT_FOR_WLAN_READY_STATUS_RESPONSE: {

        //if timeout passed perform slave reset
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(ERROR) << "STATE_WAIT_FOR_WLAN_READY_STATUS_RESPONSE timeout!";
            platform_notify_error(BPL_ERR_SLAVE_TIMEOUT_GET_WLAN_READY_STATUS_REQUEST, "");
            stop_on_failure_attempts--;
            slave_reset();
        }

    } break;
    case STATE_JOIN_INIT_BRING_UP_INTERFACES: {

        if (!set_wifi_iface_state(config.hostap_iface, WIFI_IFACE_OPER_ENABLE)) {
            LOG(ERROR) << "error changing iface state --> slave_reset();";
            platform_notify_error(BPL_ERR_SLAVE_IFACE_CHANGE_STATE_FAILED,
                                  config.hostap_iface.c_str());
            stop_on_failure_attempts--;
            slave_reset();
            break;
        }

        if (!config.backhaul_wireless_iface.empty() && !platform_settings.local_gw) {
            if (!set_wifi_iface_state(config.backhaul_wireless_iface, WIFI_IFACE_OPER_ENABLE)) {
                LOG(ERROR) << "error changing backhaul wireless iface state --> slave_reset();";
                platform_notify_error(BPL_ERR_SLAVE_IFACE_CHANGE_STATE_FAILED,
                                      config.backhaul_wireless_iface.c_str());
                stop_on_failure_attempts--;
                slave_reset();
                break;
            }
        }

        LOG(TRACE) << "goto STATE_JOIN_INIT_WAIT_FOR_IFACE_CHANGE_DONE";
        slave_state = STATE_JOIN_INIT_WAIT_FOR_IFACE_CHANGE_DONE;

        break;
    }
    case STATE_JOIN_INIT_WAIT_FOR_IFACE_CHANGE_DONE: {

        LOG(TRACE) << "goto STATE_START_AP_MANAGER";
        is_slave_reset = false;
        slave_state    = STATE_START_AP_MANAGER;
        break;
    }
    case STATE_START_AP_MANAGER: {

        LOG(INFO) << "STATE_START_AP_MANAGER";
        if (ap_manager_start()) {
            LOG(TRACE) << "goto STATE_WAIT_FOR_AP_MANAGER_INIT_DONE_NOTIFICATION";
            slave_state = STATE_WAIT_FOR_AP_MANAGER_INIT_DONE_NOTIFICATION;
        } else {
            LOG(ERROR) << "ap_manager_start() failed!";
            platform_notify_error(BPL_ERR_AP_MANAGER_START, "");
            stop_on_failure_attempts--;
            slave_reset();
        }
        break;
    }
    case STATE_WAIT_FOR_AP_MANAGER_INIT_DONE_NOTIFICATION: {
        break;
    }
    case STATE_WAIT_FOR_AP_MANAGER_JOINED: {
        break;
    }
    case STATE_AP_MANAGER_JOINED: {
        if (!is_wlan_credentials_unified && config.enable_credentials_automatic_unify) {
            LOG(TRACE) << "goto STATE_UNIFY_WIFI_CREDENTIALS";
            slave_state = STATE_UNIFY_WIFI_CREDENTIALS;
        } else {
            LOG(TRACE) << "goto STATE_START_MONITOR";
            slave_state = STATE_START_MONITOR;
        }
        break;
    }
    case STATE_UNIFY_WIFI_CREDENTIALS: {
        auto iface = (!config.backhaul_wireless_iface.empty() && !platform_settings.local_gw)
                         ? config.backhaul_wireless_iface
                         : config.hostap_iface;

        auto request = message_com::create_vs_message<
            beerocks_message::cACTION_PLATFORM_WIFI_CREDENTIALS_SET_REQUEST>(cmdu_tx);

        if (request == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        string_utils::copy_string(request->iface_name(message::IFACE_NAME_LENGTH), iface.c_str(),
                                  message::IFACE_NAME_LENGTH);
        string_utils::copy_string(request->ssid(message::WIFI_SSID_MAX_LENGTH),
                                  platform_settings.front_ssid, message::WIFI_SSID_MAX_LENGTH);
        string_utils::copy_string(request->pass(message::WIFI_PASS_MAX_LENGTH),
                                  platform_settings.front_pass, message::WIFI_PASS_MAX_LENGTH);
        string_utils::copy_string(request->security_type(message::WIFI_SECURITY_TYPE_MAX_LENGTH),
                                  platform_settings.front_security_type,
                                  message::WIFI_SECURITY_TYPE_MAX_LENGTH);

        LOG(INFO) << "unifying wlan credentials iface=" << request->iface_name()
                  << " to: ssid=" << request->ssid() << " sec=" << request->security_type()
                  << " pass=***";

        if (!message_com::send_cmdu(platform_manager_socket, cmdu_tx)) {
            LOG(ERROR) << "can't send message to platform manager!";
            return false;
        }

        slave_state_timer =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE_TIMEOUT_SEC);

        LOG(TRACE) << "goto STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE";
        slave_state = STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE;
    } break;
    case STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE: {
        //if timeout passed perform slave reset
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(ERROR) << "STATE_WAIT_FOR_UNIFY_WIFI_CREDENTIALS_RESPONSE timeout!";
            platform_notify_error(BPL_ERR_SLAVE_TIMEOUT_WIFI_CREDENTIALS_SET_REQUEST, "");
            stop_on_failure_attempts--;
            slave_reset();
        }
    } break;
    case STATE_START_MONITOR: {
        monitor_start();
        LOG(TRACE) << "goto STATE_WAIT_FOR_MONITOR_JOINED";
        slave_state = STATE_WAIT_FOR_MONITOR_JOINED;
        break;
    }
    case STATE_WAIT_FOR_MONITOR_JOINED: {
        break;
    }
    case STATE_BACKHAUL_ENABLE: {
        bool error = false;
        if (!config.backhaul_wire_iface.empty()) {
            if (config.backhaul_wire_iface_type == beerocks::IFACE_TYPE_UNSUPPORTED) {
                LOG(DEBUG) << "backhaul_wire_iface_type is UNSUPPORTED";
                platform_notify_error(BPL_ERR_CONFIG_BACKHAUL_WIRED_INTERFACE_IS_UNSUPPORTED, "");
                error = true;
            }
        }
        if (!config.backhaul_wireless_iface.empty()) {
            if (config.backhaul_wireless_iface_type == beerocks::IFACE_TYPE_UNSUPPORTED) {
                LOG(DEBUG) << "backhaul_wireless_iface is UNSUPPORTED";
                platform_notify_error(BPL_ERR_CONFIG_BACKHAUL_WIRELESS_INTERFACE_IS_UNSUPPORTED,
                                      "");
                error = true;
            }
        }
        if (config.backhaul_wire_iface.empty() && config.backhaul_wireless_iface.empty()) {
            LOG(DEBUG) << "No valid backhaul iface!";
            platform_notify_error(BPL_ERR_CONFIG_NO_VALID_BACKHAUL_INTERFACE, "");
            error = true;
        }

        if (error) {
            stop_on_failure_attempts--;
            slave_reset();
        } else {
            // backhaul manager will request for backhaul iface and tx enable after receiving ACTION_BACKHAUL_ENABLE,
            // when wireless connection is required
            LOG(TRACE) << "goto STATE_SEND_BACKHAUL_MANAGER_ENABLE";
            slave_state = STATE_SEND_BACKHAUL_MANAGER_ENABLE;
        }
        break;
    }
    case STATE_SEND_BACKHAUL_MANAGER_ENABLE: {

        // CMDU Message
        auto bh_enable =
            message_com::create_vs_message<beerocks_message::cACTION_BACKHAUL_ENABLE>(cmdu_tx);
        if (bh_enable == nullptr) {
            LOG(ERROR) << "Failed building message!";
            break;
        }

        if (!platform_settings.local_gw) {
            // Wireless config
            string_utils::copy_string(bh_enable->ssid(message::WIFI_SSID_MAX_LENGTH),
                                      platform_settings.back_ssid, message::WIFI_SSID_MAX_LENGTH);
            string_utils::copy_string(bh_enable->pass(message::WIFI_PASS_MAX_LENGTH),
                                      platform_settings.back_pass, message::WIFI_PASS_MAX_LENGTH);
            bh_enable->security_type() = static_cast<uint32_t>(
                platform_to_bwl_security(platform_settings.back_security_type));

            // Interfaces
            if (platform_settings.wired_backhaul) {
                string_utils::copy_string(bh_enable->wire_iface(message::IFACE_NAME_LENGTH),
                                          config.backhaul_wire_iface.c_str(),
                                          message::IFACE_NAME_LENGTH);
            } else {
                memset(bh_enable->wire_iface(message::WIFI_SSID_MAX_LENGTH), 0,
                       message::IFACE_NAME_LENGTH);
            }

            bh_enable->wire_iface_type()     = config.backhaul_wire_iface_type;
            bh_enable->wireless_iface_type() = config.backhaul_wireless_iface_type;
            bh_enable->wired_backhaul()      = platform_settings.wired_backhaul;
        }

        bh_enable->iface_mac()     = hostap_params.iface_mac;
        bh_enable->iface_is_5ghz() = hostap_params.iface_is_5ghz;
        bh_enable->preferred_bssid() =
            network_utils::mac_from_string(config.backhaul_preferred_bssid);

        // necessary to erase all pending enable slaves on backhaul manager
        string_utils::copy_string(bh_enable->ap_iface(message::IFACE_NAME_LENGTH),
                                  config.hostap_iface.c_str(), message::IFACE_NAME_LENGTH);
        string_utils::copy_string(bh_enable->sta_iface(message::IFACE_NAME_LENGTH),
                                  config.backhaul_wireless_iface.c_str(),
                                  message::IFACE_NAME_LENGTH);
        string_utils::copy_string(bh_enable->bridge_iface(message::IFACE_NAME_LENGTH),
                                  config.bridge_iface.c_str(), message::IFACE_NAME_LENGTH);

        // Send the message
        LOG(DEBUG) << "send ACTION_BACKHAUL_ENABLE for mac "
                   << network_utils::mac_to_string(bh_enable->iface_mac());
        if (!message_com::send_cmdu(backhaul_manager_socket, cmdu_tx)) {
            slave_reset();
        }

        // Next state
        LOG(TRACE) << "goto STATE_WAIT_FOR_BACKHAUL_MANAGER_CONNECTED_NOTIFICATION";
        slave_state = STATE_WAIT_FOR_BACKHAUL_MANAGER_CONNECTED_NOTIFICATION;
        break;
    }
    case STATE_WAIT_FOR_BACKHAUL_MANAGER_CONNECTED_NOTIFICATION: {
        break;
    }
    case STATE_WAIT_BACKHAUL_MANAGER_BUSY: {
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(TRACE) << "goto STATE_SEND_BACKHAUL_MANAGER_ENABLE";
            slave_state = STATE_SEND_BACKHAUL_MANAGER_ENABLE;
        }
        break;
    }
    case STATE_BACKHAUL_MANAGER_CONNECTED: {
        LOG(TRACE) << "MASTER_CONNECTED";

        if (!wlan_settings.band_enabled) {

            iface_status_operational_state = true;
            master_socket                  = backhaul_manager_socket;
            iface_status_ap                = eRadioStatus::OFF;
            LOG(TRACE) << "goto STATE_OPERATIONAL";
            slave_state = STATE_OPERATIONAL;
            break;
        }
        if (is_backhaul_manager) {
            if (backhaul_params.backhaul_iface == config.backhaul_wire_iface &&
                !config.backhaul_wireless_iface.empty()) {
                LOG(DEBUG) << "wire backhaul, disable iface " << config.backhaul_wireless_iface;
                if (!set_wifi_iface_state(config.backhaul_wireless_iface,
                                          WIFI_IFACE_OPER_DISABLE)) {
                    LOG(ERROR) << "error disabling backhaul wireless iface --> slave_reset()";
                    slave_reset();
                    break;
                }
            }
        } else {
            if (!config.backhaul_wireless_iface.empty()) {
                if (!set_wifi_iface_state(config.backhaul_wireless_iface,
                                          WIFI_IFACE_OPER_DISABLE)) {
                    LOG(ERROR) << "error disabling backhaul wireless iface --> slave_reset()";
                    platform_notify_error(BPL_ERR_SLAVE_IFACE_CHANGE_STATE_FAILED,
                                          config.backhaul_wireless_iface.c_str());
                    stop_on_failure_attempts--;
                    slave_reset();
                    break;
                }
            }
        }

        if (platform_settings.local_gw) {
            //TODO get bridge_iface from platform manager
            network_utils::iface_info bridge_info;
            network_utils::get_iface_info(bridge_info, config.bridge_iface);
            backhaul_params.bridge_iface = config.bridge_iface;
            //

            backhaul_params.gw_ipv4        = bridge_info.ip;
            backhaul_params.gw_bridge_mac  = bridge_info.mac;
            backhaul_params.bridge_mac     = bridge_info.mac;
            backhaul_params.bridge_ipv4    = bridge_info.ip;
            backhaul_params.backhaul_iface = backhaul_params.bridge_iface;
            backhaul_params.backhaul_mac   = bridge_info.mac;
            backhaul_params.backhaul_ipv4  = bridge_info.ip;
            backhaul_params.backhaul_bssid = network_utils::ZERO_MAC_STRING;
            // backhaul_params.backhaul_freq           = 0; // HACK temp disabled because of a bug on endian converter
            backhaul_params.backhaul_channel     = 0;
            backhaul_params.backhaul_is_wireless = 0;
            backhaul_params.backhaul_iface_type  = beerocks::IFACE_TYPE_GW_BRIDGE;
            if (is_backhaul_manager) {
                backhaul_params.backhaul_iface = config.backhaul_wire_iface;
            }
        }

        LOG(INFO) << "Backhaul Params Info:";
        LOG(INFO) << "gw_ipv4=" << backhaul_params.gw_ipv4;
        LOG(INFO) << "gw_bridge_mac=" << backhaul_params.gw_bridge_mac;
        LOG(INFO) << "controller_bridge_mac=" << backhaul_params.controller_bridge_mac;
        LOG(INFO) << "bridge_mac=" << backhaul_params.bridge_mac;
        LOG(INFO) << "bridge_ipv4=" << backhaul_params.bridge_ipv4;
        LOG(INFO) << "backhaul_iface=" << backhaul_params.backhaul_iface;
        LOG(INFO) << "backhaul_mac=" << backhaul_params.backhaul_mac;
        LOG(INFO) << "backhaul_ipv4=" << backhaul_params.backhaul_ipv4;
        LOG(INFO) << "backhaul_bssid=" << backhaul_params.backhaul_bssid;
        LOG(INFO) << "backhaul_channel=" << int(backhaul_params.backhaul_channel);
        LOG(INFO) << "backhaul_is_wireless=" << int(backhaul_params.backhaul_is_wireless);
        LOG(INFO) << "backhaul_iface_type=" << int(backhaul_params.backhaul_iface_type);
        LOG(INFO) << "is_backhaul_manager=" << int(is_backhaul_manager);

        if (is_backhaul_manager) {
            LOG(DEBUG) << "sending "
                          "ACTION_PLATFORM_SON_SLAVE_BACKHAUL_CONNECTION_COMPLETE_NOTIFICATION to "
                          "platform manager";
            auto notification = message_com::create_vs_message<
                beerocks_message::
                    cACTION_PLATFORM_SON_SLAVE_BACKHAUL_CONNECTION_COMPLETE_NOTIFICATION>(cmdu_tx);

            if (notification == nullptr) {
                LOG(ERROR) << "Failed building message!";
                return false;
            }

            notification->is_backhaul_manager() =
                is_backhaul_manager; //redundant for now but might be needed in the future
            message_com::send_cmdu(platform_manager_socket, cmdu_tx);
        }

        master_socket = backhaul_manager_socket;

        LOG(TRACE) << "goto STATE_JOIN_MASTER";
        slave_state = STATE_JOIN_MASTER;

        SLAVE_STATE_CONTINUE();
        break;
    }
    case STATE_WAIT_BEFORE_JOIN_MASTER: {

        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(TRACE) << "goto STATE_JOIN_MASTER";
            slave_state = STATE_JOIN_MASTER;
        }

        break;
    }
    case STATE_JOIN_MASTER: {

        if (master_socket == nullptr) {
            LOG(ERROR) << "master_socket == nullptr";
            platform_notify_error(BPL_ERR_SLAVE_INVALID_MASTER_SOCKET, "Invalid master socket");
            stop_on_failure_attempts--;
            slave_reset();
            break;
        }

        if (!cmdu_tx.create(0, ieee1905_1::eMessageType::AP_AUTOCONFIGURATION_WSC_MESSAGE)) {
            LOG(ERROR) << "Failed creating AP_AUTOCONFIGURATION_WSC_MESSAGE";
            return false;
        }

        auto radio_basic_caps = cmdu_tx.addClass<wfa_map::tlvApRadioBasicCapabilities>();
        if (!radio_basic_caps) {
            LOG(ERROR) << "Error creating TLV_AP_RADIO_BASIC_CAPABILITIES";
            return false;
        }
        radio_basic_caps->radio_uid() = network_utils::mac_from_string(config.radio_identifier);
        radio_basic_caps->maximum_number_of_bsss_supported() =
            4; //TODO get maximum supported VAPs from DWPAL

        // TODO: move WSC and M1 setters to separate functions
        // TODO: Currently sending dummy values, need to read them from DWPAL and use the correct WiFi
        //      Parameters based on the regulatory domain
        for (int i = 0; i < radio_basic_caps->maximum_number_of_bsss_supported(); i++) {
            auto operationClassesInfo = radio_basic_caps->create_operating_classes_info_list();
            operationClassesInfo->operating_class()            = 0; // dummy value
            operationClassesInfo->maximum_transmit_power_dbm() = 1; // dummy value

            // TODO - the number of statically non operable channels can be 0 - meaning it is
            // an optional variable length list, this is not yet supported in tlvf according to issue #8
            // for now - lets define only one.
            if (!operationClassesInfo->alloc_statically_non_operable_channels_list(1)) {
                LOG(ERROR) << "Allocation statically non operable channels list failed";
                return false;
            }
            // Set Dummy value for non operable channel list
            std::get<1>(operationClassesInfo->statically_non_operable_channels_list(0)) = 1;

            if (!radio_basic_caps->add_operating_classes_info_list(operationClassesInfo)) {
                LOG(ERROR) << "add_operating_classes_info_list failed";
                return false;
            }
        }

        // All attributes which are not explicitely set below are set to
        // default by the TLV factory, see WSC_Attributes.yml
        if (!autoconfig_wsc_add_m1()) {
            LOG(ERROR) << "Failed adding WSC M1 TLV";
            return false;
        }

        auto vs = cmdu_tx.add_vs_tlv(ieee1905_1::tlvVendorSpecific::eVendorOUI::OUI_INTEL);
        if (!vs) {
            LOG(ERROR) << "Failed adding intel vendor specific TLV";
            return false;
        }

        auto notification = message_com::add_intel_vs_data<
            beerocks_message::cACTION_CONTROL_SLAVE_JOINED_NOTIFICATION>(cmdu_tx, vs);

        if (!notification) {
            LOG(ERROR) << "Failed building cACTION_CONTROL_SLAVE_JOINED_NOTIFICATION!";
            return false;
        }

        notification->is_slave_reconf() = is_backhual_reconf;
        is_backhual_reconf              = false;

        // Version
        string_utils::copy_string(notification->slave_version(message::VERSION_LENGTH),
                                  BEEROCKS_VERSION, message::VERSION_LENGTH);

        // Platform Configuration
        notification->platform()             = config.platform;
        notification->low_pass_filter_on()   = config.backhaul_wireless_iface_filter_low;
        notification->enable_repeater_mode() = config.enable_repeater_mode;
        notification->radio_identifier() = network_utils::mac_from_string(config.radio_identifier);

        // Backhaul Params
        notification->backhaul_params().gw_ipv4 =
            network_utils::ipv4_from_string(backhaul_params.gw_ipv4);
        notification->backhaul_params().gw_bridge_mac =
            network_utils::mac_from_string(backhaul_params.gw_bridge_mac);
        notification->backhaul_params().is_backhaul_manager = is_backhaul_manager;
        notification->backhaul_params().backhaul_iface_type = backhaul_params.backhaul_iface_type;
        notification->backhaul_params().backhaul_mac =
            network_utils::mac_from_string(backhaul_params.backhaul_mac);
        notification->backhaul_params().backhaul_channel = backhaul_params.backhaul_channel;
        notification->backhaul_params().backhaul_bssid =
            network_utils::mac_from_string(backhaul_params.backhaul_bssid);
        notification->backhaul_params().backhaul_is_wireless = backhaul_params.backhaul_is_wireless;

        if (!config.bridge_iface.empty()) {
            notification->backhaul_params().bridge_mac =
                network_utils::mac_from_string(backhaul_params.bridge_mac);
            notification->backhaul_params().bridge_ipv4 =
                network_utils::ipv4_from_string(backhaul_params.bridge_ipv4);
            notification->backhaul_params().backhaul_ipv4 =
                network_utils::ipv4_from_string(backhaul_params.bridge_ipv4);
        } else {
            notification->backhaul_params().backhaul_ipv4 =
                network_utils::ipv4_from_string(backhaul_params.backhaul_ipv4);
        }

        std::copy_n(backhaul_params.backhaul_scan_measurement_list,
                    beerocks::message::BACKHAUL_SCAN_MEASUREMENT_MAX_LENGTH,
                    notification->backhaul_params().backhaul_scan_measurement_list);

        for (unsigned int i = 0; i < message::BACKHAUL_SCAN_MEASUREMENT_MAX_LENGTH; i++) {
            if (notification->backhaul_params().backhaul_scan_measurement_list[i].channel > 0) {
                LOG(DEBUG)
                    << "mac = "
                    << network_utils::mac_to_string(notification->backhaul_params()
                                                        .backhaul_scan_measurement_list[i]
                                                        .mac.oct)
                    << " channel = "
                    << int(notification->backhaul_params()
                               .backhaul_scan_measurement_list[i]
                               .channel)
                    << " rssi = "
                    << int(notification->backhaul_params().backhaul_scan_measurement_list[i].rssi);
            }
        }

        //Platform Settings
        notification->platform_settings() = platform_settings;

        //Wlan Settings
        notification->wlan_settings() = wlan_settings;
        // Hostap Params
        notification->hostap()          = hostap_params;
        notification->hostap().ant_gain = config.hostap_ant_gain;

        // Channel Selection Params
        notification->cs_params() = hostap_cs_params;

        vs->length() += notification->getLen();
        send_cmdu_to_controller(cmdu_tx);
        LOG(DEBUG) << "send SLAVE_JOINED_NOTIFICATION Size=" << int(cmdu_tx.getMessageLength());

        LOG(DEBUG) << "sending ACTION_CONTROL_SLAVE_JOINED_NOTIFICATION";
        LOG(TRACE) << "goto STATE_WAIT_FOR_JOINED_RESPONSE";
        slave_state_timer = std::chrono::steady_clock::now() +
                            std::chrono::seconds(WAIT_FOR_JOINED_RESPONSE_TIMEOUT_SEC);

        if (!wlan_settings.acs_enabled) {
            send_platform_iface_status_notif(eRadioStatus::AP_OK, true);
        }

        slave_state = STATE_WAIT_FOR_JOINED_RESPONSE;
        break;
    }
    case STATE_WAIT_FOR_JOINED_RESPONSE: {
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(INFO) << "STATE_WAIT_FOR_JOINED_RESPONSE timeout!";
            LOG(TRACE) << "goto STATE_JOIN_MASTER";
            slave_state = STATE_JOIN_MASTER;
        }
        break;
    }
    case STATE_UPDATE_MONITOR_SON_CONFIG: {
        LOG(INFO) << "sending ACTION_MONITOR_SON_CONFIG_UPDATE";

        auto update =
            message_com::create_vs_message<beerocks_message::cACTION_MONITOR_SON_CONFIG_UPDATE>(
                cmdu_tx);
        if (update == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        update->config() = son_config;
        message_com::send_cmdu(monitor_socket, cmdu_tx);
        LOG(TRACE) << "goto STATE_OPERATIONAL";
        slave_state = STATE_OPERATIONAL;
        break;
    }
    case STATE_OPERATIONAL: {
        stop_on_failure_attempts = configuration_stop_on_failure_attempts;
        process_keep_alive();
        break;
    }
    case STATE_ONBOARDING: {
        break;
    }
    case STATE_WAIT_FOR_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE: {
        if (is_credentials_changed_on_db) {
            // after slave reset credentials will update
            slave_state_timer =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(beerocks::IRE_MAX_WIRELESS_RECONNECTION_TIME_MSC);
            LOG(INFO) << "credentials changed on DB, reset the slave!";
            slave_reset();
        }
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(ERROR) << "TIMEOUT on STATE_WAIT_FOR_PLATFORM_BEEROCKS_CREDENTIALS_UPDATE_RESPONSE";
            slave_reset();
        }
        break;
    }
    case STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE: {
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            LOG(INFO) << "STATE_WAIT_FOR_WIFI_CONFIGURATION_UPDATE_COMPLETE timeout!";
            platform_notify_error(BPL_ERR_WIFI_CONFIGURATION_CHANGE_TIMEOUT,
                                  "WIFI configuration timeout!");
            slave_reset();
        }
        break;
    }
    case STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE: {
        if (std::chrono::steady_clock::now() > slave_state_timer) {
            //this is ok, not an error
            LOG(INFO) << "STATE_WAIT_FOR_ANOTHER_WIFI_CONFIGURATION_UPDATE timeout!";
            slave_reset();
        }
        break;
    }
    case STATE_VERSION_MISMATCH: {
        break;
    }
    case STATE_SSID_MISMATCH: {
        break;
    }
    case STATE_STOPPED: {
        break;
    }
    default: {
        LOG(ERROR) << "Unknown state!";
        break;
    }
    }

    return slave_ok;
}

bool slave_thread::ap_manager_start()
{
    ap_manager = new ap_manager_thread(slave_uds);

    ap_manager_thread::ap_manager_conf_t ap_manager_conf;
    ap_manager_conf.hostap_iface      = config.hostap_iface;
    ap_manager_conf.hostap_iface_type = config.hostap_iface_type;
    ap_manager_conf.acs_enabled       = wlan_settings.acs_enabled;
    ap_manager_conf.iface_filter_low  = config.backhaul_wireless_iface_filter_low;
    //ap_manager_conf.is_passive_mode     = (platform_settings.passive_mode_enabled == 1);
    ap_manager_conf.backhaul_vaps_bssid = platform_settings.backhaul_vaps_bssid;

    ap_manager->ap_manager_config(ap_manager_conf);

    if (!ap_manager->start()) {
        delete ap_manager;
        ap_manager = nullptr;
        LOG(ERROR) << "ap_manager.start()";
        return false;
    }

    return true;
}

void slave_thread::ap_manager_stop()
{
    bool did_stop = false;
    if (ap_manager_socket) {
        remove_socket(ap_manager_socket);
        delete ap_manager_socket;
        ap_manager_socket = nullptr;
        did_stop          = true;
    }
    if (ap_manager) {
        LOG(DEBUG) << "ap_manager->stop();";
        ap_manager->stop();
        delete ap_manager;
        ap_manager = nullptr;
        did_stop   = true;
    }
    if (did_stop)
        LOG(DEBUG) << "ap_manager_stop() - done";

    iface_status_ap = eRadioStatus::OFF;
}

void slave_thread::backhaul_manager_stop()
{
    if (backhaul_manager_socket) {
        remove_socket(backhaul_manager_socket);
        delete backhaul_manager_socket;
    }
    backhaul_manager_socket = nullptr;
    master_socket           = nullptr;

    iface_status_bh       = eRadioStatus::OFF;
    iface_status_bh_wired = eRadioStatus::OFF;
}

void slave_thread::platform_manager_stop()
{
    if (platform_manager_socket) {
        LOG(DEBUG) << "removing platform_manager_socket";
        remove_socket(platform_manager_socket);
        delete platform_manager_socket;
        platform_manager_socket = nullptr;
    }
}

void slave_thread::hostap_services_off() { LOG(DEBUG) << "hostap_services_off() - done"; }

bool slave_thread::hostap_services_on()
{
    bool success = true;
    LOG(DEBUG) << "hostap_services_on() - done";
    return success;
}

void slave_thread::monitor_stop()
{
    bool did_stop = false;
    if (monitor_socket) {
        remove_socket(monitor_socket);
        delete monitor_socket;
        monitor_socket = nullptr;
        did_stop       = true;
    }

    // kill monitor slave pid
    os_utils::kill_pid(config.temp_path, std::string(BEEROCKS_MONITOR) + "_" + config.hostap_iface);

    if (did_stop)
        LOG(DEBUG) << "monitor_stop() - done";
}

void slave_thread::monitor_start()
{
    monitor_stop();

    LOG(DEBUG) << "monitor_start()";

    //start new monitor process
    std::string file_name = "./" + std::string(BEEROCKS_MONITOR);
    if (access(file_name.c_str(), F_OK) == -1) //file does not exist in current location
    {
        file_name = BEEROCKS_BIN_PATH + std::string(BEEROCKS_MONITOR);
    }
    std::string cmd = file_name + " -i " + config.hostap_iface;
    SYSTEM_CALL(cmd, 2, true);
}

void slave_thread::log_son_config()
{
    LOG(DEBUG) << "SON_CONFIG_UPDATE: " << std::endl
               << "monitor_total_ch_load_notification_th_hi_percent="
               << int(son_config.monitor_total_ch_load_notification_lo_th_percent) << std::endl
               << "monitor_total_ch_load_notification_th_lo_percent="
               << int(son_config.monitor_total_ch_load_notification_hi_th_percent) << std::endl
               << "monitor_total_ch_load_notification_delta_th_percent="
               << int(son_config.monitor_total_ch_load_notification_delta_th_percent) << std::endl
               << "monitor_min_active_clients=" << int(son_config.monitor_min_active_clients)
               << std::endl
               << "monitor_active_client_th=" << int(son_config.monitor_active_client_th)
               << std::endl
               << "monitor_client_load_notification_delta_th_percent="
               << int(son_config.monitor_client_load_notification_delta_th_percent) << std::endl
               << "monitor_rx_rssi_notification_threshold_dbm="
               << int(son_config.monitor_rx_rssi_notification_threshold_dbm) << std::endl
               << "monitor_rx_rssi_notification_delta_db="
               << int(son_config.monitor_rx_rssi_notification_delta_db) << std::endl
               << "monitor_ap_idle_threshold_B=" << int(son_config.monitor_ap_idle_threshold_B)
               << std::endl
               << "monitor_ap_active_threshold_B=" << int(son_config.monitor_ap_active_threshold_B)
               << std::endl
               << "monitor_ap_idle_stable_time_sec="
               << int(son_config.monitor_ap_idle_stable_time_sec) << std::endl
               << "monitor_disable_initiative_arp="
               << int(son_config.monitor_disable_initiative_arp) << std::endl
               << "slave_keep_alive_retries=" << int(son_config.slave_keep_alive_retries);
}

/*
 * this function will add a pending action to the pending_iface_actions
 * and will prevent re-entry to the FSM until all the pending actions are complete
 */
bool slave_thread::set_wifi_iface_state(const std::string iface,
                                        beerocks::eWifiIfaceOperation iface_operation)
{
    auto operation_to_string = [&](int8_t operation) -> std::string {
        if (WIFI_IFACE_OPER_NO_CHANGE == operation) {
            return "no_change";
        } else if (WIFI_IFACE_OPER_DISABLE == operation) {
            return "disable";
        } else if (WIFI_IFACE_OPER_ENABLE == operation) {
            return "enable";
        } else if (WIFI_IFACE_OPER_RESTORE == operation) {
            return "restore";
        } else if (WIFI_IFACE_OPER_RESTART == operation) {
            return "restart";
        } else {
            return "ERROR! unknown operation!";
        }
    };

    LOG(DEBUG) << "Request iface " << iface
               << " Operation: " << operation_to_string(iface_operation);

    if (iface.empty()) {
        LOG(ERROR) << "iface is empty";
        return false;
    }

    if (pending_iface_actions.find(iface) != pending_iface_actions.end()) {
        if (pending_iface_actions[iface].operation == iface_operation) {
            LOG(ERROR) << "Same iface action is already pending for " << iface
                       << " operation: " << pending_iface_actions[iface].operation << " continue!";
            return true;
        } else {
            LOG(ERROR)
                << "!!! There is already a pending iface action for iface in the same FSM state"
                << iface << ", aborting!";
            return false;
        }
    } else {
        pending_iface_actions[iface] = {iface, iface_operation, std::chrono::steady_clock::now()};

        // CMDU Message
        auto request = message_com::create_vs_message<
            beerocks_message::cACTION_PLATFORM_WIFI_SET_IFACE_STATE_REQUEST>(cmdu_tx);

        if (request == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        string_utils::copy_string(request->iface_name(message::IFACE_NAME_LENGTH), iface.c_str(),
                                  message::IFACE_NAME_LENGTH);
        request->iface_operation() = iface_operation;

        LOG(DEBUG) << "Sending cACTION_PLATFORM_WIFI_SET_IFACE_STATE_REQUEST, iface=" << iface;

        if (!message_com::send_cmdu(platform_manager_socket, cmdu_tx)) {
            LOG(ERROR) << "can't send message to platform manager!";
            return false;
        }
    }

    return true;
}

bool slave_thread::set_radio_tx_enable(const std::string iface, bool enable)
{
    LOG(DEBUG) << "Request iface " << iface << " radio " << (enable ? "enable" : "disable");

    if (iface.empty()) {
        LOG(ERROR) << "iface is empty";
        return false;
    }

    // CMDU Message
    auto request = message_com::create_vs_message<
        beerocks_message::cACTION_PLATFORM_WIFI_SET_RADIO_TX_STATE_REQUEST>(cmdu_tx);

    if (request == nullptr) {
        LOG(ERROR) << "Failed building message!";
        return false;
    }

    string_utils::copy_string(request->iface_name(message::IFACE_NAME_LENGTH), iface.c_str(),
                              message::IFACE_NAME_LENGTH);
    request->enable() = enable;

    if (!message_com::send_cmdu(platform_manager_socket, cmdu_tx)) {
        LOG(ERROR) << "can't send message to platform manager!";
        return false;
    }

    return true;
}

void slave_thread::send_platform_iface_status_notif(eRadioStatus radio_status,
                                                    bool status_operational)
{
    // CMDU Message
    auto platform_notification = message_com::create_vs_message<
        beerocks_message::cACTION_PLATFORM_WIFI_INTERFACE_STATUS_NOTIFICATION>(cmdu_tx);

    if (platform_notification == nullptr) {
        LOG(ERROR) << "Failed building message!";
        return;
    }
    string_utils::copy_string(platform_notification->iface_name_ap(message::IFACE_NAME_LENGTH),
                              config.hostap_iface.c_str(), message::IFACE_NAME_LENGTH);
    string_utils::copy_string(platform_notification->iface_name_bh(message::IFACE_NAME_LENGTH),
                              config.backhaul_wireless_iface.c_str(), message::IFACE_NAME_LENGTH);

    platform_notification->status_ap()          = (uint8_t)radio_status;
    platform_notification->status_bh()          = (uint8_t)iface_status_bh;
    platform_notification->status_bh_wired()    = (uint8_t)iface_status_bh_wired;
    platform_notification->is_bh_manager()      = (uint8_t)is_backhaul_manager;
    platform_notification->status_operational() = (uint8_t)status_operational;

    iface_status_ap_prev                = iface_status_ap;
    iface_status_bh_prev                = iface_status_bh;
    iface_status_bh_wired_prev          = iface_status_bh_wired;
    iface_status_operational_state_prev = iface_status_operational_state;
    LOG(INFO) << "***** send_iface_status:"
              << " iface_name_ap: "
              << platform_notification->iface_name_ap(message::IFACE_NAME_LENGTH)
              << " iface_name_bh: "
              << platform_notification->iface_name_bh(message::IFACE_NAME_LENGTH)
              << " status_ap: " << (int)platform_notification->status_ap()
              << " status_bh: " << (int)platform_notification->status_bh()
              << " status_bh_wired: " << (int)platform_notification->status_bh_wired()
              << " is_bh_manager: " << (int)platform_notification->is_bh_manager()
              << " operational: " << (int)platform_notification->status_operational();

    message_com::send_cmdu(platform_manager_socket, cmdu_tx);
}

bool slave_thread::monitor_heartbeat_check()
{
    if (monitor_socket == nullptr) {
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    int time_elapsed_secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - monitor_last_seen).count();
    if (time_elapsed_secs > MONITOR_HEARTBEAT_TIMEOUT_SEC) {
        monitor_retries_counter++;
        monitor_last_seen = now;
        LOG(INFO) << "time_elapsed_secs > MONITOR_HEARTBEAT_TIMEOUT_SEC monitor_retries_counter = "
                  << int(monitor_retries_counter);
    }
    if (monitor_retries_counter >= MONITOR_HEARTBEAT_RETRIES) {
        LOG(INFO)
            << "monitor_retries_counter >= MONITOR_HEARTBEAT_RETRIES monitor_retries_counter = "
            << int(monitor_retries_counter) << " slave_reset!!";
        monitor_retries_counter = 0;
        return false;
    }
    return true;
}

bool slave_thread::ap_manager_heartbeat_check()
{
    if (ap_manager_socket == nullptr) {
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    int time_elapsed_secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - ap_manager_last_seen).count();
    if (time_elapsed_secs > AP_MANAGER_HEARTBEAT_TIMEOUT_SEC) {
        ap_manager_retries_counter++;
        ap_manager_last_seen = now;
        LOG(INFO)
            << "time_elapsed_secs > AP_MANAGER_HEARTBEAT_TIMEOUT_SEC ap_manager_retries_counter = "
            << int(ap_manager_retries_counter);
    }
    if (ap_manager_retries_counter >= AP_MANAGER_HEARTBEAT_RETRIES) {
        LOG(INFO) << "ap_manager_retries_counter >= AP_MANAGER_HEARTBEAT_RETRIES "
                     "ap_manager_retries_counter = "
                  << int(ap_manager_retries_counter) << " slave_reset!!";
        ap_manager_retries_counter = 0;
        return false;
    }
    return true;
}

bool slave_thread::send_cmdu_to_controller(ieee1905_1::CmduMessageTx &cmdu_tx)
{
    if (!master_socket) {
        LOG(ERROR) << "socket to master is nullptr";
        return false;
    }

    if (cmdu_tx.getMessageType() == ieee1905_1::eMessageType::VENDOR_SPECIFIC_MESSAGE) {
        auto beerocks_header = message_com::get_vs_class_header(cmdu_tx);
        if (!beerocks_header) {
            LOG(ERROR) << "Failed getting beerocks_header!";
            return false;
        }

        beerocks_header->radio_mac() = hostap_params.iface_mac;
        beerocks_header->direction() = beerocks::BEEROCKS_DIRECTION_CONTROLLER;
    }
    return message_com::send_cmdu(master_socket, cmdu_tx, backhaul_params.controller_bridge_mac,
                                  backhaul_params.bridge_mac);
}

bool slave_thread::handle_autoconfiguration_wsc(Socket *sd, ieee1905_1::CmduMessageRx &cmdu_rx)
{
    // Check if this is a M1 message that we sent to the controller, which was just looped back.
    // The M1 and M2 messages are both of CMDU type AP_Autoconfiguration_WSC. Thus,
    // when we send the M2 to the local agent, it will be published back on the local bus because
    // the destination is our AL-MAC, and the controller does listen to this CMDU.
    // Ideally, we should use the message type attribute from the WSC payload to distinguish.
    // Unfortunately, that is a bit complicated with the current tlv parser. However, there is another
    // way to distinguish them: the M1 message has the AP_Radio_Basic_Capabilities TLV,
    // while the M2 has the AP_Radio_Identifier TLV.
    // If this is a looped back M2 CMDU, we can treat is as handled successfully.
    if (cmdu_rx.getNextTlvType() == int(wfa_map::eTlvTypeMap::TLV_AP_RADIO_BASIC_CAPABILITIES))
        return true;

    /**
    * @brief Parse AP-Autoconfiguration WSC which should include one AP Radio Identifier
    * TLV and one or more WSC TLV containing M2
    */
    auto ruid = cmdu_rx.addClass<wfa_map::tlvApRadioIdentifier>();
    if (!ruid) {
        LOG(ERROR) << "Failed to get tlvApRadioIdentifier TLV";
        return false;
    }

    // Check if the message is for this radio agent by comparing the ruid
    if (!config.radio_identifier.compare(network_utils::mac_to_string(ruid->radio_uid())))
        return true;

    LOG(DEBUG) << "Received AP_AUTOCONFIGURATION_WSC_MESSAGE";
    // parse all M2 TLVs
    std::vector<std::shared_ptr<ieee1905_1::tlvWscM2>> m2_list;
    while (1) {
        if (cmdu_rx.getNextTlvType() != uint8_t(ieee1905_1::eTlvType::TLV_WSC))
            break;

        auto m2 = cmdu_rx.addClass<ieee1905_1::tlvWscM2>();
        if (!m2) {
            LOG(ERROR) << "Not an WSC M2 TLV!";
            return false;
        }
        m2_list.push_back(m2);
    }

    if (m2_list.empty()) {
        LOG(ERROR) << "No M2 TLVs present";
        return false;
    }

    for (auto m2 : m2_list) {
        std::string manufacturer = std::string(m2->M2Frame().manufacturer_attr.data,
                                               m2->M2Frame().manufacturer_attr.data_length);
        if (!manufacturer.compare("Intel")) {
            //TODO add support for none Intel agents
            LOG(ERROR) << "None Intel controller " << manufacturer << " , dropping message";
            return false;
        }
    }

    if (cmdu_rx.getNextTlvType() != uint8_t(ieee1905_1::eTlvType::TLV_VENDOR_SPECIFIC)) {
        LOG(ERROR) << "Not vendor specific TLV (not Intel?)";
        return false;
    }

    LOG(INFO) << "Intel controller join response";
    if (!parse_intel_join_response(sd, cmdu_rx)) {
        LOG(ERROR) << "Parse join response failed";
        return false;
    }

    return true;
}

bool slave_thread::parse_intel_join_response(Socket *sd, ieee1905_1::CmduMessageRx &cmdu_rx)
{
    LOG(DEBUG) << "ACTION_CONTROL_SLAVE_JOINED_RESPONSE sd=" << intptr_t(sd);
    if (slave_state != STATE_WAIT_FOR_JOINED_RESPONSE) {
        LOG(ERROR) << "slave_state != STATE_WAIT_FOR_JOINED_RESPONSE";
        return false;
    }

    auto beerocks_header = message_com::parse_intel_vs_message(cmdu_rx);
    if (!beerocks_header) {
        LOG(ERROR) << "Failed to parse intel vs message (not Intel?)";
        return false;
    }

    if (beerocks_header->action_op() != beerocks_message::ACTION_CONTROL_SLAVE_JOINED_RESPONSE) {
        LOG(ERROR) << "Unexpected Intel action op " << beerocks_header->action_op();
        return false;
    }

    auto joined_response =
        cmdu_rx.addClass<beerocks_message::cACTION_CONTROL_SLAVE_JOINED_RESPONSE>();
    if (joined_response == nullptr) {
        LOG(ERROR) << "addClass cACTION_CONTROL_SLAVE_JOINED_RESPONSE failed";
        return false;
    }

    // check master rejection
    if (joined_response->err_code() == beerocks::JOIN_RESP_REJECT) {
        slave_state_timer = std::chrono::steady_clock::now() +
                            std::chrono::seconds(WAIT_BEFORE_SEND_SLAVE_JOINED_NOTIFICATION_SEC);
        LOG(DEBUG) << "STATE_WAIT_FOR_JOINED_RESPONSE: join rejected!";
        LOG(DEBUG) << "goto STATE_WAIT_BEFORE_JOIN_MASTER";
        slave_state = STATE_WAIT_BEFORE_JOIN_MASTER;
        return true;
    }

    // request the current vap list from ap_manager
    auto request = message_com::create_vs_message<
        beerocks_message::cACTION_APMANAGER_HOSTAP_VAPS_LIST_UPDATE_REQUEST>(cmdu_tx);
    if (request == nullptr) {
        LOG(ERROR) << "Failed building cACTION_APMANAGER_HOSTAP_VAPS_LIST_UPDATE_REQUEST message!";
        return false;
    }
    message_com::send_cmdu(ap_manager_socket, cmdu_tx);

    // send all pending_client_association notifications
    for (auto notify : pending_client_association_cmdu) {
        auto notification = message_com::create_vs_message<
            beerocks_message::cACTION_CONTROL_CLIENT_ASSOCIATED_NOTIFICATION>(cmdu_tx);
        if (notification == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification->params() = notify.second;
        send_cmdu_to_controller(cmdu_tx);
    }
    pending_client_association_cmdu.clear();

    master_version.assign(joined_response->master_version(message::VERSION_LENGTH));

    LOG(DEBUG) << "Version (Master/Slave): " << master_version << "/" << BEEROCKS_VERSION;
    auto slave_version_s  = version::version_from_string(BEEROCKS_VERSION);
    auto master_version_s = version::version_from_string(master_version);

    // check if mismatch notification is needed
    if ((master_version_s.major > slave_version_s.major) ||
        ((master_version_s.major == slave_version_s.major) &&
         (master_version_s.minor > slave_version_s.minor)) ||
        ((master_version_s.major == slave_version_s.major) &&
         (master_version_s.minor == slave_version_s.minor) &&
         (master_version_s.build_number > slave_version_s.build_number))) {
        LOG(INFO) << "master_version > slave_version, sending "
                     "ACTION_CONTROL_VERSION_MISMATCH_NOTIFICATION";
        auto notification = message_com::create_vs_message<
            beerocks_message::cACTION_PLATFORM_VERSION_MISMATCH_NOTIFICATION>(cmdu_tx);
        if (notification == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }

        string_utils::copy_string(notification->versions().master_version, master_version.c_str(),
                                  sizeof(beerocks_message::sVersions::master_version));
        string_utils::copy_string(notification->versions().slave_version, BEEROCKS_VERSION,
                                  sizeof(beerocks_message::sVersions::slave_version));
        message_com::send_cmdu(platform_manager_socket, cmdu_tx);
    }

    // check if fatal mismatch
    if (joined_response->err_code() == beerocks::JOIN_RESP_VERSION_MISMATCH) {
        LOG(ERROR) << "Mismatch version! slave_version=" << std::string(BEEROCKS_VERSION)
                   << " master_version=" << master_version;
        LOG(DEBUG) << "goto STATE_VERSION_MISMATCH";
        slave_state = STATE_VERSION_MISMATCH;
    } else if (joined_response->err_code() == beerocks::JOIN_RESP_SSID_MISMATCH) {
        LOG(ERROR) << "Mismatch SSID!";
        LOG(DEBUG) << "goto STATE_SSID_MISMATCH";
        slave_state = STATE_SSID_MISMATCH;
    } else if (joined_response->err_code() == beerocks::JOIN_RESP_ADVERTISE_SSID_FLAG_MISMATCH) {
        LOG(INFO) << "advertise SSID flag mismatch";
        auto notification = message_com::create_vs_message<
            beerocks_message::cACTION_PLATFORM_ADVERTISE_SSID_FLAG_UPDATE_REQUEST>(cmdu_tx);
        if (notification == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        notification->flag() = (wlan_settings.advertise_ssid ? 0 : 1);
        message_com::send_cmdu(platform_manager_socket, cmdu_tx);
    } else {
        //Send master version + slave version to platform manager
        auto notification = message_com::create_vs_message<
            beerocks_message::cACTION_PLATFORM_MASTER_SLAVE_VERSIONS_NOTIFICATION>(cmdu_tx);
        if (notification == nullptr) {
            LOG(ERROR) << "Failed building message!";
            return false;
        }
        string_utils::copy_string(notification->versions().master_version, master_version.c_str(),
                                  sizeof(beerocks_message::sVersions::master_version));
        string_utils::copy_string(notification->versions().slave_version, BEEROCKS_VERSION,
                                  sizeof(beerocks_message::sVersions::slave_version));
        message_com::send_cmdu(platform_manager_socket, cmdu_tx);
        LOG(DEBUG) << "send ACTION_PLATFORM_MASTER_SLAVE_VERSIONS_NOTIFICATION";

        son_config = joined_response->config();
        log_son_config();

        slave_state = STATE_UPDATE_MONITOR_SON_CONFIG;
    }

    return true;
}

bool slave_thread::handle_channel_preference_query(Socket *sd, ieee1905_1::CmduMessageRx &cmdu_rx)
{
    LOG(DEBUG) << "Received CHANNEL_PREFERENCE_QUERY_MESSAGE";

    auto mid = cmdu_rx.getMessageId();

    // build channel preference report
    auto cmdu_tx_header =
        cmdu_tx.create(mid, ieee1905_1::eMessageType::CHANNEL_PREFERENCE_REPORT_MESSAGE);

    if (!cmdu_tx_header) {
        LOG(ERROR) << "cmdu creation of type CHANNEL_PREFERENCE_REPORT_MESSAGE, has failed";
        return false;
    }

    auto channel_preference_tlv = cmdu_tx.addClass<wfa_map::tlvChannelPreference>();
    if (!channel_preference_tlv) {
        LOG(ERROR) << "addClass ieee1905_1::tlvChannelPreference has failed";
        return false;
    }

    channel_preference_tlv->radio_uid() = network_utils::mac_from_string(config.radio_identifier);

    // Create operating class object
    auto op_class_channels = channel_preference_tlv->create_operating_classes_list();
    if (!op_class_channels) {
        LOG(ERROR) << "create_operating_classes_list() has failed!";
        return false;
    }

    // TODO: check that the data is parsed properly after fixing the following bug:
    // Since sFlags is defined after dynamic list cPreferenceOperatingClasses it cause data override
    // on the first channel on the list and sFlags itself.
    // See: https://github.com/prplfoundation/prplMesh/issues/8

    // Fill operating class object
    op_class_channels->operating_class() = 80; // random operating class for test purpose

    // Fill up channels list in operating class object, with test values
    for (uint8_t ch = 36; ch < 50; ch += 2) {
        // allocate 1 channel
        if (!op_class_channels->alloc_channel_list()) {
            LOG(ERROR) << "alloc_channel_list() has failed!";
            return false;
        }
        auto channel_idx   = op_class_channels->channel_list_length();
        auto channel_tuple = op_class_channels->channel_list(channel_idx - 1);
        if (!std::get<0>(channel_tuple)) {
            LOG(ERROR) << "getting channel entry has failed!";
            return false;
        }
        auto &channel = std::get<1>(channel_tuple);
        channel       = ch;
    }

    // Update channel list flags
    op_class_channels->flags().preference = 15; // channels preference
    op_class_channels->flags().reason_code =
        wfa_map::cPreferenceOperatingClasses::eReasonCode::UNSPECIFIED;

    // Push operating class object to the list of operating class objects
    if (!channel_preference_tlv->add_operating_classes_list(op_class_channels)) {
        LOG(ERROR) << "add_operating_classes_list() has failed!";
        return false;
    }

    return send_cmdu_to_controller(cmdu_tx);
}

bool slave_thread::autoconfig_wsc_add_m1()
{
    auto m1 = cmdu_tx.addClass<ieee1905_1::tlvWscM1>();
    if (m1 == nullptr) {
        LOG(ERROR) << "Error creating tlvWscM1";
        return false;
    }

    std::copy_n(hostap_params.iface_mac.oct, sizeof(sMacAddr), m1->M1Frame().mac_attr.data.oct);
    // TODO: read manufactured, name, model and device name from BPL
    string_utils::copy_string(m1->M1Frame().manufacturer_attr.data, "Intel",
                              m1->M1Frame().manufacturer_attr.data_length);
    string_utils::copy_string(m1->M1Frame().model_name_attr.data, "Ubuntu",
                              m1->M1Frame().model_name_attr.data_length);
    string_utils::copy_string(m1->M1Frame().model_number_attr.data, "18.04",
                              m1->M1Frame().model_number_attr.data_length);
    string_utils::copy_string(m1->M1Frame().device_name_attr.data, "prplMesh-agent",
                              m1->M1Frame().device_name_attr.data_length);
    string_utils::copy_string(m1->M1Frame().serial_number_attr.data, "prpl12345",
                              m1->M1Frame().serial_number_attr.data_length);
    std::memset(m1->M1Frame().uuid_e_attr.data, 0xff, m1->M1Frame().uuid_e_attr.data_length);
    m1->M1Frame().authentication_type_flags_attr.data = WSC::WSC_AUTH_OPEN | WSC::WSC_AUTH_WPA2;
    m1->M1Frame().encryption_type_flags_attr.data     = WSC::WSC_ENCR_NONE;
    m1->M1Frame().rf_bands_attr.data =
        hostap_params.iface_is_5ghz ? WSC::WSC_RF_BAND_5GHZ : WSC::WSC_RF_BAND_2GHZ;
    // Simulate that this radio supports both fronthaul and backhaul BSS
    WSC::set_vendor_extentions_bss_type(m1->M1Frame().vendor_extensions_attr,
                                        WSC::FRONTHAUL_BSS | WSC::BACKHAUL_BSS);
    WSC::set_primary_device_type(m1->M1Frame().primary_device_type_attr,
                                 WSC::WSC_DEV_NETWORK_INFRA_AP);
    // TODO: M1 should also have values for:
    // enrolee_nonce_attr -> to be added by encryption
    // public_key_attr -> to be added by encryption

    return true;
}
