/**
    Golden configuration snapshot: full /.config tree copy for field restore.
**/

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

inline constexpr const char* GOLDEN_CONFIG_VFS_ROOT = "/littlefs/.config-golden";
inline constexpr const char* LIVE_CONFIG_VFS_ROOT = "/littlefs/.config";
inline constexpr const char* GOLDEN_CONFIG_STAGE_VFS_ROOT = "/littlefs/.config-golden-stage";
inline constexpr const char* GOLDEN_CONFIG_ROLLBACK_VFS_ROOT = "/littlefs/.config-golden-rollback";
inline constexpr const char* LIVE_CONFIG_STAGE_VFS_ROOT = "/littlefs/.config-restore-stage";
inline constexpr const char* LIVE_CONFIG_ROLLBACK_VFS_ROOT = "/littlefs/.config-restore-rollback";
inline constexpr const char* LIVE_SCRIPTS_VFS_ROOT = "/littlefs/livescripts";
inline constexpr const char* LIVE_SCRIPTS_STAGE_VFS_ROOT = "/littlefs/.livescripts-restore-stage";
inline constexpr const char* LIVE_SCRIPTS_ROLLBACK_VFS_ROOT = "/littlefs/.livescripts-restore-rollback";
inline constexpr const char* GOLDEN_CONFIG_LOGICAL = "/.config-golden";
inline constexpr const char* LIVE_CONFIG_LOGICAL = "/.config";
inline constexpr size_t GOLDEN_FD_CHUNK = 512;
inline constexpr uint32_t GOLDEN_MIN_INTERNAL_BYTES = 8192;

// Golden snapshots cover the persisted /.config tree. Restore atomically replaces /livescripts
// with an empty tree first so stale executable scripts cannot survive; re-uploading is explicit.

inline bool isGoldenInternalSegment(const char* segment, size_t length) {
  constexpr const char* protectedSegments[] = {
      ".config-golden",
      ".config-golden-stage",
      ".config-golden-rollback",
      ".config-restore-stage",
      ".config-restore-rollback",
      ".livescripts-restore-stage",
      ".livescripts-restore-rollback",
  };
  for (const char* protectedSegment : protectedSegments) {
    const size_t protectedLength = strlen(protectedSegment);
    if (length == protectedLength && strncmp(segment, protectedSegment, protectedLength) == 0) return true;
  }
  return false;
}

inline bool isProtectedGoldenPath(const char* path) {
  if (!path) return false;
  while (*path) {
    while (*path == '/') ++path;
    const char* end = strchr(path, '/');
    size_t length = end ? static_cast<size_t>(end - path) : strlen(path);
    if (isGoldenInternalSegment(path, length)) return true;
    if (!end) break;
    path = end + 1;
  }
  return false;
}

#ifdef ARDUINO

#include <esp_heap_caps.h>

#include "MoonBase/utilities/PlatformFunctions.h"

inline bool goldenDmaReady() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= GOLDEN_MIN_INTERNAL_BYTES;
}

#endif

inline bool goldenVfsPathJoin(char* out, size_t outSize, const char* root, const char* relative) {
  if (!out || outSize == 0 || !root || !relative) return false;
  int written;
  if (relative[0] == '\0') {
    written = snprintf(out, outSize, "%s", root);
  } else {
    written = snprintf(out, outSize, "%s/%s", root, relative);
  }
  return written >= 0 && static_cast<size_t>(written) < outSize;
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
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    if (n == 0) break;
    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
      const ssize_t wrote = ::write(dstFd, buffer + off, static_cast<size_t>(n) - off);
      if (wrote <= 0) {
        if (wrote < 0 && errno == EINTR) continue;
        ok = false;
        break;
      }
      off += static_cast<size_t>(wrote);
    }
    if (!ok) break;
  }
  if (ok && ::fsync(dstFd) != 0) ok = false;
  if (::close(srcFd) != 0) ok = false;
  if (::close(dstFd) != 0) ok = false;
  if (!ok) ::unlink(dstPath);
  return ok;
}

inline ssize_t goldenReadChunk(int fd, uint8_t* buffer, size_t size) {
  size_t off = 0;
  while (off < size) {
    const ssize_t n = ::read(fd, buffer + off, size - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) break;
    off += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(off);
}

inline bool goldenFilesMatch(const char* srcPath, const char* dstPath) {
  const int srcFd = ::open(srcPath, O_RDONLY);
  if (srcFd < 0) return false;
  const int dstFd = ::open(dstPath, O_RDONLY);
  if (dstFd < 0) {
    ::close(srcFd);
    return false;
  }

  uint8_t srcBuffer[GOLDEN_FD_CHUNK];
  uint8_t dstBuffer[GOLDEN_FD_CHUNK];
  bool ok = true;
  while (true) {
    const ssize_t srcRead = goldenReadChunk(srcFd, srcBuffer, sizeof(srcBuffer));
    const ssize_t dstRead = goldenReadChunk(dstFd, dstBuffer, sizeof(dstBuffer));
    if (srcRead < 0 || srcRead != dstRead ||
        (srcRead > 0 && memcmp(srcBuffer, dstBuffer, static_cast<size_t>(srcRead)) != 0)) {
      ok = false;
      break;
    }
    if (srcRead == 0) break;
  }
  if (::close(srcFd) != 0) ok = false;
  if (::close(dstFd) != 0) ok = false;
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
    if (!goldenVfsPathJoin(childPath, sizeof(childPath), vfsRoot, entry->d_name)) {
      ok = false;
      continue;
    }
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

inline bool goldenRemovePath(const char* vfsRoot) {
  if (!goldenRemoveTree(vfsRoot)) return false;
  return ::rmdir(vfsRoot) == 0 || errno == ENOENT;
}

inline bool goldenPathExists(const char* path) {
  struct stat st;
  return path && ::stat(path, &st) == 0;
}

inline bool goldenRecoverInterruptedSwap(const char* targetRoot, const char* rollbackRoot) {
  const bool targetExists = goldenPathExists(targetRoot);
  const bool rollbackExists = goldenPathExists(rollbackRoot);
  if (!rollbackExists) return true;
  if (targetExists) return goldenRemovePath(rollbackRoot);
  return ::rename(rollbackRoot, targetRoot) == 0 && goldenPathExists(targetRoot);
}

inline bool goldenRecoverInterruptedSwaps(const char* goldenRoot, const char* goldenRollbackRoot,
                                          const char* liveRoot, const char* liveRollbackRoot) {
  const bool goldenOk = goldenRecoverInterruptedSwap(goldenRoot, goldenRollbackRoot);
  const bool liveOk = goldenRecoverInterruptedSwap(liveRoot, liveRollbackRoot);
  return goldenOk && liveOk;
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
    if (!goldenVfsPathJoin(srcChild, sizeof(srcChild), srcRoot, entry->d_name) ||
        !goldenVfsPathJoin(dstChild, sizeof(dstChild), dstRoot, entry->d_name)) {
      ok = false;
      continue;
    }
    struct stat st;
    if (::stat(srcChild, &st) != 0) {
      ok = false;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (::mkdir(dstChild, 0777) != 0 || !goldenCopyTree(srcChild, dstChild)) ok = false;
    } else if (!goldenCopyFileFd(srcChild, dstChild)) {
      ok = false;
    }
  }
  ::closedir(dir);
  return ok;
}

inline bool goldenTreesMatch(const char* srcRoot, const char* dstRoot) {
  DIR* srcDir = ::opendir(srcRoot);
  DIR* dstDir = ::opendir(dstRoot);
  if (!srcDir || !dstDir) {
    if (srcDir) ::closedir(srcDir);
    if (dstDir) ::closedir(dstDir);
    return false;
  }

  bool ok = true;
  size_t srcEntries = 0;
  size_t dstEntries = 0;
  struct dirent* entry;
  char srcChild[192];
  char dstChild[192];
  while ((entry = ::readdir(srcDir)) != nullptr) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    ++srcEntries;
    if (!goldenVfsPathJoin(srcChild, sizeof(srcChild), srcRoot, entry->d_name) ||
        !goldenVfsPathJoin(dstChild, sizeof(dstChild), dstRoot, entry->d_name)) {
      ok = false;
      continue;
    }

    struct stat srcStat;
    struct stat dstStat;
    if (::stat(srcChild, &srcStat) != 0 || ::stat(dstChild, &dstStat) != 0) {
      ok = false;
      continue;
    }
    if (S_ISDIR(srcStat.st_mode) != S_ISDIR(dstStat.st_mode)) {
      ok = false;
    } else if (S_ISDIR(srcStat.st_mode)) {
      if (!goldenTreesMatch(srcChild, dstChild)) ok = false;
    } else if (srcStat.st_size != dstStat.st_size || !goldenFilesMatch(srcChild, dstChild)) {
      ok = false;
    }
  }
  while ((entry = ::readdir(dstDir)) != nullptr) {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++dstEntries;
  }
  ::closedir(srcDir);
  ::closedir(dstDir);
  return ok && srcEntries == dstEntries;
}

inline bool goldenPrepareStagedTree(const char* srcRoot, const char* stageRoot) {
  if (!goldenRemovePath(stageRoot) || ::mkdir(stageRoot, 0777) != 0) return false;
  if (goldenCopyTree(srcRoot, stageRoot) && goldenTreesMatch(srcRoot, stageRoot)) return true;
  goldenRemovePath(stageRoot);
  return false;
}

inline bool goldenSwapStagedTree(const char* stageRoot, const char* targetRoot, const char* rollbackRoot) {
  const bool targetExists = goldenPathExists(targetRoot);
  const bool rollbackExists = goldenPathExists(rollbackRoot);
  if (rollbackExists) {
    if (!targetExists || !goldenRemovePath(rollbackRoot)) return false;
  }
  if (targetExists && ::rename(targetRoot, rollbackRoot) != 0) return false;
  if (::rename(stageRoot, targetRoot) != 0) {
    if (targetExists && (::rename(rollbackRoot, targetRoot) != 0 || !goldenPathExists(targetRoot))) return false;
    return false;
  }
  if (targetExists && !goldenRemovePath(rollbackRoot)) return false;
  return true;
}

inline bool goldenStageAndReplaceTree(const char* srcRoot, const char* targetRoot, const char* stageRoot,
                                      const char* rollbackRoot) {
  if (!goldenPrepareStagedTree(srcRoot, stageRoot)) return false;
  if (goldenSwapStagedTree(stageRoot, targetRoot, rollbackRoot)) return true;
  goldenRemovePath(stageRoot);
  return false;
}

inline bool goldenReplaceTreeWithEmpty(const char* targetRoot, const char* stageRoot, const char* rollbackRoot) {
  if (!goldenRemovePath(stageRoot) || ::mkdir(stageRoot, 0777) != 0) return false;
  if (goldenSwapStagedTree(stageRoot, targetRoot, rollbackRoot)) return true;
  goldenRemovePath(stageRoot);
  return false;
}

#ifdef ARDUINO

inline bool goldenRepairInterruptedSwaps() {
  const bool configOk = goldenRecoverInterruptedSwaps(GOLDEN_CONFIG_VFS_ROOT, GOLDEN_CONFIG_ROLLBACK_VFS_ROOT,
                                                      LIVE_CONFIG_VFS_ROOT, LIVE_CONFIG_ROLLBACK_VFS_ROOT);
  const bool scriptsOk =
      goldenRecoverInterruptedSwap(LIVE_SCRIPTS_VFS_ROOT, LIVE_SCRIPTS_ROLLBACK_VFS_ROOT);
  return configOk && scriptsOk;
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
  if (!goldenRecoverInterruptedSwap(GOLDEN_CONFIG_VFS_ROOT, GOLDEN_CONFIG_ROLLBACK_VFS_ROOT) ||
      !goldenDmaReady())
    return false;
  return goldenStageAndReplaceTree(LIVE_CONFIG_VFS_ROOT, GOLDEN_CONFIG_VFS_ROOT, GOLDEN_CONFIG_STAGE_VFS_ROOT,
                                   GOLDEN_CONFIG_ROLLBACK_VFS_ROOT);
}

inline bool goldenRestoreSnapshot() {
  if (!goldenRepairInterruptedSwaps() || !goldenDmaReady() || !goldenConfigPresent())
    return false;
  if (!goldenPrepareStagedTree(GOLDEN_CONFIG_VFS_ROOT, LIVE_CONFIG_STAGE_VFS_ROOT)) return false;
  if (!goldenReplaceTreeWithEmpty(LIVE_SCRIPTS_VFS_ROOT, LIVE_SCRIPTS_STAGE_VFS_ROOT,
                                  LIVE_SCRIPTS_ROLLBACK_VFS_ROOT)) {
    goldenRemovePath(LIVE_CONFIG_STAGE_VFS_ROOT);
    return false;
  }
  if (goldenSwapStagedTree(LIVE_CONFIG_STAGE_VFS_ROOT, LIVE_CONFIG_VFS_ROOT,
                           LIVE_CONFIG_ROLLBACK_VFS_ROOT))
    return true;
  goldenRemovePath(LIVE_CONFIG_STAGE_VFS_ROOT);
  return false;
}

inline bool goldenFactoryReset() {
  bool ok = true;
  const char* paths[] = {
      GOLDEN_CONFIG_STAGE_VFS_ROOT,     GOLDEN_CONFIG_ROLLBACK_VFS_ROOT,
      GOLDEN_CONFIG_VFS_ROOT,           LIVE_CONFIG_STAGE_VFS_ROOT,
      LIVE_CONFIG_ROLLBACK_VFS_ROOT,    LIVE_CONFIG_VFS_ROOT,
      LIVE_SCRIPTS_STAGE_VFS_ROOT,      LIVE_SCRIPTS_ROLLBACK_VFS_ROOT,
      LIVE_SCRIPTS_VFS_ROOT,
  };
  for (const char* path : paths) ok = goldenRemovePath(path) && ok;
  return ok;
}

#endif
