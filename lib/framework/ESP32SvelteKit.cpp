/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2025 theelims
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <ESP32SvelteKit.h>
#include <DmaReserve.h>
#include <MdnsRegistrationPolicy.h>
#include <esp_heap_caps.h>

//🌙 added to telemetry
bool safeModeMB = false; // 🌙 see .h
bool restartNeeded = false; // 🌙 see .h
bool saveNeeded = false; // 🌙 see.h

#if FT_MOONBASE == 1
bool moonbaseGoldenConfigPresent();
#endif

ESP32SvelteKit::ESP32SvelteKit(PsychicHttpServer *server, unsigned int numberEndpoints) : _server(server),
                                                                                          _numberEndpoints(numberEndpoints),
                                                                                          _featureService(server, &_socket),
                                                                                          _securitySettingsService(server, &ESPFS),
#if FT_ENABLED(FT_WIFI) // 🌙
                                                                                          _wifiSettingsService(server, &ESPFS, &_securitySettingsService, &_socket),
                                                                                          _wifiScanner(server, &_securitySettingsService),
                                                                                          _wifiStatus(server, &_securitySettingsService),
                                                                                          _apSettingsService(server, &ESPFS, &_securitySettingsService),
                                                                                          _apStatus(server, &_securitySettingsService, &_apSettingsService),
#endif
#if FT_ENABLED(FT_ETHERNET)
                                                                                          _ethernetSettingsService(server, &ESPFS, &_securitySettingsService, &_socket),
                                                                                          _ethernetStatus(server, &_securitySettingsService),
#endif
                                                                                          _socket(server, &_securitySettingsService, AuthenticationPredicates::IS_AUTHENTICATED),
                                                                                          _notificationService(&_socket),
#if FT_ENABLED(FT_NTP)
                                                                                          _ntpSettingsService(server, &ESPFS, &_securitySettingsService),
                                                                                          _ntpStatus(server, &_securitySettingsService),
#endif
#if FT_ENABLED(FT_UPLOAD_FIRMWARE)
                                                                                          _uploadFirmwareService(server, &_securitySettingsService, &_socket),
#endif
#if FT_ENABLED(FT_DOWNLOAD_FIRMWARE)
                                                                                          _downloadFirmwareService(server, &_securitySettingsService, &_socket),
#endif
#if FT_ENABLED(FT_MQTT)
                                                                                          _mqttSettingsService(server, &ESPFS, &_securitySettingsService),
                                                                                          _mqttStatus(server, &_mqttSettingsService, &_securitySettingsService),
#endif
#if FT_ENABLED(FT_SECURITY)
                                                                                          _authenticationService(server, &_securitySettingsService),
#endif
#if FT_ENABLED(FT_SLEEP)
                                                                                          _sleepService(server, &_securitySettingsService),
#endif
#if FT_ENABLED(FT_BATTERY)
                                                                                          _batteryService(&_socket),
#endif
#if FT_ENABLED(FT_ANALYTICS)
                                                                                          _analyticsService(&_socket),
#endif
                                                                                          _restartService(server, &_securitySettingsService),
                                                                                          _factoryResetService(server, &ESPFS, &_securitySettingsService),
#if FT_ENABLED(FT_COREDUMP)
                                                                                          _coreDump(server, &_securitySettingsService),
#endif
                                                                                          _systemStatus(server, &_securitySettingsService)
{
}

bool ESP32SvelteKit::begin()
{
    ESP_LOGV(SVK_TAG, "Loading settings from files system");
    ESPFS.begin(true);

#if FT_ENABLED(FT_WIFI) // 🌙
    // 🌙 Load WiFi state early so getSystemHostname() returns the configured hostname
    // before Ethernet init (which needs it for ETH.setHostname/DHCP).
    _wifiSettingsService.loadState();
#endif

#if FT_ENABLED(FT_ETHERNET)
    // 🌙 Ethernet DHCP uses the unified system hostname (WiFi hostname preferred, falls back to Ethernet's own)
    _ethernetSettingsService.systemHostnameProvider = [this]() -> String { return getSystemHostname(); };
    _ethernetSettingsService.initEthernet();
#endif

#if FT_ENABLED(FT_WIFI) // 🌙
    if (!_mdnsLifecycleHooksRegistered)
    {
        _mdnsLifecycleHooksRegistered = true;
        WiFi.onEvent(
            [this](WiFiEvent_t event, WiFiEventInfo_t) {
                switch (event)
                {
                case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                    stopMdns();
                    _lastMdnsMaintain = 0;
                    _mdnsAnnounceFailures = 0;
                    break;
                case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                    _lastMdnsMaintain = 0;
                    if (!_mdnsStarted)
                        startMdns();
                    else
                        announceMdnsSta();
                    break;
                default:
                    break;
                }
            });
    }
    _wifiSettingsService.initWiFi();
#endif

    // SvelteKit uses a lot of handlers, so we need to increase the max_uri_handlers
    // WWWData has 77->27!! Endpoints, Framework has 27, and each module has 4 // 🌙 updated numbers
    _server->config.max_uri_handlers = _numberEndpoints;
    _server->listen(80);

#ifdef EMBED_WWW
    // Serve static resources from PROGMEM
    ESP_LOGV(SVK_TAG, "Registering routes from PROGMEM static resources");
    WWWData::registerRoutes(
        [&](const String &uri, const String &contentType, const uint8_t *content, size_t len, const char *contentEncoding)
        {
            PsychicHttpRequestCallback requestHandler = [contentType, content, len, contentEncoding](PsychicRequest *request)
            {
                PsychicResponse response(request);
                response.setCode(200);
                response.setContentType(contentType.c_str());
                if (contentEncoding && contentEncoding[0] != '\0') {
                    response.addHeader("Content-Encoding", contentEncoding);
                }
                response.addHeader("Cache-Control", "no-cache"); // 🌙 modified after a user got annoyed ;-)
                // response.addHeader("Cache-Control", "public, immutable, max-age=31536000"); // 🌙 this is original
                size_t chunkSize = 4096;
                uint8_t *chunk = static_cast<uint8_t *>(PsychicResponse::allocateResponseBuffer(chunkSize));
                if (!chunk) {
                    chunkSize = 1024;
                    chunk = static_cast<uint8_t *>(PsychicResponse::allocateResponseBuffer(chunkSize));
                }
                if (!chunk) {
                    chunkSize = 256;
                    chunk = static_cast<uint8_t *>(PsychicResponse::allocateResponseBuffer(chunkSize));
                }
                if (!chunk) return PsychicResponse::sendServiceUnavailable(request);
                response.sendHeaders();
                for (size_t offset = 0; offset < len; offset += chunkSize)
                {
                    size_t size = len - offset < chunkSize ? len - offset : chunkSize;
                    memcpy(chunk, content + offset, size);
                    esp_err_t err = response.sendChunk(chunk, size);
                    if (err != ESP_OK) {
                        heap_caps_free(chunk);
                        return err;
                    }
                }
                heap_caps_free(chunk);
                return response.finishChunking();
            };
            PsychicWebHandler *handler = new PsychicWebHandler();
            handler->onRequest(requestHandler);
            _server->on(uri.c_str(), HTTP_GET, handler);

            // Set default end-point for all non matching requests
            // this is easier than using webServer.onNotFound()
            if (uri.equals("/index.html"))
            {
                _server->defaultEndpoint->setHandler(handler);
            }
        });
#else
    // Serve static resources from /www/
    ESP_LOGV(SVK_TAG, "Registering routes from FS /www/ static resources");
    _server->serveStatic("/_app/", ESPFS, "/www/_app/");
    _server->serveStatic("/favicon.png", ESPFS, "/www/favicon.png");
    //  Serving all other get requests with "/www/index.htm"
    _server->onNotFound([](PsychicRequest *request)
                        {
        if (request->method() == HTTP_GET) {
            PsychicFileResponse response(request, ESPFS, "/www/index.html", "text/html");
            return response.send();
            // String url = "http://" + request->host() + "/index.html";
            // request->redirect(url.c_str());
        } });
#endif

    // Serve static resources from /.config/ if set by platformio.ini
#if SERVE_CONFIG_FILES
    _server->serveStatic("/.config/", ESPFS, "/.config/"); // 🌙 use /.config (hidden folder)
#endif

#if defined(ENABLE_CORS)
    ESP_LOGV(SVK_TAG, "Enabling CORS headers");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", CORS_ORIGIN);
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Accept, Content-Type, Authorization");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Credentials", "true");
#endif

#ifdef SERIAL_INFO
    Serial.printf("Running Firmware Version: %s\n", APP_VERSION);
#endif

    // Start the services
    _socket.begin();
    _notificationService.begin();
    _factoryResetService.begin();
    _featureService.begin();
    _restartService.begin();
    _systemStatus.begin();
#if FT_ENABLED(FT_WIFI) // 🌙
    _apStatus.begin();
    _apSettingsService.begin();
    _wifiSettingsService.begin();
    _wifiScanner.begin();
    _wifiStatus.begin();
    ensureMdns();
#endif
    _socket.registerEvent("status"); // 🌙 system status event (saveNeeded, restartNeeded, safeMode, hostName)
#if FT_ENABLED(FT_ETHERNET)
    _ethernetSettingsService.begin();
    _ethernetStatus.begin();
#endif


#if FT_ENABLED(FT_COREDUMP)
    _coreDump.begin();
#endif

#if FT_ENABLED(FT_UPLOAD_FIRMWARE)
    _uploadFirmwareService.begin();
#endif

#if FT_ENABLED(FT_DOWNLOAD_FIRMWARE)
    _downloadFirmwareService.begin();
#endif

#if FT_ENABLED(FT_NTP)
    _ntpSettingsService.begin();
    _ntpStatus.begin();
#endif

#if FT_ENABLED(FT_MQTT)
    _mqttSettingsService.begin();
    _mqttStatus.begin();
#endif

#if FT_ENABLED(FT_SECURITY)
    _authenticationService.begin();
    _securitySettingsService.begin();
#endif

#if FT_ENABLED(FT_SLEEP)
    _sleepService.begin();
    _sleepService.attachOnSleepCallback([&]()
                                        {   ESP_LOGI(SVK_TAG, "Attempting to stop server");
                                            for (auto client : _server->getClientList())
                                            {
                                                client->close();
                                            }
                                            vTaskDelete(_loopTaskHandle);
                                            ESP_LOGI(SVK_TAG, "Server stopped"); });
#if FT_ENABLED(FT_MQTT)
    _sleepService.attachOnSleepCallback([&]()
                                        { _mqttSettingsService.disconnect(); });
#endif
#endif

#if FT_ENABLED(FT_BATTERY)
    _batteryService.begin();
#endif

#if FT_ENABLED(FT_ANALYTICS)
    _analyticsService.begin();
#endif

    // Start the loop task
    ESP_LOGV(SVK_TAG, "Starting loop task");
    xTaskCreatePinnedToCore(
        this->_loopImpl,                      // Function that should be called
        "ESP32 SvelteKit Loop",               // Name of the task (for debugging)
        SVELTEKIT_STACK_SIZE,                 // Stack size (bytes)  🌙
        this,                                 // Pass reference to this class instance
        (tskIDLE_PRIORITY + 2),               // task priority
        &_loopTaskHandle,                     // Task handle
#ifdef CONFIG_FREERTOS_UNICORE
                       0  // Single-core: use Core 0 (only option)
#else
                       ESP32SVELTEKIT_RUNNING_CORE           // Pin to application core
#endif
    );
    return true;
}

bool ESP32SvelteKit::startMdns()
{
    if (_mdnsStarted)
        return true;

    if (!WiFi.isConnected())
        return false;

    const size_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largestInternal < 1024)
    {
        ESP_LOGW(SVK_TAG, "mDNS defer start: largest internal=%u", (unsigned)largestInternal);
        return false;
    }

    String mdnsHostname = getSystemHostname();
    mdnsHostname.toLowerCase();
    if (!MDNS.begin(mdnsHostname.c_str()))
    {
        ESP_LOGE(SVK_TAG, "mDNS failed to start for %s (largest internal=%u)", mdnsHostname.c_str(),
                 (unsigned)largestInternal);
        return false;
    }

    MDNS.setInstanceName(mdnsHostname);
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("ws", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "Firmware Version", APP_VERSION);
    _mdnsStarted = true;
    _lastMdnsAnnounceOk = 0;
    _mdnsAnnounceFailures = 0;
    announceMdnsSta();
    ESP_LOGI(SVK_TAG, "mDNS started: http://%s.local", mdnsHostname.c_str());
    return true;
}

void ESP32SvelteKit::stopMdns()
{
    if (!_mdnsStarted)
        return;
    MDNS.end();
    _mdnsStarted = false;
    _lastMdnsAnnounceOk = 0;
    _mdnsAnnounceFailures = 0;
}

bool ESP32SvelteKit::restartMdns()
{
    stopMdns();
    _lastMdnsMaintain = 0;
    return startMdns();
}

bool ESP32SvelteKit::announceMdnsSta()
{
    esp_netif_t *netif = WiFi.STA.netif();
    if (!netif)
        return false;
    esp_err_t enable = mdns_netif_action(netif, MDNS_EVENT_ENABLE_IP4);
    esp_err_t announce = mdns_netif_action(netif, MDNS_EVENT_ANNOUNCE_IP4);
    if (mdnsAnnounceSucceeded((int)enable, (int)announce))
    {
        _lastMdnsAnnounceOk = millis();
        return true;
    }
    return false;
}

void ESP32SvelteKit::ensureMdns()
{
    if (!WiFi.isConnected())
        return;

    const size_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mdnsShouldStart(_mdnsStarted, false, true, largestInternal, 1024))
        startMdns();
}

void ESP32SvelteKit::maintainMdns()
{
    if (!WiFi.isConnected())
        return;

    const uint32_t now = millis();
    constexpr uint32_t kHealthyInterval = 60000UL;
    constexpr uint32_t kRecoveryInterval = 15000UL;
    constexpr uint32_t kStaleMs = 90000UL;
    constexpr uint8_t kMaxAnnounceFailures = 3;
    constexpr size_t kAnnounceMinInternal = 2048;
    const uint32_t interval = mdnsMaintainIntervalMs(_mdnsStarted, now, _lastMdnsAnnounceOk, kHealthyInterval,
                                                     kRecoveryInterval, kStaleMs);
    if (_lastMdnsMaintain && (uint32_t)(now - _lastMdnsMaintain) < interval)
        return;
    _lastMdnsMaintain = now;

    const size_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (_mdnsAnnounceFailures >= kMaxAnnounceFailures ||
        mdnsShouldRestart(_mdnsStarted, false, true, largestInternal, now, _lastMdnsAnnounceOk, kStaleMs, 2048))
    {
        ESP_LOGW(SVK_TAG, "mDNS stale/failed, restarting (largest internal=%u, fails=%u)", (unsigned)largestInternal,
                  (unsigned)_mdnsAnnounceFailures);
        restartMdns();
        return;
    }

    if (mdnsShouldStart(_mdnsStarted, false, true, largestInternal, 1024))
    {
        startMdns();
        return;
    }

    if (!mdnsShouldAnnounce(_mdnsStarted, false, true, largestInternal, kAnnounceMinInternal))
    {
        if (_mdnsStarted && _lastMdnsAnnounceOk != 0)
            _lastMdnsAnnounceOk = 0;
        return;
    }

    if (!announceMdnsSta())
    {
        _mdnsAnnounceFailures++;
        _lastMdnsAnnounceOk = 0;
    }
    else
        _mdnsAnnounceFailures = 0;
}

void ESP32SvelteKit::_loop()
{
    TickType_t xLastWakeTime = xTaskGetTickCount();

    bool wifi = false;
    bool ap = false;
    bool event = false;
    bool mqtt = false;
#if FT_ENABLED(FT_ETHERNET)
    bool eth = false;
#endif
    bool wifi_eth_combined = false;

    while (1)
    {
        wifi_eth_combined = false;
#if FT_ENABLED(FT_WIFI) // 🌙
        _wifiSettingsService.loop(); // 30 seconds
        if (dmaReserve::check(millis())) ensureMdns();
        maintainMdns();
        _apSettingsService.loop();   // 10 seconds
#endif
#if FT_ENABLED(FT_MQTT)
        _mqttSettingsService.loop(); // 5 seconds
#endif
#if FT_ENABLED(FT_ANALYTICS)
        _analyticsService.loop();
#endif
#if FT_ENABLED(FT_ETHERNET)
        _ethernetSettingsService.loop();
        eth = _ethernetStatus.isConnected();
        if (eth) { wifi_eth_combined = true; }
#endif

        // Query the connectivity status
#if FT_ENABLED(FT_WIFI) // 🌙
        wifi = _wifiStatus.isConnected();
        if (wifi) { wifi_eth_combined = true; }
        ap = _apStatus.isActive();
#endif
        event = _socket.getConnectedClients() > 0;
#if FT_ENABLED(FT_MQTT)
        mqtt = _mqttStatus.isConnected();
#endif

        // Update the system status
        if (wifi_eth_combined && mqtt)
        {
            _connectionStatus = ConnectionStatus::STA_MQTT;
        }
        else if (wifi_eth_combined)
        {
            _connectionStatus = event ? ConnectionStatus::STA_CONNECTED : ConnectionStatus::STA;
        }
        else if (ap)
        {
            _connectionStatus = event ? ConnectionStatus::AP_CONNECTED : ConnectionStatus::AP;
        }
        else
        {
            _connectionStatus = ConnectionStatus::OFFLINE;
        }

        // iterate over all loop functions
        for (auto &function : _loopFunctions)
        {
            function();
        }

        static int lastTime = 0;
        if (millis() - lastTime > 1000)
        {
            lastTime = millis();
            // 🌙 Emit system status flags independently of WiFi — works on all boards
            // (including ethernet-only boards where WiFiSettingsService doesn't exist)
            if (_socket.getConnectedClients()) {
                JsonDocument doc;
                doc["safeMode"] = safeModeMB;
                doc["restartNeeded"] = restartNeeded;
                doc["saveNeeded"] = saveNeeded;
                doc["hostName"] = getSystemHostname();
#if FT_MOONBASE == 1
                doc["goldenPresent"] = moonbaseGoldenConfigPresent();
#endif
                JsonObject jsonObject = doc.as<JsonObject>();
                _socket.emitEvent("status", jsonObject);
            }
            // 🌙 Compute lps values (used by both SystemStatus and AnalyticsService)
            uint32_t cpuHz = getCpuFrequencyMhz() * 1000000UL;
            lps_effects = (lps_all > 0 && lps_effects_cycles > 0) ? (uint16_t)((uint64_t)cpuHz * lps_all / lps_effects_cycles) : 0;
            lps_drivers = (lps_all > 0 && lps_drivers_cycles > 0) ? (uint16_t)((uint64_t)cpuHz * lps_all / lps_drivers_cycles) : 0;
            lps_all_snapshot = lps_all; // 🌙 latch before reset so SystemStatus reads a stable value
#if FT_ENABLED(FT_ANALYTICS)
            _analyticsService.lps_all = lps_all;
            _analyticsService.lps_effects = lps_effects;
            _analyticsService.lps_drivers = lps_drivers;
#endif
            lps_all = 0;
            lps_effects_cycles = 0;
            lps_drivers_cycles = 0;
#ifdef TELEPLOT_TASKS
            Serial.printf(">ESP32SveltekitTask:%i:%i\n", millis(), uxTaskGetStackHighWaterMark(NULL));
#endif
        }
        vTaskDelayUntil(&xLastWakeTime, ESP32SVELTEKIT_LOOP_INTERVAL / portTICK_PERIOD_MS);
    }
}
