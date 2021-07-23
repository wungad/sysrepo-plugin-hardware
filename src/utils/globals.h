// telekom / sysrepo-plugin-hardware
//
// This program is made available under the terms of the
// BSD 3-Clause license which is available at
// https://opensource.org/licenses/BSD-3-Clause
//
// SPDX-FileCopyrightText: 2021 Deutsche Telekom AG
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GLOBALS_H
#define GLOBALS_H

#include <sysrepo-cpp/Session.hpp>

#define COMPONENTS_LOCATION "/tmp/hardware_components.json"
#define DEFAULT_POLL_INTERVAL 60  // seconds

struct SensorsInitFail : public std::exception {
    const char* what() const throw() override {
        return "sensor_init() failure";
    }
};

static void logMessage(sr_log_level_t log, std::string msg) {
    msg = "IETF-Hardware: " + msg;
    switch (log) {
    case SR_LL_ERR:
        SRP_LOG_ERRMSG(msg.c_str());
        break;
    case SR_LL_WRN:
        SRP_LOG_WRNMSG(msg.c_str());
        break;
    case SR_LL_INF:
        SRP_LOG_INFMSG(msg.c_str());
        break;
    case SR_LL_DBG:
    default:
        SRP_LOG_DBGMSG(msg.c_str());
    }
}

static bool setXpath(sysrepo::S_Session& session,
                     libyang::S_Data_Node& parent,
                     std::string const& node_xpath,
                     std::string const& value) {
    try {
        libyang::S_Context ctx = session->get_context();
        if (parent) {
            parent->new_path(ctx, node_xpath.c_str(), value.c_str(), LYD_ANYDATA_CONSTSTRING, 0);
        } else {
            parent = std::make_shared<libyang::Data_Node>(ctx, node_xpath.c_str(), value.c_str(),
                                                          LYD_ANYDATA_CONSTSTRING, 0);
        }
    } catch (std::runtime_error const& e) {
        logMessage(SR_LL_WRN,
                   "At path " + node_xpath + ", value " + value + " " + ", error: " + e.what());
        return false;
    }
    return true;
}

#endif  // GLOBALS_H
