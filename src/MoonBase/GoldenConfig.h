/**
    Golden configuration snapshot: full /.config tree copy for field restore.
**/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

inline constexpr const char* GOLDEN_CONFIG_VFS_ROOT = "/littlefs/.config-golden";
inline constexpr const char* LIVE_CONFIG_VFS_ROOT = "/littlefs/.config";
inline constexpr const char* GOLDEN_CONFIG_LOGICAL = "/.config-golden";
inline constexpr const char* LIVE_CONFIG_LOGICAL = "/.config";
inline constexpr size_t GOLDEN_FD_CHUNK = 512;
inline constexpr uint32_t GOLDEN_MIN_INTERNAL_BYTES = 8192;

inline bool isProtectedGoldenPath(const char* path) {
  if (!path) return false;
  constexpr char protectedSegment[] = ".config-golden";
  constexpr size_t protectedLength = sizeof(protectedSegment) - 1;
  while (*path) {
    while (*path == '/') ++path;
    const char* end = strchr(path, '/');
    size_t length = end ? static_cast<size_t>(end - path) : strlen(path);
    if (length == protectedLength && strncmp(path, protectedSegment, protectedLength) == 0) return true;
    if (!end) break;
    path = end + 1;
  }
  return false;
}

#ifdef ARDUINO

#include <dirent.h>
#include <cerrno>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "MoonBase/utilities/PlatformFunctions.h"

inline bool goldenDmaReady() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= GOLDEN_MIN_INTERNAL_BYTES;
}

inline bool goldenVfsPathJoin(char* out, size_t outSize, const char* root, const char* relative) {
  if (!out || outSize == 0 || !root || !relative) return false;
  if (relative[0] == '\0') {
    snprintf(out, outSize, "%s", root);
    return true;
  }
  snprintf(out, outSize, "%s/%s", root, relative);
  return true;
}

inline bool goldenCopyFileFd(const char* srcPath, const char* dstPath) {
  const int srcFd = ::open(srcPath, O_RDONLY);
  if (srcFd < 0) return false;
  const int dstFd = ::open(dstPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (dstFd < 0) {
    ::close(srcFd);
    return false;
  }
  uint8_t buffer[GOLDEN_FD_CHUNK];
  bool ok = true;
  while (true) {
    const ssize_t n = ::read(srcFd, buffer, sizeof(buffer));
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
      const ssize_t wrote = ::write(dstFd, buffer + off, static_cast<size_t>(n) - off);
      if (wrote <= 0) {
        ok = false;
        break;
      }
      off += static_cast<size_t>(wrote);
    }
    if (!ok) break;
  }
  if (ok) ::fsync(dstFd);
  ::close(srcFd);
  ::close(dstFd);
  if (!ok) ::unlink(dstPath);
  return ok;
}

inline bool goldenRemoveTree(const char* vfsRoot) {
  DIR* dir = ::opendir(vfsRoot);
  if (!dir) return ::unlink(vfsRoot) == 0 || errno == ENOENT;

  bool ok = true;
  struct dirent* entry;
  char childPath[192];
  while ((entry = ::readdir(dir)) != nullptr) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    snprintf(childPath, sizeof(childPath), "%s/%s", vfsRoot, entry->d_name);
    struct stat st;
    if (::stat(childPath, &st) != 0) {
      ok = false;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!goldenRemoveTree(childPath)) ok = false;
      if (::rmdir(childPath) != 0) ok = false;
    } else if (::unlink(childPath) != 0) {
      ok = false;
    }
  }
  ::closedir(dir);
  return ok;
}

inline bool goldenCopyTree(const char* srcRoot, const char* dstRoot) {
  DIR* dir = ::opendir(srcRoot);
  if (!dir) return false;

  bool ok = true;
  struct dirent* entry;
  char srcChild[192];
  char dstChild[192];
  while ((entry = ::readdir(dir)) != nullptr) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    snprintf(srcChild, sizeof(srcChild), "%s/%s", srcRoot, entry->d_name);
    snprintf(dstChild, sizeof(dstChild), "%s/%s", dstRoot, entry->d_name);
    struct stat st;
    if (::stat(srcChild, &st) != 0) {
      ok = false;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      ::mkdir(dstChild, 0777);
      if (!goldenCopyTree(srcChild, dstChild)) ok = false;
    } else if (!goldenCopyFileFd(srcChild, dstChild)) {
      ok = false;
    }
  }
  ::closedir(dir);
  return ok;
}

inline bool goldenConfigPresent() {
  DIR* dir = ::opendir(GOLDEN_CONFIG_VFS_ROOT);
  if (!dir) return false;
  bool hasEntry = false;
  struct dirent* entry;
  while ((entry = ::readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
      hasEntry = true;
      break;
    }
  }
  ::closedir(dir);
  return hasEntry;
}

inline bool goldenSaveSnapshot() {
  if (!goldenDmaReady()) return false;
  goldenRemoveTree(GOLDEN_CONFIG_VFS_ROOT);
  ::mkdir(GOLDEN_CONFIG_VFS_ROOT, 0777);
  return goldenCopyTree(LIVE_CONFIG_VFS_ROOT, GOLDEN_CONFIG_VFS_ROOT);
}

inline bool goldenRestoreSnapshot() {
  if (!goldenDmaReady() || !goldenConfigPresent()) return false;
  goldenRemoveTree(LIVE_CONFIG_VFS_ROOT);
  ::mkdir(LIVE_CONFIG_VFS_ROOT, 0777);
  return goldenCopyTree(GOLDEN_CONFIG_VFS_ROOT, LIVE_CONFIG_VFS_ROOT);
}

#endif
