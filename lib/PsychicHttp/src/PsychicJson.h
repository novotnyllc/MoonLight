// PsychicJson.h
/*
  Async Response to use with ArduinoJson and AsyncWebServer
  Written by Andrew Melvin (SticilFace) with help from me-no-dev and BBlanchon.
  Ported to PsychicHttp by Zach Hoeken

*/
#ifndef PSYCHIC_JSON_H_
#define PSYCHIC_JSON_H_

#include "PsychicRequest.h"
#include "PsychicWebHandler.h"
#include "ChunkPrinter.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#if ARDUINOJSON_VERSION_MAJOR == 6
  #define ARDUINOJSON_6_COMPATIBILITY
  #ifndef DYNAMIC_JSON_DOCUMENT_SIZE
    #define DYNAMIC_JSON_DOCUMENT_SIZE 4096
  #endif
#endif


#ifndef JSON_BUFFER_SIZE
  #define JSON_BUFFER_SIZE 4*1024
#endif

constexpr const char *JSON_MIMETYPE = "application/json";

#if ARDUINOJSON_VERSION_MAJOR >= 7
class PsychicJsonAllocator : public ArduinoJson::Allocator
{
  public:
    void *allocate(size_t size) override
    {
      return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    void deallocate(void *pointer) override { heap_caps_free(pointer); }

    void *reallocate(void *pointer, size_t size) override
    {
      return heap_caps_realloc_prefer(pointer, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static PsychicJsonAllocator *instance()
    {
      static PsychicJsonAllocator allocator;
      return &allocator;
    }
};
#endif

/*
 * Json Response
 * */

class PsychicJsonResponse : public PsychicResponse
{
  protected:
    #ifdef ARDUINOJSON_5_COMPATIBILITY
      DynamicJsonBuffer _jsonBuffer;
    #elif ARDUINOJSON_VERSION_MAJOR == 6
      DynamicJsonDocument _jsonBuffer;
    #else
      JsonDocument _jsonBuffer;
    #endif

    JsonVariant _root;
    size_t _contentLength;

  public:
    #ifdef ARDUINOJSON_5_COMPATIBILITY
      PsychicJsonResponse(PsychicRequest *request, bool isArray = false);
    #elif ARDUINOJSON_VERSION_MAJOR == 6
      PsychicJsonResponse(PsychicRequest *request, bool isArray = false, size_t maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE);
    #else
      PsychicJsonResponse(PsychicRequest *request, bool isArray = false);
    #endif

    ~PsychicJsonResponse() {}

    JsonVariant &getRoot();
    size_t getLength();
    
    virtual esp_err_t send() override;
};

class PsychicJsonHandler : public PsychicWebHandler
{
  protected:
    PsychicJsonRequestCallback _onRequest;
    #if ARDUINOJSON_VERSION_MAJOR == 6
      const size_t _maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE;
    #endif

  public:
    #ifdef ARDUINOJSON_5_COMPATIBILITY
      PsychicJsonHandler();
      PsychicJsonHandler(PsychicJsonRequestCallback onRequest);
    #elif ARDUINOJSON_VERSION_MAJOR == 6
      PsychicJsonHandler(size_t maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE);
      PsychicJsonHandler(PsychicJsonRequestCallback onRequest, size_t maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE);
    #else
      PsychicJsonHandler();
      PsychicJsonHandler(PsychicJsonRequestCallback onRequest);
    #endif

    void onRequest(PsychicJsonRequestCallback fn);
    virtual esp_err_t handleRequest(PsychicRequest *request) override;
};

#endif
