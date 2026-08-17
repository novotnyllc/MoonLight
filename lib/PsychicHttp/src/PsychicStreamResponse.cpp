#include "PsychicStreamResponse.h"
#include "PsychicResponse.h"
#include "PsychicRequest.h"

PsychicStreamResponse::PsychicStreamResponse(PsychicRequest *request, const String& contentType)
 : PsychicResponse(request), _buffer(NULL) {

  setContentType(contentType.c_str());
  addHeader("Content-Disposition", "inline");
}

 
PsychicStreamResponse::PsychicStreamResponse(PsychicRequest *request, const String& contentType, const String& name)
  : PsychicResponse(request), _buffer(NULL) {

  setContentType(contentType.c_str());

  char buf[26+name.length()];
  snprintf(buf, sizeof (buf), "attachment; filename=\"%s\"", name.c_str());
  addHeader("Content-Disposition", buf);
}

 
PsychicStreamResponse::~PsychicStreamResponse()
{
  endSend();
}


esp_err_t PsychicStreamResponse::beginSend()
{
  if(_buffer)
    return ESP_OK;

  //Buffer to hold ChunkPrinter and stream buffer. Using placement new will keep us at a single allocation.
  size_t chunkSize = STREAM_CHUNK_SIZE;
  _buffer = (uint8_t*)malloc(chunkSize + sizeof(ChunkPrinter));
  if (!_buffer)
  {
    chunkSize = 256;
    _buffer = (uint8_t*)malloc(chunkSize + sizeof(ChunkPrinter));
  }
  
  if(!_buffer)
  {
    sendServiceUnavailable(_request);
    return ESP_FAIL;
  }

  _printer = new (_buffer) ChunkPrinter(this, _buffer + sizeof(ChunkPrinter), chunkSize);

  sendHeaders();
  return ESP_OK;
}


esp_err_t PsychicStreamResponse::endSend()
{
  esp_err_t err = ESP_OK;
  
  if(!_buffer)
    err = ESP_FAIL;
  else
  {
    _printer->~ChunkPrinter(); //flushed on destruct
    err = finishChunking();
    free(_buffer);
    _buffer = NULL;
  }
  return err;
}


void PsychicStreamResponse::flush()
{
  if(_buffer)
    _printer->flush();
}


size_t PsychicStreamResponse::write(uint8_t data)
{
  return _buffer ? _printer->write(data) : 0;
}


size_t PsychicStreamResponse::write(const uint8_t *buffer, size_t size)
{
  return _buffer ? _printer->write(buffer, size) : 0;
}


size_t PsychicStreamResponse::copyFrom(Stream &stream)
{
  if(_buffer)
    return _printer->copyFrom(stream);

  return 0;
}
