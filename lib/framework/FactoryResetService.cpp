/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2023 - 2025 theelims
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <FactoryResetService.h>

using namespace std::placeholders;

FactoryResetService::FactoryResetService(PsychicHttpServer *server,
                                         FS *fs,
                                         SecurityManager *securityManager) : _server(server),
                                                                             fs(fs),
                                                                             _securityManager(securityManager)
{
}

void FactoryResetService::begin()
{
    _server->on(FACTORY_RESET_SERVICE_PATH,
                HTTP_POST,
                _securityManager->wrapRequest(std::bind(&FactoryResetService::handleRequest, this, _1), AuthenticationPredicates::IS_ADMIN));

    ESP_LOGV(SVK_TAG, "Registered POST endpoint: %s", FACTORY_RESET_SERVICE_PATH);
}

esp_err_t FactoryResetService::handleRequest(PsychicRequest *request)
{
    if (!clearStorage())
    {
        request->reply(500, "text/plain", "Factory reset failed to clear configuration storage");
        return ESP_OK;
    }
    request->reply(200);
    RestartService::restartNow();

    return ESP_OK;
}

void FactoryResetService::factoryReset()
{
    if (!clearStorage())
    {
        ESP_LOGE(SVK_TAG, "Factory reset failed to clear configuration storage");
        return;
    }
    RestartService::restartNow();
}

bool FactoryResetService::clearStorage()
{
    if (_resetHook) return _resetHook();

    File root = fs->open(FS_CONFIG_DIRECTORY);
    File file;
    while (file = root.openNextFile())
    {
        String path = file.path();
        file.close();
        if (!fs->remove(path)) return false;
    }
    return true;
}
