#include <ConfigRecovery.h>
#include <RecoveryPolicy.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr const char* TAG = "ConfigRecovery";
constexpr const char* RECOVERY_ROOT = "/.config-recovery";
constexpr const char* ACTIVE_FILE = "/.config-recovery/active";
constexpr const char* ACTIVE_TEMP = "/.config-recovery/active.tmp";
constexpr const char* LAST_ACTION_FILE = "/.config-recovery/last_action";
constexpr const char* RESTORE_MARKER_FILE = "/.config-recovery/restore_in_progress";
constexpr const char* SLOT_MANIFEST = "/manifest";
constexpr const char* SLOT_MANIFEST_TEMP = "/manifest.tmp";
constexpr const char* SLOT_MANIFEST_V1 = "MoonLightConfigRecovery/1";
constexpr const char* SLOT_MANIFEST_V2 = "MoonLightConfigRecovery/2";
constexpr uint32_t SCAN_INTERVAL_MS = 60000;
constexpr uint32_t CONFIRMATION_MS = 3 * 60 * 1000;

struct Fingerprint {
  uint64_t xorHash = 0;
  uint64_t sumHash = 0;
  uint32_t fileCount = 0;
  bool valid = true;

  bool operator==(const Fingerprint& other) const {
    return valid && other.valid && xorHash == other.xorHash && sumHash == other.sumHash && fileCount == other.fileCount;
  }
  bool operator!=(const Fingerprint& other) const { return !(*this == other); }
};

FS* recoveryFs = nullptr;
int activeSlot = -1;
bool recoveryAvailable = false;
bool recoveryPending = false;
#ifdef CONFIG_RECOVERY_REQUIRE_SOUND
bool recoverySoundRequired = true;
#else
bool recoverySoundRequired = false;
#endif
RecoverySoundState recoverySoundState;
bool forceRestoreRequested = false;
bool forceRestorePerformed = false;
bool candidateHealthy = false;
uint32_t candidateSince = 0;
uint32_t lastScan = 0;
Fingerprint confirmedFingerprint;
Fingerprint currentFingerprint;
const char* recoveryLastAction = "none";

void setLastAction(const char* action) {
  recoveryLastAction = action;
  File file = recoveryFs->open(LAST_ACTION_FILE, "w");
  if (file) {
    file.print(action);
    file.close();
  }
}

void readLastAction() {
  File file = recoveryFs->open(LAST_ACTION_FILE, "r");
  if (!file) return;
  String action = file.readString();
  file.close();
  if (action == "restored") recoveryLastAction = "restored";
  if (action == "confirmed") recoveryLastAction = "confirmed";
}

uint64_t fnv1a(uint64_t hash, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

void addFileFingerprint(Fingerprint& tree, const String& logicalPath, File& file) {
  uint64_t fileHash = fnv1a(1469598103934665603ULL, reinterpret_cast<const uint8_t*>(logicalPath.c_str()), logicalPath.length());
  uint8_t buffer[512];
  size_t count;
  while ((count = file.read(buffer, sizeof(buffer))) > 0) {
    fileHash = fnv1a(fileHash, buffer, count);
    delay(0);
  }
  if (file.position() != file.size()) {
    tree.valid = false;
    return;
  }
  tree.xorHash ^= fileHash;
  tree.sumHash += fileHash;
  ++tree.fileCount;
}

void fingerprintTree(Fingerprint& result, const String& physicalRoot, const String& logicalRoot) {
  File root = recoveryFs->open(physicalRoot);
  if (!root || !root.isDirectory()) {
    root.close();
    result.valid = false;
    return;
  }

  File entry;
  while ((entry = root.openNextFile())) {
    String path = entry.path();
    String relative = path.substring(physicalRoot.length());
    String logicalPath = logicalRoot + relative;
    bool directory = entry.isDirectory();
    entry.close();
    if (directory) {
      fingerprintTree(result, path, logicalPath);
    } else {
      File file = recoveryFs->open(path, "r");
      if (!file) {
        result.valid = false;
        break;
      }
      addFileFingerprint(result, logicalPath, file);
      file.close();
    }
  }
  root.close();
}

Fingerprint fingerprintWorkingSet(const String& base = "") {
  Fingerprint result;
  fingerprintTree(result, base + "/config", "/config");
  File scripts = recoveryFs->open(base + "/livescripts");
  if (!scripts || !scripts.isDirectory()) {
    scripts.close();
    result.valid = false;
    return result;
  }
  File entry;
  while ((entry = scripts.openNextFile())) {
    String path = entry.path();
    String name = path.substring(path.lastIndexOf('/') + 1);
    bool validScript = !entry.isDirectory() && name.endsWith(".sc");
    entry.close();
    if (!validScript) {
      result.valid = false;
      break;
    }
    File file = recoveryFs->open(path, "r");
    if (!file) {
      result.valid = false;
      break;
    }
    addFileFingerprint(result, String("/livescripts/") + name, file);
    file.close();
  }
  scripts.close();
  return result;
}

Fingerprint fingerprintCurrent() {
  Fingerprint result;
  fingerprintTree(result, "/.config", "/config");
  File root = recoveryFs->open("/");
  if (!root || !root.isDirectory()) {
    root.close();
    result.valid = false;
    return result;
  }
  File entry;
  while ((entry = root.openNextFile())) {
    String path = entry.path();
    String name = path.substring(path.lastIndexOf('/') + 1);
    bool liveScript = !entry.isDirectory() && name.endsWith(".sc");
    entry.close();
    if (!liveScript) continue;
    File file = recoveryFs->open(path, "r");
    if (!file) {
      result.valid = false;
      break;
    }
    addFileFingerprint(result, String("/livescripts/") + name, file);
    file.close();
  }
  root.close();
  return result;
}

bool removeTree(const String& path) {
  File root = recoveryFs->open(path);
  if (!root) return true;
  if (!root.isDirectory()) {
    root.close();
    return recoveryFs->remove(path);
  }

  File entry;
  while ((entry = root.openNextFile())) {
    String child = entry.path();
    entry.close();
    if (!removeTree(child)) {
      root.close();
      return false;
    }
  }
  root.close();
  return recoveryFs->rmdir(path);
}

bool copyFile(const String& source, const String& destination) {
  File input = recoveryFs->open(source, "r");
  File output = recoveryFs->open(destination, "w");
  if (!input || !output) {
    input.close();
    output.close();
    return false;
  }
  uint8_t buffer[512];
  size_t count;
  while ((count = input.read(buffer, sizeof(buffer))) > 0) {
    if (output.write(buffer, count) != count) {
      input.close();
      output.close();
      return false;
    }
    delay(0);
  }
  bool complete = input.position() == input.size();
  input.close();
  output.close();
  return complete;
}

bool copyTree(const String& source, const String& destination) {
  File root = recoveryFs->open(source);
  if (!root) return false;
  if (!root.isDirectory()) return false;
  if (!recoveryFs->exists(destination) && !recoveryFs->mkdir(destination)) {
    root.close();
    return false;
  }

  File entry;
  while ((entry = root.openNextFile())) {
    String sourcePath = entry.path();
    String destinationPath = destination + sourcePath.substring(source.length());
    bool directory = entry.isDirectory();
    entry.close();

    if (directory) {
      if (!copyTree(sourcePath, destinationPath)) {
        root.close();
        return false;
      }
      continue;
    }

    if (!copyFile(sourcePath, destinationPath)) {
      root.close();
      return false;
    }
  }
  root.close();
  return true;
}

bool copyRootLiveScripts(const String& destination) {
  if (!recoveryFs->exists(destination) && !recoveryFs->mkdir(destination)) return false;
  File root = recoveryFs->open("/");
  if (!root || !root.isDirectory()) {
    root.close();
    return false;
  }
  File entry;
  while ((entry = root.openNextFile())) {
    String sourcePath = entry.path();
    String name = sourcePath.substring(sourcePath.lastIndexOf('/') + 1);
    bool liveScript = !entry.isDirectory() && name.endsWith(".sc");
    entry.close();
    if (!liveScript) continue;
    if (!copyFile(sourcePath, destination + "/" + name)) {
      root.close();
      return false;
    }
  }
  root.close();
  return true;
}

bool removeRootLiveScripts() {
  File root = recoveryFs->open("/");
  if (!root || !root.isDirectory()) {
    root.close();
    return false;
  }
  std::vector<String> paths;
  File entry;
  while ((entry = root.openNextFile())) {
    String path = entry.path();
    String name = path.substring(path.lastIndexOf('/') + 1);
    if (!entry.isDirectory() && name.endsWith(".sc")) paths.push_back(path);
    entry.close();
  }
  root.close();
  for (const String& path : paths) {
    if (!recoveryFs->remove(path)) return false;
  }
  return true;
}

bool restoreRootLiveScripts(const String& source) {
  File scripts = recoveryFs->open(source);
  if (!scripts || !scripts.isDirectory()) {
    scripts.close();
    return false;
  }
  File entry;
  while ((entry = scripts.openNextFile())) {
    String sourcePath = entry.path();
    String name = sourcePath.substring(sourcePath.lastIndexOf('/') + 1);
    bool validScript = !entry.isDirectory() && name.endsWith(".sc");
    entry.close();
    if (!validScript) {
      scripts.close();
      return false;
    }
    if (!copyFile(sourcePath, String("/") + name)) {
      scripts.close();
      return false;
    }
  }
  scripts.close();
  return true;
}

String slotRoot(int slot) { return String(RECOVERY_ROOT) + "/slot" + slot; }

bool readableDirectory(const String& path) {
  File directory = recoveryFs->open(path, "r");
  bool readable = directory && directory.isDirectory();
  directory.close();
  return readable;
}

bool readSlotManifest(const String& root, Fingerprint& expected) {
  File manifest = recoveryFs->open(root + SLOT_MANIFEST, "r");
  if (!manifest || manifest.isDirectory()) {
    manifest.close();
    return false;
  }
  String content = manifest.readString();
  manifest.close();
  unsigned long long xorHash = 0;
  unsigned long long sumHash = 0;
  unsigned long long fileCount = 0;
  int consumed = 0;
  if (sscanf(content.c_str(), "MoonLightConfigRecovery/2\n%llx\n%llx\n%llu%n", &xorHash, &sumHash, &fileCount, &consumed) != 3 ||
      consumed != static_cast<int>(content.length()) || fileCount > UINT32_MAX) {
    return false;
  }
  expected.xorHash = static_cast<uint64_t>(xorHash);
  expected.sumHash = static_cast<uint64_t>(sumHash);
  expected.fileCount = static_cast<uint32_t>(fileCount);
  expected.valid = true;
  return true;
}

bool writeSlotManifest(const String& root, const Fingerprint& fingerprint) {
  if (!fingerprint.valid) return false;
  char content[96];
  int length = snprintf(
      content,
      sizeof(content),
      "%s\n%016llx\n%016llx\n%lu",
      SLOT_MANIFEST_V2,
      static_cast<unsigned long long>(fingerprint.xorHash),
      static_cast<unsigned long long>(fingerprint.sumHash),
      static_cast<unsigned long>(fingerprint.fileCount));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(content)) return false;
  String temporary = root + SLOT_MANIFEST_TEMP;
  recoveryFs->remove(temporary);
  File manifest = recoveryFs->open(temporary, "w");
  if (!manifest) return false;
  bool written = manifest.write(reinterpret_cast<const uint8_t*>(content), static_cast<size_t>(length)) == static_cast<size_t>(length);
  manifest.close();
  if (!written) return false;
  String destination = root + SLOT_MANIFEST;
  if (recoveryFs->exists(destination) && !recoveryFs->remove(destination)) return false;
  return recoveryFs->rename(temporary, destination);
}

bool legacyManifestEligible(const String& root) {
  File manifest = recoveryFs->open(root + SLOT_MANIFEST, "r");
  if (!manifest) return !recoveryFs->exists(root + SLOT_MANIFEST);
  if (manifest.isDirectory()) {
    manifest.close();
    return false;
  }
  String content = manifest.readString();
  manifest.close();
  return content == SLOT_MANIFEST_V1;
}

bool validateSlot(int slot) {
  String root = slotRoot(slot);
  bool configReadable = readableDirectory(root + "/config");
  bool livescriptsReadable = readableDirectory(root + "/livescripts");
  if (!recoverySlotReady(true, configReadable, livescriptsReadable)) return false;
  Fingerprint actual = fingerprintWorkingSet(root);
  Fingerprint expected;
  return actual.valid && readSlotManifest(root, expected) && actual == expected;
}

int readActiveSlotIndex() {
  File file = recoveryFs->open(ACTIVE_FILE, "r");
  if (!file) return -1;
  int value = file.read();
  file.close();
  if (value != '0' && value != '1') return -1;
  return value - '0';
}

bool upgradeLegacySlot(int slot, bool failureReset, const Fingerprint& liveFingerprint) {
  if (slot < 0) return false;
  String root = slotRoot(slot);
  bool rootsReadable = readableDirectory(root + "/config") && readableDirectory(root + "/livescripts");
  if (!legacyManifestEligible(root)) return false;
  Fingerprint slotFingerprint = rootsReadable ? fingerprintWorkingSet(root) : Fingerprint{};
  bool slotMatchesLive = slotFingerprint.valid && slotFingerprint == liveFingerprint;
  if (!recoveryMayUpgradeLegacySlot(failureReset, rootsReadable, liveFingerprint.valid, slotMatchesLive)) return false;
  if (!writeSlotManifest(root, slotFingerprint) || !validateSlot(slot)) return false;
  ESP_LOGW(TAG, "Upgraded matching legacy recovery slot %d without adopting live files", slot);
  return true;
}

bool writeActiveSlot(int slot) {
  if (!recoveryFs->exists(RECOVERY_ROOT) && !recoveryFs->mkdir(RECOVERY_ROOT)) return false;
  File file = recoveryFs->open(ACTIVE_TEMP, "w");
  if (!file) return false;
  bool written = file.print(slot) == 1;
  file.close();
  if (!written) return false;
  return recoveryFs->rename(ACTIVE_TEMP, ACTIVE_FILE);
}

bool markRestoreInProgress() {
  if (recoveryFs->exists(RESTORE_MARKER_FILE)) return true;
  File file = recoveryFs->open(RESTORE_MARKER_FILE, "w");
  if (!file) return false;
  bool written = file.print('1') == 1;
  file.close();
  return written;
}

bool restoreSlot(int slot) {
  String source = slotRoot(slot);
  if (!validateSlot(slot)) return false;
  Fingerprint sourceFingerprint = fingerprintWorkingSet(source);
  if (!sourceFingerprint.valid) return false;
  if (!markRestoreInProgress()) return false;
  if (!removeTree("/.config") || !removeRootLiveScripts()) return false;
  if (!copyTree(source + "/config", "/.config") || !restoreRootLiveScripts(source + "/livescripts")) return false;
  if (fingerprintCurrent() != sourceFingerprint) return false;
  return recoveryFs->remove(RESTORE_MARKER_FILE);
}

bool promoteCurrent() {
  int target = activeSlot == 0 ? 1 : 0;
  String targetRoot = slotRoot(target);
  if (!removeTree(targetRoot)) return false;
  if (!recoveryFs->exists(RECOVERY_ROOT) && !recoveryFs->mkdir(RECOVERY_ROOT)) return false;
  if (!recoveryFs->mkdir(targetRoot)) return false;
  if (!copyTree("/.config", targetRoot + "/config") || !copyRootLiveScripts(targetRoot + "/livescripts")) return false;

  Fingerprint afterCopy = fingerprintCurrent();
  Fingerprint staged = fingerprintWorkingSet(targetRoot);
  if (afterCopy != currentFingerprint || staged != afterCopy || !writeSlotManifest(targetRoot, staged) || !validateSlot(target) || !writeActiveSlot(target)) return false;

  activeSlot = target;
  confirmedFingerprint = staged;
  currentFingerprint = afterCopy;
  recoveryAvailable = true;
  recoveryPending = false;
  setLastAction("confirmed");
  ESP_LOGW(TAG, "Confirmed stable configuration in slot %d", activeSlot);
  return true;
}

bool isFailureReset(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT || reason == ESP_RST_CPU_LOCKUP;
}
}  // namespace

bool ConfigRecovery::begin(FS* fs, esp_reset_reason_t resetReason) {
  recoveryFs = fs;
  readLastAction();
  currentFingerprint = fingerprintCurrent();
  int configuredSlot = readActiveSlotIndex();
  activeSlot = configuredSlot >= 0 && validateSlot(configuredSlot) ? configuredSlot : -1;
  if (activeSlot < 0 && upgradeLegacySlot(configuredSlot, isFailureReset(resetReason), currentFingerprint)) activeSlot = configuredSlot;
  recoveryAvailable = activeSlot >= 0;
  bool restoreInProgress = recoveryFs->exists(RESTORE_MARKER_FILE);
  confirmedFingerprint = recoveryAvailable ? fingerprintWorkingSet(slotRoot(activeSlot)) : Fingerprint{};
  if (recoveryAvailable && !confirmedFingerprint.valid) {
    activeSlot = -1;
    recoveryAvailable = false;
  }

  if (restoreInProgress) {
    if (!recoveryAvailable) {
      ESP_LOGE(TAG, "Interrupted configuration restore has no valid active slot");
      return false;
    }
    ESP_LOGW(TAG, "Resuming interrupted configuration restore from slot %d", activeSlot);
    if (restoreSlot(activeSlot)) {
      setLastAction("restored");
      currentFingerprint = confirmedFingerprint;
      recoveryPending = false;
      forceRestorePerformed = true;
      return true;
    }
    ESP_LOGE(TAG, "Interrupted configuration restore retry failed; blocking settings initialization");
    return false;
  }

  if (forceRestoreRequested && recoveryAvailable) {
    ESP_LOGW(TAG, "Recovery button requested confirmed configuration slot %d", activeSlot);
    if (restoreSlot(activeSlot)) {
      setLastAction("restored");
      currentFingerprint = confirmedFingerprint;
      recoveryPending = false;
      forceRestorePerformed = true;
      return true;
    }
    ESP_LOGE(TAG, "Button-requested configuration restore failed; blocking settings initialization");
    return false;
  }

  if (recoveryShouldRestore(recoveryAvailable, isFailureReset(resetReason), currentFingerprint.valid, currentFingerprint == confirmedFingerprint)) {
    ESP_LOGW(TAG, "Unconfirmed configuration failed; restoring slot %d", activeSlot);
    if (restoreSlot(activeSlot)) {
      setLastAction("restored");
      delay(100);
      ESP.restart();
    }
    ESP_LOGE(TAG, "Configuration restore failed; blocking settings initialization");
    return false;
  }

  recoveryPending = !recoveryAvailable || currentFingerprint != confirmedFingerprint;
  candidateHealthy = false;
  candidateSince = millis();
  lastScan = millis();
  return true;
}

void ConfigRecovery::loop(bool healthy) {
  uint32_t now = millis();
  bool healthyForConfirmation = healthy && recoverySoundState.fresh(recoverySoundRequired, now);
  if (!healthyForConfirmation) {
    candidateHealthy = false;
    candidateSince = now;
  } else if (!candidateHealthy) {
    candidateHealthy = true;
    candidateSince = now;
  }
  // ponytail: never fopen the live tree from the SvelteKit loop; confirm the boot fingerprint only.
  recoveryPending = !recoveryAvailable || currentFingerprint != confirmedFingerprint;
  if (recoveryPending && candidateHealthy && (uint32_t)(now - candidateSince) >= CONFIRMATION_MS) {
    promoteCurrent();
  }
}

void ConfigRecovery::clear() {
  if (recoveryFs) removeTree(RECOVERY_ROOT);
  activeSlot = -1;
  recoveryAvailable = false;
  recoveryPending = true;
  recoveryLastAction = "none";
}

void ConfigRecovery::requestRestore() { forceRestoreRequested = true; }

void ConfigRecovery::requireSound(bool required) {
#ifdef CONFIG_RECOVERY_REQUIRE_SOUND
  (void)required;
  recoverySoundRequired = true;
#else
  recoverySoundRequired = required;
#endif
}

void ConfigRecovery::reportSoundHealthy(bool healthy) {
  recoverySoundState.report(healthy, millis());
}

bool ConfigRecovery::available() { return recoveryAvailable; }
bool ConfigRecovery::pending() { return recoveryPending; }

uint32_t ConfigRecovery::secondsUntilConfirmation() {
  if (!recoveryPending) return 0;
  uint32_t elapsed = millis() - candidateSince;
  return elapsed >= CONFIRMATION_MS ? 0 : (CONFIRMATION_MS - elapsed + 999) / 1000;
}

const char* ConfigRecovery::lastAction() { return recoveryLastAction; }
bool ConfigRecovery::soundHealthy() {
  return recoverySoundState.fresh(recoverySoundRequired, millis());
}
bool ConfigRecovery::restoredThisBoot() { return forceRestorePerformed; }
