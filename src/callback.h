// (C) 2020 Deutsche Telekom AG.
//
// Deutsche Telekom AG and all other contributors /
// copyright owners license this file to you under the Apache
// License, Version 2.0 (the "License"); you may not use this
// file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef CALLBACK_H
#define CALLBACK_H

#include <component_data.h>
#include <sensor_data.h>
#include <utils/rapidjson/document.h>
#include <utils/rapidjson/istreamwrapper.h>

#include <chrono>
#include <fstream>
#include <regex>

namespace hardware {

struct Callback {
    using S_Data_Node = libyang::S_Data_Node;
    using S_Context = libyang::S_Context;
    using Data_Node = libyang::Data_Node;
    using S_Session = sysrepo::S_Session;
    using Value = rapidjson::Value;
    using Document = rapidjson::Document;
    using IStreamWrapper = rapidjson::IStreamWrapper;
    using ComponentMap = std::unordered_map<std::string, ComponentData>;

    static int configurationCallback([[maybe_unused]] S_Session session,
                                     char const* module_name,
                                     [[maybe_unused]] char const* xpath,
                                     sr_event_t event,
                                     uint32_t /* request_id */) {
        printCurrentConfig(session, module_name);
        return SR_ERR_OK;
    }

    static int operationalCallback(S_Session session,
                                   const char* module_name,
                                   const char* /* path */,
                                   const char* request_xpath,
                                   uint32_t /* request_id */,
                                   S_Data_Node& parent) {
        parent = nullptr;
        std::smatch sm;
        std::string request_xpath_string(request_xpath), filteredName;
        if (std::regex_search(
                request_xpath_string, sm,
                std::regex("ietf-hardware:hardware/component\\[name='(.*?)'\\](.*)"))) {
            filteredName = sm[1];
        } else if (!std::regex_search(request_xpath_string, std::regex("ietf-hardware:(.*)"))) {
            logMessage(SR_LL_ERR, std::string("Invalid requested xpath: ") + request_xpath_string);
            return SR_ERR_CALLBACK_FAILED;
        }

        int rc = system("/usr/bin/lshw -json > " COMPONENTS_LOCATION);
        if (rc == -1) {
            logMessage(SR_LL_ERR, "lshw command failed");
            return SR_ERR_CALLBACK_FAILED;
        }
        logMessage(SR_LL_DBG, "lshw command returned:" + std::to_string(rc));
        std::string const set_xpath("/ietf-hardware:hardware");

        // +--ro last-change?   yang:date-and-time
        std::time_t lastChange(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
        char timeString[100];
        if (std::strftime(timeString, sizeof(timeString), "%FT%TZ", std::localtime(&lastChange))) {
            setXpath(session, parent, set_xpath + "/last-change", timeString);
        }

        ComponentMap hwComponents;

        std::ifstream ifs(COMPONENTS_LOCATION, std::ifstream::in);
        if (ifs.fail()) {
            logMessage(SR_LL_ERR, "Can't open: " COMPONENTS_LOCATION);
            return SR_ERR_CALLBACK_FAILED;
        }
        IStreamWrapper isw(ifs);
        Document doc;
        doc.ParseStream(isw);
        if (!doc.IsObject() && !doc.IsArray()) {
            logMessage(SR_LL_ERR, "lshw json root-node is not an object or array");
            return SR_ERR_CALLBACK_FAILED;
        }

        parseAndSetComponents(doc, hwComponents, std::string());

        if (filteredName.empty()) {
            for (auto const& c : hwComponents) {
                c.second.setXpathForAllMembers(session, parent, set_xpath);
            }
        } else if (hwComponents.find(filteredName) != hwComponents.end()) {
            hwComponents[filteredName].setXpathForAllMembers(session, parent, set_xpath);
            return SR_ERR_OK;
        }

        try {
            HardwareSensors hwsens;
            std::vector<Sensor> sensors = hwsens.parseSensorData();
            for (auto const& s : sensors) {
                if (filteredName.empty()) {
                    s.setXpathForAllMembers(session, parent, set_xpath);
                } else if (s.name == filteredName) {
                    s.setXpathForAllMembers(session, parent, set_xpath);
                    return SR_ERR_OK;
                }
            }
        } catch (std::exception const& e) {
            logMessage(SR_LL_WRN, "hardware-sensors nodes failure: " + std::string(e.what()));
        }

        if (!parent) {
            return SR_ERR_CALLBACK_FAILED;
        }
        return SR_ERR_OK;
    }

    static std::string toIANAclass(std::string const& inputClass) {
        std::string returnedClass("iana-hardware:unknown");
        static std::unordered_map<std::string, std::string> _{
            {"storage", "iana-hardware:storage-drive"},
            {"power", "iana-hardware:battery"},
            {"processor", "iana-hardware:cpu"},
            {"network", "iana-hardware:port"}};

        if (_.find(inputClass) != _.end()) {
            returnedClass = _.at(inputClass);
        }
        return returnedClass;
    }

    static std::string parseAndSetComponent(Value const& parsee,
                                            std::string const& parentName,
                                            Value::ConstMemberIterator itr,
                                            ComponentMap& hwComponents,
                                            int32_t& parent_rel_pos) {
        std::string name;
        if (itr != parsee.MemberEnd()) {
            name = itr->value.GetString();
        } else {
            // invalid entry, go to the next one
            return std::string();
        }

        // firmware node, skip this one and set the parent's firmware-rev
        // +--ro firmware-rev?     string
        if (name == "firmware" && !parentName.empty() &&
            (itr = parsee.FindMember("version")) != parsee.MemberEnd()) {
            hwComponents[parentName].firmwareRev = itr->value.GetString();
            return std::string();
        }

        // Check if a node with the current name exists, if so rename the current one
        if (hwComponents.find(name) != hwComponents.end()) {
            name = parentName + ":" + name;
        }
        hwComponents[name].name = name;

        // +--rw class             identityref
        if ((itr = parsee.FindMember("class")) != parsee.MemberEnd()) {
            hwComponents[name].classType = toIANAclass(itr->value.GetString());
        } else {
            hwComponents[name].classType = "iana-hardware:unknown";
        }

        // +--rw name              string
        // +--ro description?      string
        // +--ro hardware-rev?     string
        // +--ro serial-num?       string
        // +--ro mfg-name?         string
        // +--ro model-name?       string
        // +--rw alias?            string
        for (auto const& mapValue : getLSHWtoIETFmap()) {
            std::string const stringValue = mapValue.first;
            if ((itr = parsee.FindMember(stringValue.c_str())) != parsee.MemberEnd()) {
                hwComponents[name].setValueFromLSHWmap(stringValue, itr->value.GetString());
            }
        }

        // +--ro software-rev?     string
        // +--ro uuid?             yang:uuid
        if ((itr = parsee.FindMember("configuration")) != parsee.MemberEnd()) {
            Value::ConstMemberIterator config_elem = itr->value.FindMember("uuid");
            if (config_elem != itr->value.MemberEnd()) {
                hwComponents[name].uuid = config_elem->value.GetString();
            }
            if ((config_elem = itr->value.FindMember("driverversion")) != itr->value.MemberEnd()) {
                hwComponents[name].softwareRev = config_elem->value.GetString();
            }
            if ((config_elem = itr->value.FindMember("firmware")) != itr->value.MemberEnd()) {
                hwComponents[name].firmwareRev = config_elem->value.GetString();
            }
        }

        // +--rw parent?           -> ../../component/name
        // +--rw parent-rel-pos?   int32
        if (!parentName.empty()) {
            hwComponents[name].parentName = parentName;
            hwComponents[name].parent_rel_pos = parent_rel_pos;
            parent_rel_pos++;
        }
        if ((itr = parsee.FindMember("children")) != parsee.MemberEnd()) {
            hwComponents[name].children =
                parseAndSetComponents(itr->value.GetArray(), hwComponents, name);
        }

        return name;
    }

    static std::vector<std::string> parseAndSetComponents(Value const& parsee,
                                                          ComponentMap& hwComponents,
                                                          std::string const& parentName) {
        std::vector<std::string> siblings;
        int32_t parent_rel_pos(0);

        if (!parsee.IsArray()) {
            std::string name(parseAndSetComponent(parsee, parentName, parsee.MemberBegin(),
                                                  hwComponents, parent_rel_pos));
            if (!name.empty()) {
                siblings.emplace_back(name);
            }
            return siblings;
        }

        for (auto& m : parsee.GetArray()) {
            Value::ConstMemberIterator itr = m.FindMember("id");
            std::string name(
                parseAndSetComponent(m, parentName, itr, hwComponents, parent_rel_pos));
            if (!name.empty()) {
                siblings.emplace_back(name);
            }
        }
        return siblings;
    }

    static void printCurrentConfig(S_Session& session, char const* module_name) {
        try {
            std::string xpath(std::string("/") + module_name + std::string(":*//*"));
            auto values = session->get_items(xpath.c_str());
            if (values == nullptr)
                return;

            for (unsigned int i = 0; i < values->val_cnt(); i++)
                logMessage(SR_LL_DBG, values->val(i)->to_string());

        } catch (const std::exception& e) {
            logMessage(SR_LL_WRN, e.what());
        }
    }

    static std::unordered_map<std::string, std::string> const& getLSHWtoIETFmap() {
        static std::unordered_map<std::string, std::string> const _{
            {"description", "/description"}, {"vendor", "/mfg-name"},
            {"serial", "/serial-num"},       {"product", "/model-name"},
            {"version", "/hardware-rev"},    {"handle", "/alias"}};
        return _;
    }
};

}  // namespace hardware

#endif  // CALLBACK_H
