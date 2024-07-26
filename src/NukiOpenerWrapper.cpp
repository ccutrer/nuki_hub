#include "NukiOpenerWrapper.h"
#include <RTOS.h>
#include "PreferencesKeys.h"
#include "MqttTopics.h"
#include "Logger.h"
#include "RestartReason.h"
#include <NukiOpenerUtils.h>
#include "Config.h"

NukiOpenerWrapper* nukiOpenerInst;
Preferences* nukiOpenerPreferences = nullptr;

NukiOpenerWrapper::NukiOpenerWrapper(const std::string& deviceName, NukiDeviceId* deviceId, BleScanner::Scanner* scanner, NukiNetworkOpener* network, Gpio* gpio, Preferences* preferences)
: _deviceName(deviceName),
  _deviceId(deviceId),
  _nukiOpener(deviceName, _deviceId->get()),
  _bleScanner(scanner),
  _network(network),
  _gpio(gpio),
  _preferences(preferences)
{
    Log->print("Device id opener: ");
    Log->println(_deviceId->get());

    nukiOpenerInst = this;

    memset(&_lastKeyTurnerState, sizeof(NukiLock::KeyTurnerState), 0);
    memset(&_lastBatteryReport, sizeof(NukiLock::BatteryReport), 0);
    memset(&_batteryReport, sizeof(NukiLock::BatteryReport), 0);
    memset(&_keyTurnerState, sizeof(NukiLock::KeyTurnerState), 0);
    _keyTurnerState.lockState = NukiOpener::LockState::Undefined;

    network->setLockActionReceivedCallback(nukiOpenerInst->onLockActionReceivedCallback);
    network->setConfigUpdateReceivedCallback(nukiOpenerInst->onConfigUpdateReceivedCallback);
    if(_preferences->getBool(preference_disable_non_json, false)) network->setKeypadCommandReceivedCallback(nukiOpenerInst->onKeypadCommandReceivedCallback);
    network->setKeypadJsonCommandReceivedCallback(nukiOpenerInst->onKeypadJsonCommandReceivedCallback);

    _gpio->addCallback(NukiOpenerWrapper::gpioActionCallback);
}


NukiOpenerWrapper::~NukiOpenerWrapper()
{
    _bleScanner = nullptr;
}


void NukiOpenerWrapper::initialize()
{
    _nukiOpener.initialize();
    _nukiOpener.registerBleScanner(_bleScanner);

    _intervalLockstate = _preferences->getInt(preference_query_interval_lockstate);
    _intervalConfig = _preferences->getInt(preference_query_interval_configuration);
    _intervalBattery = _preferences->getInt(preference_query_interval_battery);
    _intervalKeypad = _preferences->getInt(preference_query_interval_keypad);
    _keypadEnabled = _preferences->getBool(preference_keypad_info_enabled);
    _publishAuthData = _preferences->getBool(preference_publish_authdata);
    _maxKeypadCodeCount = _preferences->getUInt(preference_opener_max_keypad_code_count);
    _maxTimeControlEntryCount = _preferences->getUInt(preference_opener_max_timecontrol_entry_count);
    _restartBeaconTimeout = _preferences->getInt(preference_restart_ble_beacon_lost);
    _hassEnabled = _preferences->getString(preference_mqtt_hass_discovery) != "";
    _nrOfRetries = _preferences->getInt(preference_command_nr_of_retries, 200);
    _retryDelay = _preferences->getInt(preference_command_retry_delay);
    _rssiPublishInterval = _preferences->getInt(preference_rssi_publish_interval) * 1000;

    if(_nrOfRetries < 0 || _nrOfRetries == 200)
    {
        _nrOfRetries = 3;
        _preferences->putInt(preference_command_nr_of_retries, _nrOfRetries);
    }

    if(_retryDelay <= 100)
    {
        _retryDelay = 100;
        _preferences->putInt(preference_command_retry_delay, _retryDelay);
    }

    if(_intervalLockstate == 0)
    {
        _intervalLockstate = 60 * 30;
        _preferences->putInt(preference_query_interval_lockstate, _intervalLockstate);
    }
    if(_intervalConfig == 0)
    {
        _intervalConfig = 60 * 60;
        _preferences->putInt(preference_query_interval_configuration, _intervalConfig);
    }
    if(_intervalBattery == 0)
    {
        _intervalBattery = 60 * 30;
        _preferences->putInt(preference_query_interval_battery, _intervalBattery);
    }
    if(_intervalKeypad == 0)
    {
        _intervalKeypad = 60 * 30;
        _preferences->putInt(preference_query_interval_keypad, _intervalKeypad);
    }
    if(_restartBeaconTimeout < 10)
    {
        _restartBeaconTimeout = -1;
        _preferences->putInt(preference_restart_ble_beacon_lost, _restartBeaconTimeout);
    }

    _nukiOpener.setEventHandler(this);
    _nukiOpener.setDisconnectTimeout(5000);

    Log->print(F("Lock state interval: "));
    Log->print(_intervalLockstate);
    Log->print(F(" | Battery interval: "));
    Log->print(_intervalBattery);
    Log->print(F(" | Publish auth data: "));
    Log->println(_publishAuthData ? "yes" : "no");

    if(!_publishAuthData)
    {
        _clearAuthData = true;
    }
}

void NukiOpenerWrapper::update()
{
    if(!_paired)
    {
        Log->println(F("Nuki opener start pairing"));
        _network->publishBleAddress("");

        Nuki::AuthorizationIdType idType = _preferences->getBool(preference_register_opener_as_app) ?
                                           Nuki::AuthorizationIdType::App :
                                           Nuki::AuthorizationIdType::Bridge;

        if(_nukiOpener.pairNuki(idType) == NukiOpener::PairingResult::Success)
        {
            Log->println(F("Nuki opener paired"));
            _paired = true;
            _network->publishBleAddress(_nukiOpener.getBleAddress().toString());
        }
        else
        {
            delay(200);
            return;
        }
    }

    int64_t lastReceivedBeaconTs = _nukiOpener.getLastReceivedBeaconTs();
    int64_t ts = (esp_timer_get_time() / 1000);
    uint8_t queryCommands = _network->queryCommands();

    if(_restartBeaconTimeout > 0 &&
       ts > 60000 &&
       lastReceivedBeaconTs > 0 &&
       _disableBleWatchdogTs < ts &&
       (ts - lastReceivedBeaconTs > _restartBeaconTimeout * 1000))
    {
        Log->print("No BLE beacon received from the opener for ");
        Log->print((ts - lastReceivedBeaconTs) / 1000);
        Log->println(" seconds, restarting device.");
        delay(200);
        restartEsp(RestartReason::BLEBeaconWatchdog);
    }

    _nukiOpener.updateConnectionState();

    if(_statusUpdated || _nextLockStateUpdateTs == 0 || ts >= _nextLockStateUpdateTs || (queryCommands & QUERY_COMMAND_LOCKSTATE) > 0)
    {
        _statusUpdated = false;
        _nextLockStateUpdateTs = ts + _intervalLockstate * 1000;
        updateKeyTurnerState();
        _network->publishStatusUpdated(_statusUpdated);
    }
    if(_nextBatteryReportTs == 0 || ts > _nextBatteryReportTs || (queryCommands & QUERY_COMMAND_BATTERY) > 0)
    {
        _nextBatteryReportTs = ts + _intervalBattery * 1000;
        updateBatteryState();
    }
    if(_nextConfigUpdateTs == 0 || ts > _nextConfigUpdateTs || (queryCommands & QUERY_COMMAND_CONFIG) > 0)
    {
        _nextConfigUpdateTs = ts + _intervalConfig * 1000;
        updateConfig();
        if(_hassEnabled && !_hassSetupCompleted)
        {
            setupHASS();
        }
    }
    if(_waitAuthLogUpdateTs != 0 && ts > _waitAuthLogUpdateTs)
    {
        _waitAuthLogUpdateTs = 0;
        updateAuthData(true);
    }
    if(_waitKeypadUpdateTs != 0 && ts > _waitKeypadUpdateTs)
    {
        _waitKeypadUpdateTs = 0;
        updateKeypad(true);
    }
    if(_waitTimeControlUpdateTs != 0 && ts > _waitTimeControlUpdateTs)
    {
        _waitTimeControlUpdateTs = 0;
        updateTimeControl(true);
    }
    if(_hassEnabled && _configRead && _network->reconnected())
    {
        setupHASS();
    }
    if(_rssiPublishInterval > 0 && (_nextRssiTs == 0 || ts > _nextRssiTs))
    {
        _nextRssiTs = ts + _rssiPublishInterval;

        int rssi = _nukiOpener.getRssi();
        if(rssi != _lastRssi)
        {
            _network->publishRssi(rssi);
            _lastRssi = rssi;
        }
    }

    if(_hasKeypad && _keypadEnabled && (_nextKeypadUpdateTs == 0 || ts > _nextKeypadUpdateTs || (queryCommands & QUERY_COMMAND_KEYPAD) > 0))
    {
        _nextKeypadUpdateTs = ts + _intervalKeypad * 1000;
        updateKeypad(false);
    }

    if(_nextLockAction != (NukiOpener::LockAction)0xff && ts > _nextRetryTs)
    {
        Nuki::CmdResult cmdResult = _nukiOpener.lockAction(_nextLockAction, 0, 0);
        delay(250);

        char resultStr[15] = {0};
        NukiOpener::cmdResultToString(cmdResult, resultStr);

        _network->publishCommandResult(resultStr);

        Log->print(F("Lock action result: "));
        Log->println(resultStr);

        if(cmdResult == Nuki::CmdResult::Success)
        {
            _retryCount = 0;
            _nextLockAction = (NukiOpener::LockAction) 0xff;
            _network->publishRetry("--");

            if(_intervalLockstate > 10)
            {
                _nextLockStateUpdateTs = ts + 10 * 1000;
            }
        }
        else
        {
            if(_retryCount < _nrOfRetries)
            {
                Log->print(F("Opener: Last command failed, retrying after "));
                Log->print(_retryDelay);
                Log->print(F(" milliseconds. Retry "));
                Log->print(_retryCount + 1);
                Log->print(" of ");
                Log->println(_nrOfRetries);

                _network->publishRetry(std::to_string(_retryCount + 1));

                _nextRetryTs = (esp_timer_get_time() / 1000) + _retryDelay;

                ++_retryCount;
            }
            else
            {
                Log->println(F("Opener: Maximum number of retries exceeded, aborting."));
                _network->publishRetry("failed");
                _retryCount = 0;
                _nextRetryTs = 0;
                _nextLockAction = (NukiOpener::LockAction) 0xff;
            }
        }
        postponeBleWatchdog();
    }

    if(_clearAuthData)
    {
        _network->clearAuthorizationInfo();
        _clearAuthData = false;
    }

    memcpy(&_lastKeyTurnerState, &_keyTurnerState, sizeof(NukiOpener::OpenerState));
}


void NukiOpenerWrapper::electricStrikeActuation()
{
    _nextLockAction = NukiOpener::LockAction::ElectricStrikeActuation;
}

void NukiOpenerWrapper::activateRTO()
{
    _nextLockAction = NukiOpener::LockAction::ActivateRTO;
}

void NukiOpenerWrapper::activateCM()
{
    _nextLockAction = NukiOpener::LockAction::ActivateCM;
}

void NukiOpenerWrapper::deactivateRtoCm()
{
    if(_keyTurnerState.nukiState == NukiOpener::State::ContinuousMode) _nextLockAction = NukiOpener::LockAction::DeactivateCM;
    else if(_keyTurnerState.lockState == NukiOpener::LockState::RTOactive) _nextLockAction = NukiOpener::LockAction::DeactivateRTO;
}

void NukiOpenerWrapper::deactivateRTO()
{
    _nextLockAction = NukiOpener::LockAction::DeactivateRTO;
}

void NukiOpenerWrapper::deactivateCM()
{
    _nextLockAction = NukiOpener::LockAction::DeactivateCM;
}

bool NukiOpenerWrapper::isPinSet()
{
    return _nukiOpener.getSecurityPincode() != 0;
}

bool NukiOpenerWrapper::isPinValid()
{
    return _preferences->getInt(preference_opener_pin_status, 4) == 1;
}

void NukiOpenerWrapper::setPin(const uint16_t pin)
{
    _nukiOpener.saveSecurityPincode(pin);
}

void NukiOpenerWrapper::unpair()
{
    _nukiOpener.unPairNuki();
    Preferences nukiBlePref;
    nukiBlePref.begin("NukiHubopener", false);
    nukiBlePref.clear();
    nukiBlePref.end();
    _deviceId->assignNewId();
    _preferences->remove(preference_nuki_id_opener);
    _paired = false;
}

void NukiOpenerWrapper::updateKeyTurnerState()
{
    Nuki::CmdResult result = (Nuki::CmdResult)-1;
    _retryCount = 0;

    while(_retryCount < _nrOfRetries + 1)
    {
        Log->print(F("Querying opener state: "));
        result =_nukiOpener.requestOpenerState(&_keyTurnerState);
        delay(250);
        if(result != Nuki::CmdResult::Success) {
            ++_retryCount;
        }
        else break;
    }

    char resultStr[15];
    memset(&resultStr, 0, sizeof(resultStr));
    NukiOpener::cmdResultToString(result, resultStr);
    _network->publishLockstateCommandResult(resultStr);

    if(result != Nuki::CmdResult::Success)
    {
        _retryLockstateCount++;
        postponeBleWatchdog();
        if(_retryLockstateCount < _nrOfRetries + 1)
        {
            _nextLockStateUpdateTs = (esp_timer_get_time() / 1000) + _retryDelay;
        }
        return;
    }
    _retryLockstateCount = 0;

    if(_statusUpdated &&
        _keyTurnerState.lockState == NukiOpener::LockState::Locked &&
        _lastKeyTurnerState.lockState == NukiOpener::LockState::Locked &&
        _lastKeyTurnerState.nukiState == _keyTurnerState.nukiState)
    {
        Log->println(F("Nuki opener: Ring detected (Locked)"));
        _network->publishRing(true);
    }
    else
    {
        if(_keyTurnerState.lockState != _lastKeyTurnerState.lockState &&
        _keyTurnerState.lockState == NukiOpener::LockState::Open &&
        _keyTurnerState.trigger == NukiOpener::Trigger::Manual)
        {
            Log->println(F("Nuki opener: Ring detected (Open)"));
            _network->publishRing(false);
        }

        _network->publishKeyTurnerState(_keyTurnerState, _lastKeyTurnerState);
        updateGpioOutputs();

        if(_keyTurnerState.nukiState == NukiOpener::State::ContinuousMode)
        {
            Log->println(F("Continuous Mode"));
        }

        char lockStateStr[20];
        lockstateToString(_keyTurnerState.lockState, lockStateStr);
        Log->println(lockStateStr);
    }

    if(_publishAuthData)
    {
        Log->println(F("Publishing auth data"));
        updateAuthData(false);
        Log->println(F("Done publishing auth data"));
    }

    postponeBleWatchdog();
    Log->println(F("Done querying opener state"));
}

void NukiOpenerWrapper::updateBatteryState()
{
    Nuki::CmdResult result = (Nuki::CmdResult)-1;
    _retryCount = 0;

    while(_retryCount < _nrOfRetries + 1)
    {
        Log->print(F("Querying opener battery state: "));
        result = _nukiOpener.requestBatteryReport(&_batteryReport);
        delay(250);
        if(result != Nuki::CmdResult::Success) {
            ++_retryCount;
        }
        else break;
    }

    printCommandResult(result);
    if(result == Nuki::CmdResult::Success)
    {
        _network->publishBatteryReport(_batteryReport);
    }
    postponeBleWatchdog();
    Log->println(F("Done querying opener battery state"));
}

void NukiOpenerWrapper::updateConfig()
{
    readConfig();
    readAdvancedConfig();
    _configRead = true;
    bool expectedConfig = true;

    if(_nukiConfigValid)
    {
        if(_preferences->getUInt(preference_nuki_id_opener, 0) == 0 || _retryConfigCount == 10)
        {
            _preferences->putUInt(preference_nuki_id_opener, _nukiConfig.nukiId);
        }

        if(_preferences->getUInt(preference_nuki_id_opener, 0) == _nukiConfig.nukiId)
        {
            _hasKeypad = _nukiConfig.hasKeypad > 0 || _nukiConfig.hasKeypadV2 > 0;
            _firmwareVersion = std::to_string(_nukiConfig.firmwareVersion[0]) + "." + std::to_string(_nukiConfig.firmwareVersion[1]) + "." + std::to_string(_nukiConfig.firmwareVersion[2]);
            _hardwareVersion = std::to_string(_nukiConfig.hardwareRevision[0]) + "." + std::to_string(_nukiConfig.hardwareRevision[1]);
            if(_preferences->getBool(preference_conf_info_enabled, false)) _network->publishConfig(_nukiConfig);
            _retryConfigCount = 0;

            if(_preferences->getBool(preference_timecontrol_info_enabled)) updateTimeControl(false);

            const int pinStatus = _preferences->getInt(preference_opener_pin_status, 4);

            if(isPinSet()) {
                Nuki::CmdResult result = (Nuki::CmdResult)-1;
                _retryCount = 0;

                while(_retryCount < _nrOfRetries + 1)
                {
                    result = _nukiOpener.verifySecurityPin();
                    delay(250);
                    if(result != Nuki::CmdResult::Success) {
                        ++_retryCount;
                    }
                    else break;
                }

                if(result != Nuki::CmdResult::Success)
                {
                    if(pinStatus != 2) {
                        _preferences->putInt(preference_opener_pin_status, 2);
                    }
                }
                else
                {
                    if(pinStatus != 1) {
                        _preferences->putInt(preference_opener_pin_status, 1);
                    }
                }
            }
            else
            {
                if(pinStatus != 0) {
                    _preferences->putInt(preference_opener_pin_status, 0);
                }
            }
        }
        else
        {
            expectedConfig = false;
            ++_retryConfigCount;
        }
    }
    else
    {
        expectedConfig = false;
        ++_retryConfigCount;
    }
    if(_nukiAdvancedConfigValid && _preferences->getUInt(preference_nuki_id_opener, 0) == _nukiConfig.nukiId)
    {
        if(_preferences->getBool(preference_conf_info_enabled, false)) _network->publishAdvancedConfig(_nukiAdvancedConfig);
        _retryConfigCount = 0;
    }
    else
    {
        expectedConfig = false;
        ++_retryConfigCount;
    }
    if(!expectedConfig && _retryConfigCount < 11)
    {
        int64_t ts = (esp_timer_get_time() / 1000);
        _nextConfigUpdateTs = ts + 60000;
    }
}

void NukiOpenerWrapper::updateAuthData(bool retrieved)
{
    if(!isPinValid())
    {
        Log->println(F("No valid PIN set"));
        return;
    }

    if(!retrieved)
    {
        delay(250);
        Nuki::CmdResult result = (Nuki::CmdResult)-1;
        _retryCount = 0;

        while(_retryCount < _nrOfRetries + 1)
        {
            Log->print(F("Retrieve log entries: "));
            result = _nukiOpener.retrieveLogEntries(0, _preferences->getInt(preference_authlog_max_entries, MAX_AUTHLOG), 1, false);
            delay(250);
            if(result != Nuki::CmdResult::Success) {
                ++_retryCount;
            }
            else break;
        }

        Log->println(result);
        printCommandResult(result);
        if(result == Nuki::CmdResult::Success)
        {
            _waitAuthLogUpdateTs = (esp_timer_get_time() / 1000) + 5000;
            delay(100);

            std::list<NukiOpener::LogEntry> log;
            _nukiOpener.getLogEntries(&log);

            if(log.size() > _preferences->getInt(preference_authlog_max_entries, 3))
            {
                log.resize(_preferences->getInt(preference_authlog_max_entries, 3));
            }

            log.sort([](const NukiOpener::LogEntry& a, const NukiOpener::LogEntry& b) { return a.index < b.index; });

            if(log.size() > 0)
            {
                 _network->publishAuthorizationInfo(log, true);
            }
        }
    }
    else
    {
        std::list<NukiOpener::LogEntry> log;
        _nukiOpener.getLogEntries(&log);

        if(log.size() > _preferences->getInt(preference_authlog_max_entries, MAX_AUTHLOG))
        {
            log.resize(_preferences->getInt(preference_authlog_max_entries, MAX_AUTHLOG));
        }

        log.sort([](const NukiOpener::LogEntry& a, const NukiOpener::LogEntry& b) { return a.index < b.index; });

        Log->print(F("Log size: "));
        Log->println(log.size());

        if(log.size() > 0)
        {
             _network->publishAuthorizationInfo(log, false);
        }
    }

    postponeBleWatchdog();
}

void NukiOpenerWrapper::updateKeypad(bool retrieved)
{
    if(!_preferences->getBool(preference_keypad_info_enabled)) return;

    if(!retrieved)
    {
        Nuki::CmdResult result = (Nuki::CmdResult)-1;
        _retryCount = 0;

        while(_retryCount < _nrOfRetries + 1)
        {
            Log->print(F("Querying opener keypad: "));
            result = _nukiOpener.retrieveKeypadEntries(0, _preferences->getInt(preference_keypad_max_entries, MAX_KEYPAD));
            delay(250);
            if(result != Nuki::CmdResult::Success) {
                ++_retryCount;
            }
            else break;
        }

        printCommandResult(result);
        if(result == Nuki::CmdResult::Success)
        {
            _waitKeypadUpdateTs = (esp_timer_get_time() / 1000) + 5000;
        }
    }
    else
    {
        std::list<NukiOpener::KeypadEntry> entries;
        _nukiOpener.getKeypadEntries(&entries);

        Log->print(F("Lock keypad codes: "));
        Log->println(entries.size());

        entries.sort([](const NukiOpener::KeypadEntry& a, const NukiOpener::KeypadEntry& b) { return a.codeId < b.codeId; });

        if(entries.size() > _preferences->getInt(preference_keypad_max_entries, MAX_KEYPAD))
        {
            entries.resize(_preferences->getInt(preference_keypad_max_entries, MAX_KEYPAD));
        }

        uint keypadCount = entries.size();
        if(keypadCount > _maxKeypadCodeCount)
        {
            _maxKeypadCodeCount = keypadCount;
            _preferences->putUInt(preference_opener_max_keypad_code_count, _maxKeypadCodeCount);
        }

        _network->publishKeypad(entries, _maxKeypadCodeCount);

        _keypadCodeIds.clear();
        _keypadCodeIds.reserve(entries.size());
        for(const auto& entry : entries)
        {
            _keypadCodeIds.push_back(entry.codeId);
        }
    }

    postponeBleWatchdog();
}

void NukiOpenerWrapper::updateTimeControl(bool retrieved)
{
    if(!_preferences->getBool(preference_timecontrol_info_enabled)) return;

    if(!retrieved)
    {
        Nuki::CmdResult result = (Nuki::CmdResult)-1;
        _retryCount = 0;

        while(_retryCount < _nrOfRetries + 1)
        {
            Log->print(F("Querying opener time control: "));
            result = _nukiOpener.retrieveTimeControlEntries();
            delay(250);
            if(result != Nuki::CmdResult::Success) {
                ++_retryCount;
            }
            else break;
        }

        printCommandResult(result);
        if(result == Nuki::CmdResult::Success)
        {
            _waitTimeControlUpdateTs = (esp_timer_get_time() / 1000) + 5000;
        }
    }
    else
    {
        std::list<NukiOpener::TimeControlEntry> timeControlEntries;
        _nukiOpener.getTimeControlEntries(&timeControlEntries);

        Log->print(F("Opener time control entries: "));
        Log->println(timeControlEntries.size());

        timeControlEntries.sort([](const NukiOpener::TimeControlEntry& a, const NukiOpener::TimeControlEntry& b) { return a.entryId < b.entryId; });

        if(timeControlEntries.size() > _preferences->getInt(preference_timecontrol_max_entries, MAX_TIMECONTROL))
        {
            timeControlEntries.resize(_preferences->getInt(preference_timecontrol_max_entries, MAX_TIMECONTROL));
        }

        uint timeControlCount = timeControlEntries.size();
        if(timeControlCount > _maxTimeControlEntryCount)
        {
            _maxTimeControlEntryCount = timeControlCount;
            _preferences->putUInt(preference_opener_max_timecontrol_entry_count, _maxTimeControlEntryCount);
        }

        _network->publishTimeControl(timeControlEntries, _maxTimeControlEntryCount);

        _timeControlIds.clear();
        _timeControlIds.reserve(timeControlEntries.size());
        for(const auto& entry : timeControlEntries)
        {
            _timeControlIds.push_back(entry.entryId);
        }
    }

    postponeBleWatchdog();
}

void NukiOpenerWrapper::postponeBleWatchdog()
{
    _disableBleWatchdogTs = (esp_timer_get_time() / 1000) + 15000;
}

NukiOpener::LockAction NukiOpenerWrapper::lockActionToEnum(const char *str)
{
    if(strcmp(str, "activateRTO") == 0 || strcmp(str, "ActivateRTO") == 0) return NukiOpener::LockAction::ActivateRTO;
    else if(strcmp(str, "deactivateRTO") == 0 || strcmp(str, "DeactivateRTO") == 0) return NukiOpener::LockAction::DeactivateRTO;
    else if(strcmp(str, "electricStrikeActuation") == 0 || strcmp(str, "ElectricStrikeActuation") == 0) return NukiOpener::LockAction::ElectricStrikeActuation;
    else if(strcmp(str, "activateCM") == 0 || strcmp(str, "ActivateCM") == 0) return NukiOpener::LockAction::ActivateCM;
    else if(strcmp(str, "deactivateCM") == 0 || strcmp(str, "DeactivateCM") == 0) return NukiOpener::LockAction::DeactivateCM;
    else if(strcmp(str, "fobAction2") == 0 || strcmp(str, "FobAction2") == 0) return NukiOpener::LockAction::FobAction2;
    else if(strcmp(str, "fobAction1") == 0 || strcmp(str, "FobAction1") == 0) return NukiOpener::LockAction::FobAction1;
    else if(strcmp(str, "fobAction3") == 0 || strcmp(str, "FobAction3") == 0) return NukiOpener::LockAction::FobAction3;
    return (NukiOpener::LockAction)0xff;
}

LockActionResult NukiOpenerWrapper::onLockActionReceivedCallback(const char *value)
{
    NukiOpener::LockAction action;

    if(value)
    {
        if(strlen(value) > 0)
        {
            action = nukiOpenerInst->lockActionToEnum(value);
            if((int)action == 0xff) return LockActionResult::UnknownAction;
        }
        else return LockActionResult::UnknownAction;
    }
    else return LockActionResult::UnknownAction;

    nukiOpenerPreferences = new Preferences();
    nukiOpenerPreferences->begin("nukihub", true);
    uint32_t aclPrefs[17];
    nukiOpenerPreferences->getBytes(preference_acl, &aclPrefs, sizeof(aclPrefs));

    if((action == NukiOpener::LockAction::ActivateRTO && (int)aclPrefs[9] == 1) || (action == NukiOpener::LockAction::DeactivateRTO && (int)aclPrefs[10] == 1) || (action == NukiOpener::LockAction::ElectricStrikeActuation && (int)aclPrefs[11] == 1) || (action == NukiOpener::LockAction::ActivateCM && (int)aclPrefs[12] == 1) || (action == NukiOpener::LockAction::DeactivateCM && (int)aclPrefs[13] == 1) || (action == NukiOpener::LockAction::FobAction1 && (int)aclPrefs[14] == 1) || (action == NukiOpener::LockAction::FobAction2 && (int)aclPrefs[15] == 1) || (action == NukiOpener::LockAction::FobAction3 && (int)aclPrefs[16] == 1))
    {
        nukiOpenerPreferences->end();
        nukiOpenerInst->_nextLockAction = action;
        return LockActionResult::Success;
    }

    nukiOpenerPreferences->end();
    return LockActionResult::AccessDenied;
}

void NukiOpenerWrapper::onConfigUpdateReceivedCallback(const char *value)
{
    nukiOpenerInst->onConfigUpdateReceived(value);
}

Nuki::AdvertisingMode NukiOpenerWrapper::advertisingModeToEnum(const char *str)
{
    if(strcmp(str, "Automatic") == 0) return Nuki::AdvertisingMode::Automatic;
    else if(strcmp(str, "Normal") == 0) return Nuki::AdvertisingMode::Normal;
    else if(strcmp(str, "Slow") == 0) return Nuki::AdvertisingMode::Slow;
    else if(strcmp(str, "Slowest") == 0) return Nuki::AdvertisingMode::Slowest;
    return (Nuki::AdvertisingMode)0xff;
}

Nuki::TimeZoneId NukiOpenerWrapper::timeZoneToEnum(const char *str)
{
    if(strcmp(str, "Africa/Cairo") == 0) return Nuki::TimeZoneId::Africa_Cairo;
    else if(strcmp(str, "Africa/Lagos") == 0) return Nuki::TimeZoneId::Africa_Lagos;
    else if(strcmp(str, "Africa/Maputo") == 0) return Nuki::TimeZoneId::Africa_Maputo;
    else if(strcmp(str, "Africa/Nairobi") == 0) return Nuki::TimeZoneId::Africa_Nairobi;
    else if(strcmp(str, "America/Anchorage") == 0) return Nuki::TimeZoneId::America_Anchorage;
    else if(strcmp(str, "America/Argentina/Buenos_Aires") == 0) return Nuki::TimeZoneId::America_Argentina_Buenos_Aires;
    else if(strcmp(str, "America/Chicago") == 0) return Nuki::TimeZoneId::America_Chicago;
    else if(strcmp(str, "America/Denver") == 0) return Nuki::TimeZoneId::America_Denver;
    else if(strcmp(str, "America/Halifax") == 0) return Nuki::TimeZoneId::America_Halifax;
    else if(strcmp(str, "America/Los_Angeles") == 0) return Nuki::TimeZoneId::America_Los_Angeles;
    else if(strcmp(str, "America/Manaus") == 0) return Nuki::TimeZoneId::America_Manaus;
    else if(strcmp(str, "America/Mexico_City") == 0) return Nuki::TimeZoneId::America_Mexico_City;
    else if(strcmp(str, "America/New_York") == 0) return Nuki::TimeZoneId::America_New_York;
    else if(strcmp(str, "America/Phoenix") == 0) return Nuki::TimeZoneId::America_Phoenix;
    else if(strcmp(str, "America/Regina") == 0) return Nuki::TimeZoneId::America_Regina;
    else if(strcmp(str, "America/Santiago") == 0) return Nuki::TimeZoneId::America_Santiago;
    else if(strcmp(str, "America/Sao_Paulo") == 0) return Nuki::TimeZoneId::America_Sao_Paulo;
    else if(strcmp(str, "America/St_Johns") == 0) return Nuki::TimeZoneId::America_St_Johns;
    else if(strcmp(str, "Asia/Bangkok") == 0) return Nuki::TimeZoneId::Asia_Bangkok;
    else if(strcmp(str, "Asia/Dubai") == 0) return Nuki::TimeZoneId::Asia_Dubai;
    else if(strcmp(str, "Asia/Hong_Kong") == 0) return Nuki::TimeZoneId::Asia_Hong_Kong;
    else if(strcmp(str, "Asia/Jerusalem") == 0) return Nuki::TimeZoneId::Asia_Jerusalem;
    else if(strcmp(str, "Asia/Karachi") == 0) return Nuki::TimeZoneId::Asia_Karachi;
    else if(strcmp(str, "Asia/Kathmandu") == 0) return Nuki::TimeZoneId::Asia_Kathmandu;
    else if(strcmp(str, "Asia/Kolkata") == 0) return Nuki::TimeZoneId::Asia_Kolkata;
    else if(strcmp(str, "Asia/Riyadh") == 0) return Nuki::TimeZoneId::Asia_Riyadh;
    else if(strcmp(str, "Asia/Seoul") == 0) return Nuki::TimeZoneId::Asia_Seoul;
    else if(strcmp(str, "Asia/Shanghai") == 0) return Nuki::TimeZoneId::Asia_Shanghai;
    else if(strcmp(str, "Asia/Tehran") == 0) return Nuki::TimeZoneId::Asia_Tehran;
    else if(strcmp(str, "Asia/Tokyo") == 0) return Nuki::TimeZoneId::Asia_Tokyo;
    else if(strcmp(str, "Asia/Yangon") == 0) return Nuki::TimeZoneId::Asia_Yangon;
    else if(strcmp(str, "Australia/Adelaide") == 0) return Nuki::TimeZoneId::Australia_Adelaide;
    else if(strcmp(str, "Australia/Brisbane") == 0) return Nuki::TimeZoneId::Australia_Brisbane;
    else if(strcmp(str, "Australia/Darwin") == 0) return Nuki::TimeZoneId::Australia_Darwin;
    else if(strcmp(str, "Australia/Hobart") == 0) return Nuki::TimeZoneId::Australia_Hobart;
    else if(strcmp(str, "Australia/Perth") == 0) return Nuki::TimeZoneId::Australia_Perth;
    else if(strcmp(str, "Australia/Sydney") == 0) return Nuki::TimeZoneId::Australia_Sydney;
    else if(strcmp(str, "Europe/Berlin") == 0) return Nuki::TimeZoneId::Europe_Berlin;
    else if(strcmp(str, "Europe/Helsinki") == 0) return Nuki::TimeZoneId::Europe_Helsinki;
    else if(strcmp(str, "Europe/Istanbul") == 0) return Nuki::TimeZoneId::Europe_Istanbul;
    else if(strcmp(str, "Europe/London") == 0) return Nuki::TimeZoneId::Europe_London;
    else if(strcmp(str, "Europe/Moscow") == 0) return Nuki::TimeZoneId::Europe_Moscow;
    else if(strcmp(str, "Pacific/Auckland") == 0) return Nuki::TimeZoneId::Pacific_Auckland;
    else if(strcmp(str, "Pacific/Guam") == 0) return Nuki::TimeZoneId::Pacific_Guam;
    else if(strcmp(str, "Pacific/Honolulu") == 0) return Nuki::TimeZoneId::Pacific_Honolulu;
    else if(strcmp(str, "Pacific/Pago_Pago") == 0) return Nuki::TimeZoneId::Pacific_Pago_Pago;
    else if(strcmp(str, "None") == 0) return Nuki::TimeZoneId::None;
    return (Nuki::TimeZoneId)0xff;
}

uint8_t NukiOpenerWrapper::fobActionToInt(const char *str)
{
    if(strcmp(str, "No Action") == 0) return 0;
    else if(strcmp(str, "Toggle RTO") == 0) return 1;
    else if(strcmp(str, "Activate RTO") == 0) return 2;
    else if(strcmp(str, "Deactivate RTO") == 0) return 3;
    else if(strcmp(str, "Open") == 0) return 7;
    else if(strcmp(str, "Ring") == 0) return 8;
    return 99;
}

uint8_t NukiOpenerWrapper::operatingModeToInt(const char *str)
{
    if(strcmp(str, "Generic door opener") == 0) return 0;
    else if(strcmp(str, "Analogue intercom") == 0) return 1;
    else if(strcmp(str, "Digital intercom") == 0) return 2;
    else if(strcmp(str, "Siedle") == 0) return 3;
    else if(strcmp(str, "TCS") == 0) return 4;
    else if(strcmp(str, "Bticino") == 0) return 5;
    else if(strcmp(str, "Siedle HTS") == 0) return 6;
    else if(strcmp(str, "STR") == 0) return 7;
    else if(strcmp(str, "Ritto") == 0) return 8;
    else if(strcmp(str, "Fermax") == 0) return 9;
    else if(strcmp(str, "Comelit") == 0) return 10;
    else if(strcmp(str, "Urmet BiBus") == 0) return 11;
    else if(strcmp(str, "Urmet 2Voice") == 0) return 12;
    else if(strcmp(str, "Golmar") == 0) return 13;
    else if(strcmp(str, "SKS") == 0) return 14;
    else if(strcmp(str, "Spare") == 0) return 15;
    return 99;
}

uint8_t NukiOpenerWrapper::doorbellSuppressionToInt(const char *str)
{
    if(strcmp(str, "Off") == 0) return 0;
    else if(strcmp(str, "CM") == 0) return 1;
    else if(strcmp(str, "RTO") == 0) return 2;
    else if(strcmp(str, "CM & RTO") == 0) return 3;
    else if(strcmp(str, "Ring") == 0) return 4;
    else if(strcmp(str, "CM & Ring") == 0) return 5;
    else if(strcmp(str, "RTO & Ring") == 0) return 6;
    else if(strcmp(str, "CM & RTO & Ring") == 0) return 7;
    return 99;
}

uint8_t NukiOpenerWrapper::soundToInt(const char *str)
{
    if(strcmp(str, "No Sound") == 0) return 0;
    else if(strcmp(str, "Sound 1") == 0) return 1;
    else if(strcmp(str, "Sound 2") == 0) return 2;
    else if(strcmp(str, "Sound 3") == 0) return 3;
    return 99;
}

NukiOpener::ButtonPressAction NukiOpenerWrapper::buttonPressActionToEnum(const char* str)
{
    if(strcmp(str, "No Action") == 0) return NukiOpener::ButtonPressAction::NoAction;
    else if(strcmp(str, "Toggle RTO") == 0) return NukiOpener::ButtonPressAction::ToggleRTO;
    else if(strcmp(str, "Activate RTO") == 0) return NukiOpener::ButtonPressAction::ActivateRTO;
    else if(strcmp(str, "Deactivate RTO") == 0) return NukiOpener::ButtonPressAction::DeactivateRTO;
    else if(strcmp(str, "Toggle CM") == 0) return NukiOpener::ButtonPressAction::ToggleCM;
    else if(strcmp(str, "Activate CM") == 0) return NukiOpener::ButtonPressAction::ActivateCM;
    else if(strcmp(str, "Deactivate CM") == 0) return NukiOpener::ButtonPressAction::DectivateCM;
    else if(strcmp(str, "Open") == 0) return NukiOpener::ButtonPressAction::Open;
    return (NukiOpener::ButtonPressAction)0xff;
}

Nuki::BatteryType NukiOpenerWrapper::batteryTypeToEnum(const char* str)
{
    if(strcmp(str, "Alkali") == 0) return Nuki::BatteryType::Alkali;
    else if(strcmp(str, "Accumulators") == 0) return Nuki::BatteryType::Accumulators;
    else if(strcmp(str, "Lithium") == 0) return Nuki::BatteryType::Lithium;
    return (Nuki::BatteryType)0xff;
}

void NukiOpenerWrapper::onConfigUpdateReceived(const char *value)
{

    JsonDocument jsonResult;
    char _resbuf[2048];

    if(!_configRead || !_nukiConfigValid)
    {
        jsonResult["general"] = "configNotReady";
        serializeJson(jsonResult, _resbuf, sizeof(_resbuf));
        _network->publishConfigCommandResult(_resbuf);
        return;
    }

    if(!isPinValid())
    {
        jsonResult["general"] = "noValidPinSet";
        serializeJson(jsonResult, _resbuf, sizeof(_resbuf));
        _network->publishConfigCommandResult(_resbuf);
        return;
    }

    JsonDocument json;
    DeserializationError jsonError = deserializeJson(json, value);

    if(jsonError)
    {
        jsonResult["general"] = "invalidJson";
        serializeJson(jsonResult, _resbuf, sizeof(_resbuf));
        _network->publishConfigCommandResult(_resbuf);
        return;
    }

    Nuki::CmdResult cmdResult;
    const char *basicKeys[14] = {"name", "latitude", "longitude", "pairingEnabled", "buttonEnabled", "ledFlashEnabled", "timeZoneOffset", "dstMode", "fobAction1",  "fobAction2", "fobAction3", "operatingMode", "advertisingMode", "timeZone"};
    const char *advancedKeys[20] = {"intercomID", "busModeSwitch", "shortCircuitDuration", "electricStrikeDelay", "randomElectricStrikeDelay", "electricStrikeDuration", "disableRtoAfterRing", "rtoTimeout", "doorbellSuppression", "doorbellSuppressionDuration", "soundRing", "soundOpen", "soundRto", "soundCm", "soundConfirmation", "soundLevel", "singleButtonPressAction", "doubleButtonPressAction", "batteryType", "automaticBatteryTypeDetection"};
    bool basicUpdated = false;
    bool advancedUpdated = false;
    uint32_t basicOpenerConfigAclPrefs[16];
    uint32_t advancedOpenerConfigAclPrefs[20];

    nukiOpenerPreferences = new Preferences();
    nukiOpenerPreferences->begin("nukihub", true);
    nukiOpenerPreferences->getBytes(preference_conf_opener_basic_acl, &basicOpenerConfigAclPrefs, sizeof(basicOpenerConfigAclPrefs));
    nukiOpenerPreferences->getBytes(preference_conf_opener_advanced_acl, &advancedOpenerConfigAclPrefs, sizeof(advancedOpenerConfigAclPrefs));

    for(int i=0; i < 14; i++)
    {
        if(json[basicKeys[i]])
        {
            const char *jsonchar = json[basicKeys[i]].as<const char*>();

            if(strlen(jsonchar) == 0)
            {
                jsonResult[basicKeys[i]] = "noValueSet";
                continue;
            }

            if((int)basicOpenerConfigAclPrefs[i] == 1)
            {
                cmdResult = Nuki::CmdResult::Error;
                _retryCount = 0;

                while(_retryCount < _nrOfRetries + 1)
                {
                    if(strcmp(basicKeys[i], "name") == 0)
                    {
                        if(strlen(jsonchar) <= 32)
                        {
                            if(strcmp((const char*)_nukiConfig.name, jsonchar) == 0) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setName(std::string(jsonchar));
                        }
                        else jsonResult[basicKeys[i]] = "valueTooLong";
                    }
                    else if(strcmp(basicKeys[i], "latitude") == 0)
                    {
                        const float keyvalue = atof(jsonchar);

                        if(keyvalue > 0)
                        {
                            if(_nukiConfig.latitude == keyvalue) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setLatitude(keyvalue);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "longitude") == 0)
                    {
                        const float keyvalue = atof(jsonchar);

                        if(keyvalue > 0)
                        {
                            if(_nukiConfig.longitude == keyvalue) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setLongitude(keyvalue);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "pairingEnabled") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiConfig.pairingEnabled == keyvalue) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.enablePairing((keyvalue > 0));
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "buttonEnabled") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiConfig.buttonEnabled == keyvalue) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.enableButton((keyvalue > 0));
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "ledFlashEnabled") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiConfig.ledFlashEnabled == keyvalue) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.enableLedFlash((keyvalue > 0));
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "timeZoneOffset") == 0)
                    {
                        const int16_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 0 && keyvalue <= 60)
                        {
                            if(_nukiConfig.timeZoneOffset == keyvalue) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setTimeZoneOffset(keyvalue);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "dstMode") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiConfig.dstMode == keyvalue) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.enableDst((keyvalue > 0));
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "fobAction1") == 0)
                    {
                        const uint8_t fobAct1 = nukiOpenerInst->fobActionToInt(jsonchar);

                        if(fobAct1 != 99)
                        {
                            if(_nukiConfig.fobAction1 == fobAct1) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setFobAction(1, fobAct1);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "fobAction2") == 0)
                    {
                        const uint8_t fobAct2 = nukiOpenerInst->fobActionToInt(jsonchar);

                        if(fobAct2 != 99)
                        {
                            if(_nukiConfig.fobAction2 == fobAct2) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setFobAction(2, fobAct2);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "fobAction3") == 0)
                    {
                        const uint8_t fobAct3 = nukiOpenerInst->fobActionToInt(jsonchar);

                        if(fobAct3 != 99)
                        {
                            if(_nukiConfig.fobAction3 == fobAct3) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setFobAction(3, fobAct3);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "operatingMode") == 0)
                    {
                        const uint8_t opmode = nukiOpenerInst->operatingModeToInt(jsonchar);

                        if(opmode != 99)
                        {
                            if(_nukiConfig.operatingMode == opmode) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setOperatingMode(opmode);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "advertisingMode") == 0)
                    {
                        Nuki::AdvertisingMode advmode = nukiOpenerInst->advertisingModeToEnum(jsonchar);

                        if((int)advmode != 0xff)
                        {
                            if(_nukiConfig.advertisingMode == advmode) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setAdvertisingMode(advmode);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }
                    else if(strcmp(basicKeys[i], "timeZone") == 0)
                    {
                        Nuki::TimeZoneId tzid = nukiOpenerInst->timeZoneToEnum(jsonchar);

                        if((int)tzid != 0xff)
                        {
                            if(_nukiConfig.timeZoneId == tzid) jsonResult[basicKeys[i]] = "unchanged";
                            else cmdResult = _nukiOpener.setTimeZoneId(tzid);
                        }
                        else jsonResult[basicKeys[i]] = "invalidValue";
                    }

                    if(cmdResult != Nuki::CmdResult::Success) {
                        ++_retryCount;
                    }
                    else break;
                }

                if(cmdResult == Nuki::CmdResult::Success) basicUpdated = true;

                if(!jsonResult[basicKeys[i]]) {
                    char resultStr[15] = {0};
                    NukiOpener::cmdResultToString(cmdResult, resultStr);
                    jsonResult[basicKeys[i]] = resultStr;
                }
            }
            else jsonResult[basicKeys[i]] = "accessDenied";
        }
    }

    for(int j=0; j < 20; j++)
    {
        if(json[advancedKeys[j]])
        {
            const char *jsonchar = json[advancedKeys[j]].as<const char*>();

            if(strlen(jsonchar) == 0)
            {
                jsonResult[advancedKeys[j]] = "noValueSet";
                continue;
            }

            if((int)advancedOpenerConfigAclPrefs[j] == 1)
            {
                cmdResult = Nuki::CmdResult::Error;
                _retryCount = 0;

                while(_retryCount < _nrOfRetries + 1)
                {
                    if(strcmp(advancedKeys[j], "intercomID") == 0)
                    {
                        const uint16_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 0)
                        {
                            if(_nukiAdvancedConfig.intercomID == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setIntercomID(keyvalue);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "busModeSwitch") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiAdvancedConfig.busModeSwitch == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setBusModeSwitch((keyvalue > 0));
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "shortCircuitDuration") == 0)
                    {
                        const uint16_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 0)
                        {
                            if(_nukiAdvancedConfig.shortCircuitDuration == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setShortCircuitDuration(keyvalue);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "electricStrikeDelay") == 0)
                    {
                        const uint16_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 0 && keyvalue <= 30000)
                        {
                            if(_nukiAdvancedConfig.electricStrikeDelay == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setElectricStrikeDelay(keyvalue);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "randomElectricStrikeDelay") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiAdvancedConfig.randomElectricStrikeDelay == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.enableRandomElectricStrikeDelay((keyvalue > 0));
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "electricStrikeDuration") == 0)
                    {
                        const uint16_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 1000 && keyvalue <= 30000)
                        {
                            if(_nukiAdvancedConfig.electricStrikeDuration == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setElectricStrikeDuration(keyvalue);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "disableRtoAfterRing") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiAdvancedConfig.disableRtoAfterRing == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.disableRtoAfterRing((keyvalue > 0));
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "rtoTimeout") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 5 && keyvalue <= 60)
                        {
                            if(_nukiAdvancedConfig.rtoTimeout == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setRtoTimeout(keyvalue);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "doorbellSuppression") == 0)
                    {
                        const uint8_t dbsupr = nukiOpenerInst->doorbellSuppressionToInt(jsonchar);

                        if(dbsupr != 99)
                        {
                            if(_nukiAdvancedConfig.doorbellSuppression == dbsupr) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setDoorbellSuppression(dbsupr);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "doorbellSuppressionDuration") == 0)
                    {
                        const uint16_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 500 && keyvalue <= 10000)
                        {
                            if(_nukiAdvancedConfig.doorbellSuppressionDuration == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setDoorbellSuppressionDuration(keyvalue);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "soundRing") == 0)
                    {
                        const uint8_t sound = nukiOpenerInst->soundToInt(jsonchar);

                        if(sound != 99)
                        {
                            if(_nukiAdvancedConfig.soundRing == sound) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setSoundRing(sound);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "soundOpen") == 0)
                    {
                        const uint8_t sound = nukiOpenerInst->soundToInt(jsonchar);

                        if(sound != 99)
                        {
                            if(_nukiAdvancedConfig.soundOpen == sound) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setSoundOpen(sound);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "soundRto") == 0)
                    {
                        const uint8_t sound = nukiOpenerInst->soundToInt(jsonchar);

                        if(sound != 99)
                        {
                            if(_nukiAdvancedConfig.soundRto == sound) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setSoundRto(sound);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "soundCm") == 0)
                    {
                        const uint8_t sound = nukiOpenerInst->soundToInt(jsonchar);

                        if(sound != 99)
                        {
                            if(_nukiAdvancedConfig.soundCm == sound) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setSoundCm(sound);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "soundConfirmation") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiAdvancedConfig.soundConfirmation == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.enableSoundConfirmation((keyvalue > 0));
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "soundLevel") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue >= 0 && keyvalue <= 255)
                        {
                            if(_nukiAdvancedConfig.soundLevel == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setSoundLevel(keyvalue);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "singleButtonPressAction") == 0)
                    {
                        NukiOpener::ButtonPressAction sbpa = nukiOpenerInst->buttonPressActionToEnum(jsonchar);

                        if((int)sbpa != 0xff)
                        {
                            if(_nukiAdvancedConfig.singleButtonPressAction == sbpa) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setSingleButtonPressAction(sbpa);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "doubleButtonPressAction") == 0)
                    {
                        NukiOpener::ButtonPressAction dbpa = nukiOpenerInst->buttonPressActionToEnum(jsonchar);

                        if((int)dbpa != 0xff)
                        {
                            if(_nukiAdvancedConfig.doubleButtonPressAction == dbpa) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setDoubleButtonPressAction(dbpa);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "batteryType") == 0)
                    {
                        Nuki::BatteryType battype = nukiOpenerInst->batteryTypeToEnum(jsonchar);

                        if((int)battype != 0xff)
                        {
                            if(_nukiAdvancedConfig.batteryType == battype) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.setBatteryType(battype);
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }
                    else if(strcmp(advancedKeys[j], "automaticBatteryTypeDetection") == 0)
                    {
                        const uint8_t keyvalue = atoi(jsonchar);

                        if(keyvalue == 0 || keyvalue == 1)
                        {
                            if(_nukiAdvancedConfig.automaticBatteryTypeDetection == keyvalue) jsonResult[advancedKeys[j]] = "unchanged";
                            else cmdResult = _nukiOpener.enableAutoBatteryTypeDetection((keyvalue > 0));
                        }
                        else jsonResult[advancedKeys[j]] = "invalidValue";
                    }

                    if(cmdResult != Nuki::CmdResult::Success) {
                        ++_retryCount;
                    }
                    else break;
                }

                if(cmdResult == Nuki::CmdResult::Success) advancedUpdated = true;

                if(!jsonResult[advancedKeys[j]]) {
                    char resultStr[15] = {0};
                    NukiOpener::cmdResultToString(cmdResult, resultStr);
                    jsonResult[advancedKeys[j]] = resultStr;
                }
            }
            else jsonResult[advancedKeys[j]] = "accessDenied";
        }
    }

    nukiOpenerPreferences->end();

    if(basicUpdated || advancedUpdated) jsonResult["general"] = "success";
    else jsonResult["general"] = "noChange";

    _nextConfigUpdateTs = (esp_timer_get_time() / 1000) + 300;

    serializeJson(jsonResult, _resbuf, sizeof(_resbuf));
    _network->publishConfigCommandResult(_resbuf);

    return;
}

void NukiOpenerWrapper::onKeypadCommandReceivedCallback(const char *command, const uint &id, const String &name, const String &code, const int& enabled)
{
    nukiOpenerInst->onKeypadCommandReceived(command, id, name, code, enabled);
}

void NukiOpenerWrapper::onKeypadJsonCommandReceivedCallback(const char *value)
{
    nukiOpenerInst->onKeypadJsonCommandReceived(value);
}

void NukiOpenerWrapper::gpioActionCallback(const GpioAction &action, const int& pin)
{
    switch(action)
    {
        case GpioAction::ElectricStrikeActuation:
            nukiOpenerInst->electricStrikeActuation();
            break;
        case GpioAction::ActivateRTO:
            nukiOpenerInst->activateRTO();
            break;
        case GpioAction::ActivateCM:
            nukiOpenerInst->activateCM();
            break;
        case GpioAction::DeactivateRtoCm:
            nukiOpenerInst->deactivateRtoCm();
            break;
        case GpioAction::DeactivateRTO:
            nukiOpenerInst->deactivateRTO();
            break;
        case GpioAction::DeactivateCM:
            nukiOpenerInst->deactivateCM();
            break;
    }
}

void NukiOpenerWrapper::onKeypadCommandReceived(const char *command, const uint &id, const String &name, const String &code, const int& enabled)
{
    if(_preferences->getBool(preference_disable_non_json, false)) return;

    if(!_preferences->getBool(preference_keypad_control_enabled))
    {
        _network->publishKeypadCommandResult("KeypadControlDisabled");
        return;
    }

    if(!_hasKeypad)
    {
        if(_configRead)
        {
            _network->publishKeypadCommandResult("KeypadNotAvailable");
        }
        return;
    }
    if(!_keypadEnabled)
    {
        return;
    }

    bool idExists = std::find(_keypadCodeIds.begin(), _keypadCodeIds.end(), id) != _keypadCodeIds.end();
    int codeInt = code.toInt();
    bool codeValid = codeInt > 100000 && codeInt < 1000000 && (code.indexOf('0') == -1);
    Nuki::CmdResult result = (Nuki::CmdResult)-1;
    _retryCount = 0;

    while(_retryCount < _nrOfRetries + 1)
    {
        if(strcmp(command, "add") == 0)
        {
            if(name == "" || name == "--")
            {
                _network->publishKeypadCommandResult("MissingParameterName");
                return;
            }
            if(codeInt == 0)
            {
                _network->publishKeypadCommandResult("MissingParameterCode");
                return;
            }
            if(!codeValid)
            {
                _network->publishKeypadCommandResult("CodeInvalid");
                return;
            }

            NukiOpener::NewKeypadEntry entry;
            memset(&entry, 0, sizeof(entry));
            size_t nameLen = name.length();
            memcpy(&entry.name, name.c_str(), nameLen > 20 ? 20 : nameLen);
            entry.code = codeInt;
            result = _nukiOpener.addKeypadEntry(entry);
            delay(250);
            Log->print("Add keypad code: ");
            Log->println((int)result);
            updateKeypad(false);
        }
        else if(strcmp(command, "delete") == 0)
        {
            if(!idExists)
            {
                _network->publishKeypadCommandResult("UnknownId");
                return;
            }

            result = _nukiOpener.deleteKeypadEntry(id);
            delay(250);
            Log->print("Delete keypad code: ");
            Log->println((int)result);
            updateKeypad(false);
        }
        else if(strcmp(command, "update") == 0)
        {
            if(name == "" || name == "--")
            {
                _network->publishKeypadCommandResult("MissingParameterName");
                return;
            }
            if(codeInt == 0)
            {
                _network->publishKeypadCommandResult("MissingParameterCode");
                return;
            }
            if(!codeValid)
            {
                _network->publishKeypadCommandResult("CodeInvalid");
                return;
            }
            if(!idExists)
            {
                _network->publishKeypadCommandResult("UnknownId");
                return;
            }

            NukiOpener::UpdatedKeypadEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.codeId = id;
            size_t nameLen = name.length();
            memcpy(&entry.name, name.c_str(), nameLen > 20 ? 20 : nameLen);
            entry.code = codeInt;
            entry.enabled = enabled == 0 ? 0 : 1;
            result = _nukiOpener.updateKeypadEntry(entry);
            delay(250);
            Log->print("Update keypad code: "); 
            Log->println((int)result);
            updateKeypad(false);
        }
        else if(strcmp(command, "--") == 0)
        {
            return;
        }
        else
        {
            _network->publishKeypadCommandResult("UnknownCommand");
            return;
        }

        if(result != Nuki::CmdResult::Success) {
            ++_retryCount;
        }
        else break;
    }

    if((int)result != -1)
    {
        char resultStr[15];
        memset(&resultStr, 0, sizeof(resultStr));
        NukiOpener::cmdResultToString(result, resultStr);
        _network->publishKeypadCommandResult(resultStr);
    }
}

void NukiOpenerWrapper::onKeypadJsonCommandReceived(const char *value)
{
    if(!isPinValid())
    {
        _network->publishKeypadJsonCommandResult("noValidPinSet");
        return;
    }

    if(!_preferences->getBool(preference_keypad_control_enabled))
    {
        _network->publishKeypadJsonCommandResult("keypadControlDisabled");
        return;
    }

    if(!_hasKeypad)
    {
        if(_configRead && _nukiConfigValid)
        {
            _network->publishKeypadJsonCommandResult("keypadNotAvailable");
            return;
        }

        _network->publishKeypadJsonCommandResult("configNotReady");
        return;
    }

    if(!_keypadEnabled)
    {
        _network->publishKeypadJsonCommandResult("keypadDisabled");
        return;
    }

    JsonDocument json;
    DeserializationError jsonError = deserializeJson(json, value);

    if(jsonError)
    {
        _network->publishKeypadJsonCommandResult("invalidJson");
        return;
    }

    char oldName[21];
    const char *action = json["action"].as<const char*>();
    uint16_t codeId = json["codeId"].as<unsigned int>();
    uint32_t code;
    uint8_t enabled;
    uint8_t timeLimited;
    String name;
    String allowedFrom;
    String allowedUntil;
    String allowedWeekdays;
    String allowedFromTime;
    String allowedUntilTime;

    if(json.containsKey("code")) code = json["code"].as<unsigned int>();
    else code = 12;

    if(json.containsKey("enabled")) enabled = json["enabled"].as<unsigned int>();
    else enabled = 2;

    if(json.containsKey("timeLimited")) timeLimited = json["timeLimited"].as<unsigned int>();
    else timeLimited = 2;

    if(json.containsKey("name")) name = json["name"].as<String>();
    if(json.containsKey("allowedFrom")) allowedFrom = json["allowedFrom"].as<String>();
    if(json.containsKey("allowedUntil")) allowedUntil = json["allowedUntil"].as<String>();
    if(json.containsKey("allowedWeekdays")) allowedWeekdays = json["allowedWeekdays"].as<String>();
    if(json.containsKey("allowedFromTime")) allowedFromTime = json["allowedFromTime"].as<String>();
    if(json.containsKey("allowedUntilTime")) allowedUntilTime = json["allowedUntilTime"].as<String>();

    if(action)
    {
        bool idExists = false;

        if(codeId)
        {
            idExists = std::find(_keypadCodeIds.begin(), _keypadCodeIds.end(), codeId) != _keypadCodeIds.end();
        }

        Nuki::CmdResult result = (Nuki::CmdResult)-1;
        _retryCount = 0;

        while(_retryCount < _nrOfRetries + 1)
        {
            if(strcmp(action, "delete") == 0) {
                if(idExists)
                {
                    result = _nukiOpener.deleteKeypadEntry(codeId);
                    delay(250);
                    Log->print(F("Delete keypad code: "));
                    Log->println((int)result);
                }
                else
                {
                    _network->publishKeypadJsonCommandResult("noExistingCodeIdSet");
                    return;
                }
            }
            else if(strcmp(action, "add") == 0 || strcmp(action, "update") == 0)
            {
                if(name.length() < 1)
                {
                    if (strcmp(action, "update") != 0)
                    {
                        _network->publishKeypadJsonCommandResult("noNameSet");
                        return;
                    }
                }

                if(!code == 12)
                {
                    String codeStr = json["code"].as<String>();
                    bool codeValid = code > 100000 && code < 1000000 && (codeStr.indexOf('0') == -1);

                    if (!codeValid)
                    {
                        _network->publishKeypadJsonCommandResult("noValidCodeSet");
                        return;
                    }
                }
                else if (strcmp(action, "update") != 0)
                {
                    _network->publishKeypadJsonCommandResult("noCodeSet");
                    return;
                }

                unsigned int allowedFromAr[6];
                unsigned int allowedUntilAr[6];
                unsigned int allowedFromTimeAr[2];
                unsigned int allowedUntilTimeAr[2];
                uint8_t allowedWeekdaysInt = 0;

                if(timeLimited == 1)
                {
                    if(allowedFrom.length() > 0)
                    {
                        if(allowedFrom.length() == 19)
                        {
                            allowedFromAr[0] = (uint16_t)allowedFrom.substring(0, 4).toInt();
                            allowedFromAr[1] = (uint8_t)allowedFrom.substring(5, 7).toInt();
                            allowedFromAr[2] = (uint8_t)allowedFrom.substring(8, 10).toInt();
                            allowedFromAr[3] = (uint8_t)allowedFrom.substring(11, 13).toInt();
                            allowedFromAr[4] = (uint8_t)allowedFrom.substring(14, 16).toInt();
                            allowedFromAr[5] = (uint8_t)allowedFrom.substring(17, 19).toInt();

                            if(allowedFromAr[0] < 2000 || allowedFromAr[0] > 3000 || allowedFromAr[1] < 1 || allowedFromAr[1] > 12 || allowedFromAr[2] < 1 || allowedFromAr[2] > 31 || allowedFromAr[3] < 0 || allowedFromAr[3] > 23 || allowedFromAr[4] < 0 || allowedFromAr[4] > 59 || allowedFromAr[5] < 0 || allowedFromAr[5] > 59)
                            {
                                _network->publishKeypadJsonCommandResult("invalidAllowedFrom");
                                return;
                            }
                        }
                        else
                        {
                            _network->publishKeypadJsonCommandResult("invalidAllowedFrom");
                            return;
                        }
                    }

                    if(allowedUntil.length() > 0)
                    {
                        if(allowedUntil.length() > 0 == 19)
                        {
                            allowedUntilAr[0] = (uint16_t)allowedUntil.substring(0, 4).toInt();
                            allowedUntilAr[1] = (uint8_t)allowedUntil.substring(5, 7).toInt();
                            allowedUntilAr[2] = (uint8_t)allowedUntil.substring(8, 10).toInt();
                            allowedUntilAr[3] = (uint8_t)allowedUntil.substring(11, 13).toInt();
                            allowedUntilAr[4] = (uint8_t)allowedUntil.substring(14, 16).toInt();
                            allowedUntilAr[5] = (uint8_t)allowedUntil.substring(17, 19).toInt();

                            if(allowedUntilAr[0] < 2000 || allowedUntilAr[0] > 3000 || allowedUntilAr[1] < 1 || allowedUntilAr[1] > 12 || allowedUntilAr[2] < 1 || allowedUntilAr[2] > 31 || allowedUntilAr[3] < 0 || allowedUntilAr[3] > 23 || allowedUntilAr[4] < 0 || allowedUntilAr[4] > 59 || allowedUntilAr[5] < 0 || allowedUntilAr[5] > 59)
                            {
                                _network->publishKeypadJsonCommandResult("invalidAllowedUntil");
                                return;
                            }
                        }
                        else
                        {
                            _network->publishKeypadJsonCommandResult("invalidAllowedUntil");
                            return;
                        }
                    }

                    if(allowedFromTime.length() > 0)
                    {
                        if(allowedFromTime.length() == 5)
                        {
                            allowedFromTimeAr[0] = (uint8_t)allowedFromTime.substring(0, 2).toInt();
                            allowedFromTimeAr[1] = (uint8_t)allowedFromTime.substring(3, 5).toInt();

                            if(allowedFromTimeAr[0] < 0 || allowedFromTimeAr[0] > 23 || allowedFromTimeAr[1] < 0 || allowedFromTimeAr[1] > 59)
                            {
                                _network->publishKeypadJsonCommandResult("invalidAllowedFromTime");
                                return;
                            }
                        }
                        else
                        {
                            _network->publishKeypadJsonCommandResult("invalidAllowedFromTime");
                            return;
                        }
                    }

                    if(allowedUntilTime.length() > 0)
                    {
                        if(allowedUntilTime.length() == 5)
                        {
                            allowedUntilTimeAr[0] = (uint8_t)allowedUntilTime.substring(0, 2).toInt();
                            allowedUntilTimeAr[1] = (uint8_t)allowedUntilTime.substring(3, 5).toInt();

                            if(allowedUntilTimeAr[0] < 0 || allowedUntilTimeAr[0] > 23 || allowedUntilTimeAr[1] < 0 || allowedUntilTimeAr[1] > 59)
                            {
                                _network->publishKeypadJsonCommandResult("invalidAllowedUntilTime");
                                return;
                            }
                        }
                        else
                        {
                            _network->publishKeypadJsonCommandResult("invalidAllowedUntilTime");
                            return;
                        }
                    }

                    if(allowedWeekdays.indexOf("mon") >= 0) allowedWeekdaysInt += 64;
                    if(allowedWeekdays.indexOf("tue") >= 0) allowedWeekdaysInt += 32;
                    if(allowedWeekdays.indexOf("wed") >= 0) allowedWeekdaysInt += 16;
                    if(allowedWeekdays.indexOf("thu") >= 0) allowedWeekdaysInt += 8;
                    if(allowedWeekdays.indexOf("fri") >= 0) allowedWeekdaysInt += 4;
                    if(allowedWeekdays.indexOf("sat") >= 0) allowedWeekdaysInt += 2;
                    if(allowedWeekdays.indexOf("sun") >= 0) allowedWeekdaysInt += 1;
                }

                if(strcmp(action, "add") == 0)
                {
                    NukiOpener::NewKeypadEntry entry;
                    memset(&entry, 0, sizeof(entry));
                    size_t nameLen = name.length();
                    memcpy(&entry.name, name.c_str(), nameLen > 20 ? 20 : nameLen);
                    entry.code = code;
                    entry.timeLimited = timeLimited == 1 ? 1 : 0;

                    if(allowedFrom.length() > 0)
                    {
                        entry.allowedFromYear = allowedFromAr[0];
                        entry.allowedFromMonth = allowedFromAr[1];
                        entry.allowedFromDay = allowedFromAr[2];
                        entry.allowedFromHour = allowedFromAr[3];
                        entry.allowedFromMin = allowedFromAr[4];
                        entry.allowedFromSec = allowedFromAr[5];
                    }

                    if(allowedUntil.length() > 0)
                    {
                        entry.allowedUntilYear = allowedUntilAr[0];
                        entry.allowedUntilMonth = allowedUntilAr[1];
                        entry.allowedUntilDay = allowedUntilAr[2];
                        entry.allowedUntilHour = allowedUntilAr[3];
                        entry.allowedUntilMin = allowedUntilAr[4];
                        entry.allowedUntilSec = allowedUntilAr[5];
                    }

                    entry.allowedWeekdays = allowedWeekdaysInt;

                    if(allowedFromTime.length() > 0)
                    {
                        entry.allowedFromTimeHour = allowedFromTimeAr[0];
                        entry.allowedFromTimeMin = allowedFromTimeAr[1];
                    }

                    if(allowedUntilTime.length() > 0)
                    {
                        entry.allowedUntilTimeHour = allowedUntilTimeAr[0];
                        entry.allowedUntilTimeMin = allowedUntilTimeAr[1];
                    }

                    result = _nukiOpener.addKeypadEntry(entry);
                    delay(250);
                    Log->print(F("Add keypad code: "));
                    Log->println((int)result);
                }
                else if (strcmp(action, "update") == 0)
                {
                    if(!codeId)
                    {
                        _network->publishKeypadJsonCommandResult("noCodeIdSet");
                        return;
                    }

                    if(!idExists)
                    {
                        _network->publishKeypadJsonCommandResult("noExistingCodeIdSet");
                        return;
                    }

                    Nuki::CmdResult resultKp = _nukiOpener.retrieveKeypadEntries(0, _preferences->getInt(preference_keypad_max_entries, MAX_KEYPAD));
                    delay(250);
                    bool foundExisting = false;

                    if(resultKp == Nuki::CmdResult::Success)
                    {
                        std::list<NukiOpener::KeypadEntry> entries;
                        _nukiOpener.getKeypadEntries(&entries);

                        for(const auto& entry : entries)
                        {
                            if (codeId != entry.codeId) continue;
                            else foundExisting = true;

                            if(name.length() < 1)
                            {
                                memset(oldName, 0, sizeof(oldName));
                                memcpy(oldName, entry.name, sizeof(entry.name));
                            }
                            if(code == 12) code = entry.code;
                            if(enabled == 2) enabled = entry.enabled;
                            if(timeLimited == 2) timeLimited = entry.timeLimited;
                            if(allowedFrom.length() < 1)
                            {
                                allowedFrom = "old";
                                allowedFromAr[0] = entry.allowedFromYear;
                                allowedFromAr[1] = entry.allowedFromMonth;
                                allowedFromAr[2] = entry.allowedFromDay;
                                allowedFromAr[3] = entry.allowedFromHour;
                                allowedFromAr[4] = entry.allowedFromMin;
                                allowedFromAr[5] = entry.allowedFromSec;
                            }
                            if(allowedUntil.length() < 1)
                            {
                                allowedUntil = "old";
                                allowedUntilAr[0] = entry.allowedUntilYear;
                                allowedUntilAr[1] = entry.allowedUntilMonth;
                                allowedUntilAr[2] = entry.allowedUntilDay;
                                allowedUntilAr[3] = entry.allowedUntilHour;
                                allowedUntilAr[4] = entry.allowedUntilMin;
                                allowedUntilAr[5] = entry.allowedUntilSec;
                            }
                            if(allowedWeekdays.length() < 1) allowedWeekdaysInt = entry.allowedWeekdays;
                            if(allowedFromTime.length() < 1)
                            {
                                allowedFromTime = "old";
                                allowedFromTimeAr[0] = entry.allowedFromTimeHour;
                                allowedFromTimeAr[1] = entry.allowedFromTimeMin;
                            }

                            if(allowedUntilTime.length() < 1)
                            {
                                allowedUntilTime = "old";
                                allowedUntilTimeAr[0] = entry.allowedUntilTimeHour;
                                allowedUntilTimeAr[1] = entry.allowedUntilTimeMin;
                            }
                        }

                        if(!foundExisting)
                        {
                            _network->publishKeypadJsonCommandResult("failedToRetrieveExistingKeypadEntry");
                            return;
                        }
                    }
                    else
                    {
                        _network->publishKeypadJsonCommandResult("failedToRetrieveExistingKeypadEntry");
                        return;
                    }

                    NukiOpener::UpdatedKeypadEntry entry;

                    memset(&entry, 0, sizeof(entry));
                    entry.codeId = codeId;
                    entry.code = code;

                    if(!name.length() > 0)
                    {
                        size_t nameLen = strlen(oldName);
                        memcpy(&entry.name, oldName, nameLen > 20 ? 20 : nameLen);
                    }
                    else
                    {
                        size_t nameLen = name.length();
                        memcpy(&entry.name, name.c_str(), nameLen > 20 ? 20 : nameLen);
                    }
                    entry.enabled = enabled;
                    entry.timeLimited = timeLimited;

                    if(enabled == 1)
                    {
                        if(timeLimited == 1)
                        {
                            if(allowedFrom.length() > 0)
                            {
                                entry.allowedFromYear = allowedFromAr[0];
                                entry.allowedFromMonth = allowedFromAr[1];
                                entry.allowedFromDay = allowedFromAr[2];
                                entry.allowedFromHour = allowedFromAr[3];
                                entry.allowedFromMin = allowedFromAr[4];
                                entry.allowedFromSec = allowedFromAr[5];
                            }

                            if(allowedUntil.length() > 0)
                            {
                                entry.allowedUntilYear = allowedUntilAr[0];
                                entry.allowedUntilMonth = allowedUntilAr[1];
                                entry.allowedUntilDay = allowedUntilAr[2];
                                entry.allowedUntilHour = allowedUntilAr[3];
                                entry.allowedUntilMin = allowedUntilAr[4];
                                entry.allowedUntilSec = allowedUntilAr[5];
                            }

                            entry.allowedWeekdays = allowedWeekdaysInt;

                            if(allowedFromTime.length() > 0)
                            {
                                entry.allowedFromTimeHour = allowedFromTimeAr[0];
                                entry.allowedFromTimeMin = allowedFromTimeAr[1];
                            }

                            if(allowedUntilTime.length() > 0)
                            {
                                entry.allowedUntilTimeHour = allowedUntilTimeAr[0];
                                entry.allowedUntilTimeMin = allowedUntilTimeAr[1];
                            }
                        }
                    }

                    result = _nukiOpener.updateKeypadEntry(entry);
                    delay(250);
                    Log->print(F("Update keypad code: "));
                    Log->println((int)result);
                }
            }
            else
            {
                _network->publishKeypadJsonCommandResult("invalidAction");
                return;
            }

            if(result != Nuki::CmdResult::Success) {
                ++_retryCount;
            }
            else break;
        }

        updateKeypad(false);

        if((int)result != -1)
        {
            char resultStr[15];
            memset(&resultStr, 0, sizeof(resultStr));
            NukiOpener::cmdResultToString(result, resultStr);
            _network->publishKeypadJsonCommandResult(resultStr);
        }
    }
    else
    {
        _network->publishKeypadJsonCommandResult("noActionSet");
        return;
    }
}

void NukiOpenerWrapper::onTimeControlCommandReceived(const char *value)
{
    if(!_configRead || !_nukiConfigValid)
    {
        _network->publishTimeControlCommandResult("configNotReady");
        return;
    }

    if(!isPinValid())
    {
        _network->publishTimeControlCommandResult("noValidPinSet");
        return;
    }

    if(!_preferences->getBool(preference_timecontrol_control_enabled))
    {
        _network->publishTimeControlCommandResult("timeControlControlDisabled");
        return;
    }

    JsonDocument json;
    DeserializationError jsonError = deserializeJson(json, value);

    if(jsonError)
    {
        _network->publishTimeControlCommandResult("invalidJson");
        return;
    }

    const char *action = json["action"].as<const char*>();
    uint8_t entryId = json["entryId"].as<unsigned int>();
    uint8_t enabled;
    String weekdays;
    String time;
    String lockAction;
    NukiOpener::LockAction timeControlLockAction;

    if(json.containsKey("enabled")) enabled = json["enabled"].as<unsigned int>();
    else enabled = 2;

    if(json.containsKey("weekdays")) weekdays = json["weekdays"].as<String>();
    if(json.containsKey("time")) time = json["time"].as<String>();
    if(json.containsKey("lockAction")) lockAction = json["lockAction"].as<String>();

    if(lockAction.length() > 0)
    {
        timeControlLockAction = nukiOpenerInst->lockActionToEnum(lockAction.c_str());

        if((int)timeControlLockAction == 0xff)
        {
            _network->publishTimeControlCommandResult("invalidLockAction");
            return;
        }
    }

    if(action)
    {
        bool idExists = false;

        if(entryId)
        {
            idExists = std::find(_timeControlIds.begin(), _timeControlIds.end(), entryId) != _timeControlIds.end();
        }

        Nuki::CmdResult result = (Nuki::CmdResult)-1;
        _retryCount = 0;

        while(_retryCount < _nrOfRetries + 1)
        {
            if(strcmp(action, "delete") == 0) {
                if(idExists)
                {
                    result = _nukiOpener.removeTimeControlEntry(entryId);
                    delay(250);
                    Log->print(F("Delete time control: "));
                    Log->println((int)result);
                }
                else
                {
                    _network->publishTimeControlCommandResult("noExistingEntryIdSet");
                    return;
                }
            }
            else if(strcmp(action, "add") == 0 || strcmp(action, "update") == 0)
            {
                uint8_t timeHour;
                uint8_t timeMin;
                uint8_t weekdaysInt = 0;
                unsigned int timeAr[2];

                if(time.length() > 0)
                {
                    if(time.length() == 5)
                    {
                        timeAr[0] = (uint8_t)time.substring(0, 2).toInt();
                        timeAr[1] = (uint8_t)time.substring(3, 5).toInt();

                        if(timeAr[0] < 0 || timeAr[0] > 23 || timeAr[1] < 0 || timeAr[1] > 59)
                        {
                            _network->publishTimeControlCommandResult("invalidTime");
                            return;
                        }
                    }
                    else
                    {
                        _network->publishTimeControlCommandResult("invalidTime");
                        return;
                    }
                }

                if(weekdays.indexOf("mon") >= 0) weekdaysInt += 64;
                if(weekdays.indexOf("tue") >= 0) weekdaysInt += 32;
                if(weekdays.indexOf("wed") >= 0) weekdaysInt += 16;
                if(weekdays.indexOf("thu") >= 0) weekdaysInt += 8;
                if(weekdays.indexOf("fri") >= 0) weekdaysInt += 4;
                if(weekdays.indexOf("sat") >= 0) weekdaysInt += 2;
                if(weekdays.indexOf("sun") >= 0) weekdaysInt += 1;

                if(strcmp(action, "add") == 0)
                {
                    NukiOpener::NewTimeControlEntry entry;
                    memset(&entry, 0, sizeof(entry));
                    entry.weekdays = weekdaysInt;

                    if(time.length() > 0)
                    {
                        entry.timeHour = timeAr[0];
                        entry.timeMin = timeAr[1];
                    }

                    entry.lockAction = timeControlLockAction;
                    result = _nukiOpener.addTimeControlEntry(entry);
                    delay(250);
                    Log->print(F("Add time control: "));
                    Log->println((int)result);
                }
                else if (strcmp(action, "update") == 0)
                {
                    if(!idExists)
                    {
                        _network->publishTimeControlCommandResult("noExistingEntryIdSet");
                        return;
                    }

                    Nuki::CmdResult resultTc = _nukiOpener.retrieveTimeControlEntries();
                    delay(250);
                    bool foundExisting = false;

                    if(resultTc == Nuki::CmdResult::Success)
                    {
                        std::list<NukiOpener::TimeControlEntry> timeControlEntries;
                        _nukiOpener.getTimeControlEntries(&timeControlEntries);

                        for(const auto& entry : timeControlEntries)
                        {
                            if (entryId != entry.entryId) continue;
                            else foundExisting = true;

                            if(enabled == 2) enabled = entry.enabled;
                            if(weekdays.length() < 1) weekdaysInt = entry.weekdays;
                            if(time.length() < 1)
                            {
                                time = "old";
                                timeAr[0] = entry.timeHour;
                                timeAr[1] = entry.timeMin;
                            }
                            if(lockAction.length() < 1) timeControlLockAction = entry.lockAction;
                        }

                        if(!foundExisting)
                        {
                            _network->publishTimeControlCommandResult("failedToRetrieveExistingKeypadEntry");
                            return;
                        }
                    }
                    else
                    {
                        _network->publishTimeControlCommandResult("failedToRetrieveExistingKeypadEntry");
                        return;
                    }

                    NukiOpener::TimeControlEntry entry;
                    memset(&entry, 0, sizeof(entry));
                    entry.entryId = entryId;
                    entry.enabled = enabled;
                    entry.weekdays = weekdaysInt;

                    if(time.length() > 0)
                    {
                        entry.timeHour = timeAr[0];
                        entry.timeMin = timeAr[1];
                    }

                    entry.lockAction = timeControlLockAction;
                    result = _nukiOpener.updateTimeControlEntry(entry);
                    delay(250);
                    Log->print(F("Update time control: "));
                    Log->println((int)result);
                }
            }
            else
            {
                _network->publishTimeControlCommandResult("invalidAction");
                return;
            }

            if(result != Nuki::CmdResult::Success) {
                ++_retryCount;
            }
            else break;
        }

        if((int)result != -1)
        {
            char resultStr[15];
            memset(&resultStr, 0, sizeof(resultStr));
            NukiOpener::cmdResultToString(result, resultStr);
            _network->publishTimeControlCommandResult(resultStr);
        }

        _nextConfigUpdateTs = (esp_timer_get_time() / 1000) + 300;
    }
    else
    {
        _network->publishTimeControlCommandResult("noActionSet");
        return;
    }
}

const NukiOpener::OpenerState &NukiOpenerWrapper::keyTurnerState()
{
    return _keyTurnerState;
}

const bool NukiOpenerWrapper::isPaired() const
{
    return _paired;
}

const bool NukiOpenerWrapper::hasKeypad() const
{
    return _hasKeypad;
}

const BLEAddress NukiOpenerWrapper::getBleAddress() const
{
    return _nukiOpener.getBleAddress();
}

BleScanner::Scanner *NukiOpenerWrapper::bleScanner()
{
    return _bleScanner;
}

void NukiOpenerWrapper::notify(Nuki::EventType eventType)
{
    if(eventType == Nuki::EventType::KeyTurnerStatusUpdated)
    {
        Log->println("KeyTurnerStatusUpdated");
        _statusUpdated = true;
        _network->publishStatusUpdated(_statusUpdated);
    }
}

void NukiOpenerWrapper::readConfig()
{
    Nuki::CmdResult result = (Nuki::CmdResult)-1;
    _retryCount = 0;

    while(_retryCount < _nrOfRetries + 1)
    {
        Nuki::CmdResult result = _nukiOpener.requestConfig(&_nukiConfig);
        delay(250);
        _nukiConfigValid = result == Nuki::CmdResult::Success;

        if(!_nukiConfigValid) {
            ++_retryCount;
        }
        else break;
    }

    char resultStr[20];
    NukiOpener::cmdResultToString(result, resultStr);
    Log->println(resultStr);
    postponeBleWatchdog();
}

void NukiOpenerWrapper::readAdvancedConfig()
{
    Nuki::CmdResult result = (Nuki::CmdResult)-1;
    _retryCount = 0;

    while(_retryCount < _nrOfRetries + 1)
    {
         result = _nukiOpener.requestAdvancedConfig(&_nukiAdvancedConfig);
         delay(250);
        _nukiAdvancedConfigValid = result == Nuki::CmdResult::Success;

        if(!_nukiAdvancedConfigValid) {
            ++_retryCount;
        }
        else break;
    }

    char resultStr[20];
    NukiOpener::cmdResultToString(result, resultStr);
    Log->println(resultStr);
    postponeBleWatchdog();
}

void NukiOpenerWrapper::setupHASS()
{
    if(!_nukiConfigValid) return;
    if(_preferences->getUInt(preference_nuki_id_opener, 0) != _nukiConfig.nukiId) return;

    String baseTopic = _preferences->getString(preference_mqtt_opener_path);
    char uidString[20];
    itoa(_nukiConfig.nukiId, uidString, 16);

    if(_preferences->getBool(preference_opener_continuous_mode)) _network->publishHASSConfig((char*)"Opener", baseTopic.c_str(), (char*)_nukiConfig.name, uidString, _firmwareVersion.c_str(), _hardwareVersion.c_str(), _publishAuthData, _hasKeypad, (char*)"deactivateCM", (char*)"activateCM", (char*)"electricStrikeActuation");
    else _network->publishHASSConfig((char*)"Opener", baseTopic.c_str(), (char*)_nukiConfig.name, uidString, _firmwareVersion.c_str(), _hardwareVersion.c_str(), _publishAuthData, _hasKeypad, (char*)"deactivateRTO", (char*)"activateRTO", (char*)"electricStrikeActuation");

    _hassSetupCompleted = true;

    Log->println("HASS setup for opener completed.");
}

void NukiOpenerWrapper::disableHASS()
{
    if(!_nukiConfigValid) // only ask for config once to save battery life
    {
        Nuki::CmdResult result = (Nuki::CmdResult)-1;
        _retryCount = 0;

        while(_retryCount < _nrOfRetries + 1)
        {
            result = _nukiOpener.requestConfig(&_nukiConfig);
            delay(250);
            _nukiConfigValid = result == Nuki::CmdResult::Success;

            if(!_nukiConfigValid) {
                ++_retryCount;
            }
            else break;
        }
    }

    if(_nukiConfigValid)
    {
        char uidString[20];
        itoa(_nukiConfig.nukiId, uidString, 16);
        _network->removeHASSConfig(uidString);
    }
    else Log->println(F("Unable to disable HASS. Invalid config received."));
}

void NukiOpenerWrapper::printCommandResult(Nuki::CmdResult result)
{
    char resultStr[15];
    NukiOpener::cmdResultToString(result, resultStr);
    Log->println(resultStr);
}

std::string NukiOpenerWrapper::firmwareVersion() const
{
    return _firmwareVersion;
}

std::string NukiOpenerWrapper::hardwareVersion() const
{
    return _hardwareVersion;
}

void NukiOpenerWrapper::disableWatchdog()
{
    _restartBeaconTimeout = -1;
}

void NukiOpenerWrapper::updateGpioOutputs()
{
    using namespace NukiOpener;

    const auto& pinConfiguration = _gpio->pinConfiguration();

    const LockState& lockState = _keyTurnerState.lockState;

    bool rtoActive = _keyTurnerState.lockState == LockState::RTOactive;
    bool cmActive = _keyTurnerState.nukiState == State::ContinuousMode;

    for(const auto& entry : pinConfiguration)
    {
        switch(entry.role)
        {
            case PinRole::OutputHighRtoActive:
                _gpio->setPinOutput(entry.pin, rtoActive ? HIGH : LOW);
                break;
            case PinRole::OutputHighCmActive:
                _gpio->setPinOutput(entry.pin, cmActive ? HIGH : LOW);
                break;
            case PinRole::OutputHighRtoOrCmActive:
                _gpio->setPinOutput(entry.pin, rtoActive || cmActive ? HIGH : LOW);
                break;
        }
    }
}