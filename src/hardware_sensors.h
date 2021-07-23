// telekom / sysrepo-plugin-hardware
//
// This program is made available under the terms of the
// BSD 3-Clause license which is available at
// https://opensource.org/licenses/BSD-3-Clause
//
// SPDX-FileCopyrightText: 2021 Deutsche Telekom AG
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef HARDWARE_SENSORS_H
#define HARDWARE_SENSORS_H

#include <sensor_data.h>
#include <utils/globals.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace hardware {

struct HardwareSensors {

    static HardwareSensors& getInstance() {
        static HardwareSensors instance;
        return instance;
    }

private:
    HardwareSensors() {
        if (sensors_init(nullptr) != 0) {
            throw SensorsInitFail();
        }
    }

    void checkAndTriggerNotification(std::string const& componentName,
                                     std::shared_ptr<SensorThreshold> sensThr,
                                     int32_t sensorValue) {
        if ((sensThr->type == SensorThreshold::ThresholdType::min &&
             sensorValue < sensThr->value) ||
            (sensThr->type == SensorThreshold::ThresholdType::max &&
             sensorValue > sensThr->value)) {
            logMessage(SR_LL_INF, "Sensor threshold triggered for: " + componentName + " value " +
                                      std::to_string(sensorValue) + ". Sending Notification...");

            std::string notifPath("/ietf-hardware:hardware/component[name='");
            notifPath += componentName + "']/sensor-notifications-augment:sensor-threshold-crossed";
            /* connect to sysrepo */
            auto conn = std::make_shared<sysrepo::Connection>();

            /* start session */
            auto sess = std::make_shared<sysrepo::Session>(conn);

            auto in_vals = std::make_shared<sysrepo::Vals>(3);
            in_vals->val(0)->set((notifPath + "/name").c_str(), sensThr->name.c_str(), SR_STRING_T);
            if (sensThr->type == SensorThreshold::ThresholdType::min) {
                in_vals->val(1)->set((notifPath + "/min").c_str(), sensThr->value);
            } else {
                in_vals->val(1)->set((notifPath + "/max").c_str(), sensThr->value);
            }
            in_vals->val(2)->set((notifPath + "/sensor-value").c_str(), sensorValue);

            sess->event_notif_send(notifPath.c_str(), in_vals);
            sensThr->triggered = true;
        }
    }

    void runFunc() {
        std::unique_lock<std::mutex> lk(mNotificationMtx);
        bool worthChecking(true);
        while (mCV.wait_for(lk, std::chrono::seconds(ComponentData::pollInterval)) ==
                   std::cv_status::timeout &&
               worthChecking) {
            worthChecking = false;
            for (auto const& configData : ComponentData::hwConfigData) {
                if (configData && !configData->sensorThresholds.empty()) {
                    std::optional<int32_t> value = getValue(configData->name);
                    if (!value) {
                        worthChecking = true;
                        continue;
                    }
                    for (auto const& sensThr : configData->sensorThresholds) {
                        if (!sensThr->triggered) {
                            worthChecking = true;
                            checkAndTriggerNotification(configData->name, sensThr, value.value());
                        }
                    }
                }
            }
        }
    }

public:
    HardwareSensors(HardwareSensors const&) = delete;
    void operator=(HardwareSensors const&) = delete;

    ~HardwareSensors() {
        notifyAndJoin();
        sensors_cleanup();
    }

    void notify() {
        mCV.notify_all();
    }

    void notifyAndJoin() {
        if (mThread.joinable()) {
            mCV.notify_all();
            mThread.join();
        }
    }

    void startThread() {
        if (mThread.joinable()) {
            mThread.join();
        }
        mThread = std::thread(&HardwareSensors::runFunc, this);
    }

    std::optional<int32_t> getValue(std::string const& sensorName) {
        sensors_chip_name const* cn = nullptr;
        int c = 0;
        std::optional<int32_t> value;
        while ((cn = sensors_get_detected_chips(0, &c))) {
            sensors_feature const* feature = nullptr;
            int f = 0;
            while ((feature = sensors_get_features(cn, &f))) {
                if (sensorName != (std::string(cn->prefix) + "/" + feature->name)) {
                    break;
                }
                switch (feature->type) {
                case SENSORS_FEATURE_IN:
                    value =
                        Sensor::getValueFromSubfeature(cn, feature, SENSORS_SUBFEATURE_IN_INPUT, 3);
                    break;
                case SENSORS_FEATURE_CURR:
                    value = Sensor::getValueFromSubfeature(cn, feature,
                                                           SENSORS_SUBFEATURE_CURR_INPUT, 3);
                    break;
                case SENSORS_FEATURE_TEMP:
                    value = Sensor::getValueFromSubfeature(cn, feature,
                                                           SENSORS_SUBFEATURE_TEMP_INPUT, 0);
                    break;
                case SENSORS_FEATURE_FAN:
                    value = Sensor::getValueFromSubfeature(cn, feature,
                                                           SENSORS_SUBFEATURE_FAN_INPUT, 0);
                    break;
                case SENSORS_FEATURE_POWER:
                    value = Sensor::getValueFromSubfeature(cn, feature,
                                                           SENSORS_SUBFEATURE_POWER_INPUT, 0);
                    break;
                case SENSORS_FEATURE_HUMIDITY:
                    value = Sensor::getValueFromSubfeature(cn, feature,
                                                           SENSORS_SUBFEATURE_HUMIDITY_INPUT, 0);
                    break;
                default:
                    break;
                }

                if (value) {
                    return value;
                }
            }
        }
        return value;
    }

    void parseSensorData(ComponentMap& hwComponents) {
        sensors_chip_name const* cn = nullptr;
        int c = 0;
        while ((cn = sensors_get_detected_chips(0, &c))) {
            sensors_feature const* feature = nullptr;
            int f = 0;
            while ((feature = sensors_get_features(cn, &f))) {
                Sensor tempSensor(std::string(cn->prefix) + "/" + feature->name);
                bool result(false);
                switch (feature->type) {
                case SENSORS_FEATURE_IN:
                    tempSensor.valueType = Sensor::ValueType::volts_dc;
                    tempSensor.valuePrecision = 3;
                    result = tempSensor.setValueFromSubfeature(
                        cn, feature, SENSORS_SUBFEATURE_IN_INPUT, tempSensor.valuePrecision);
                    break;
                case SENSORS_FEATURE_CURR:
                    tempSensor.valueType = Sensor::ValueType::amperes;
                    tempSensor.valuePrecision = 3;
                    result = tempSensor.setValueFromSubfeature(
                        cn, feature, SENSORS_SUBFEATURE_CURR_INPUT, tempSensor.valuePrecision);
                    break;
                case SENSORS_FEATURE_TEMP:
                    tempSensor.valueType = Sensor::ValueType::celsius;
                    result = tempSensor.setValueFromSubfeature(
                        cn, feature, SENSORS_SUBFEATURE_TEMP_INPUT, tempSensor.valuePrecision);
                    break;
                case SENSORS_FEATURE_FAN:
                    tempSensor.valueType = Sensor::ValueType::rpm;
                    result = tempSensor.setValueFromSubfeature(
                        cn, feature, SENSORS_SUBFEATURE_FAN_INPUT, tempSensor.valuePrecision);
                    break;
                case SENSORS_FEATURE_POWER:
                    tempSensor.valueType = Sensor::ValueType::watts;
                    result = tempSensor.setValueFromSubfeature(
                        cn, feature, SENSORS_SUBFEATURE_POWER_INPUT, tempSensor.valuePrecision);
                    break;
                case SENSORS_FEATURE_HUMIDITY:
                    tempSensor.valueType = Sensor::ValueType::percent_rh;
                    result = tempSensor.setValueFromSubfeature(
                        cn, feature, SENSORS_SUBFEATURE_HUMIDITY_INPUT, tempSensor.valuePrecision);
                    break;
                default:
                    tempSensor.valueType = Sensor::ValueType::other;
                    break;
                }
                if (result) {
                    hwComponents.emplace(std::string(tempSensor.name),
                                         std::make_shared<Sensor>(tempSensor));
                }
            }
        }
        for (auto const& configData : ComponentData::hwConfigData) {
            if (configData) {
                auto const& component = hwComponents.find(configData->name);
                component->second->sensorThresholds = configData->sensorThresholds;
            }
        }
    }

    std::mutex mNotificationMtx;
    std::condition_variable mCV;

private:
    std::thread mThread;
};

}  // namespace hardware

#endif  // HARDWARE_SENSORS_H
