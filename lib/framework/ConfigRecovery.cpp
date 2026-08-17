#include <ConfigRecovery.h>

#include <esp_log.h>

namespace {
constexpr const char* TAG = "ConfigRecovery";
constexpr const char* RECOVERY_ROOT = "/.config-recovery";
constexpr const char* ACTIVE_FILE = "/.config-recovery/active";
constexpr const char* ACTIVE_TEMP = "/.config-recovery/active.tmp";
constexpr const char* LAST_ACTION_FILE = "/.config-recovery/last_action";
constexpr uint32_t SCAN_INTERVAL_MS = 60000;
constexpr uint32_t CONFIRMATION_MS = 10 * 60 * 1000;

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
bool recoverySoundHealthy = false;
bool forceRestoreRequested = false;
bool forceRestorePerformed = false;
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
  tree.xorHash ^= fileHash;
  tree.sumHash += fileHash;
  ++tree.fileCount;
}

void fingerprintTree(Fingerprint& result, const String& physicalRoot, const String& logicalRoot) {
  File root = recoveryFs->open(physicalRoot);
  if (!root) return;
  if (!root.isDirectory()) {
    addFileFingerprint(result, logicalRoot, root);
    root.close();
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
  fingerprintTree(result, base + "/livescripts", "/livescripts");
  return result;
}

Fingerprint fingerprintCurrent() {
  Fingerprint result;
  fingerprintTree(result, "/.config", "/config");
  fingerprintTree(result, "/livescripts", "/livescripts");
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

bool copyTree(const String& source, const String& destination) {
  File root = recoveryFs->open(source);
  if (!root) return true;
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

    File input = recoveryFs->open(sourcePath, "r");
    File output = recoveryFs->open(destinationPath, "w");
    if (!input || !output) {
      input.close();
      output.close();
      root.close();
      return false;
    }
    uint8_t buffer[512];
    size_t count;
    while ((count = input.read(buffer, sizeof(buffer))) > 0) {
      if (output.write(buffer, count) != count) {
        input.close();
        output.close();
        root.close();
        return false;
      }
      delay(0);
    }
    input.close();
    output.close();
  }
  root.close();
  return true;
}

String slotRoot(int slot) { return String(RECOVERY_ROOT) + "/slot" + slot; }

int readActiveSlot() {
  File file = recoveryFs->open(ACTIVE_FILE, "r");
  if (!file) return -1;
  int value = file.read();
  file.close();
  if (value != '0' && value != '1') return -1;
  int slot = value - '0';
  return recoveryFs->exists(slotRoot(slot)) ? slot : -1;
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

bool restoreSlot(int slot) {
  String source = slotRoot(slot);
  if (!removeTree("/.config") || !removeTree("/livescripts")) return false;
  if (!copyTree(source + "/config", "/.config") || !copyTree(source + "/livescripts", "/livescripts")) return false;
  return fingerprintCurrent() == fingerprintWorkingSet(source);
}

bool promoteCurrent() {
  int target = activeSlot == 0 ? 1 : 0;
  String targetRoot = slotRoot(target);
  if (!removeTree(targetRoot)) return false;
  if (!recoveryFs->exists(RECOVERY_ROOT) && !recoveryFs->mkdir(RECOVERY_ROOT)) return false;
  if (!recoveryFs->mkdir(targetRoot)) return false;
  if (!copyTree("/.config", targetRoot + "/config") || !copyTree("/livescripts", targetRoot + "/livescripts")) return false;

  Fingerprint afterCopy = fingerprintCurrent();
  Fingerprint staged = fingerprintWorkingSet(targetRoot);
  if (afterCopy != currentFingerprint || staged != afterCopy || !writeActiveSlot(target)) return false;

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

void ConfigRecovery::begin(FS* fs, esp_reset_reason_t resetReason) {
  recoveryFs = fs;
  readLastAction();
  activeSlot = readActiveSlot();
  recoveryAvailable = activeSlot >= 0;
  currentFingerprint = fingerprintCurrent();
  confirmedFingerprint = recoveryAvailable ? fingerprintWorkingSet(slotRoot(activeSlot)) : Fingerprint{};
  if (recoveryAvailable && !confirmedFingerprint.valid) {
    activeSlot = -1;
    recoveryAvailable = false;
  }

  if (forceRestoreRequested && recoveryAvailable) {
    ESP_LOGW(TAG, "Recovery button requested confirmed configuration slot %d", activeSlot);
    if (restoreSlot(activeSlot)) {
      setLastAction("restored");
      currentFingerprint = confirmedFingerprint;
      recoveryPending = false;
      forceRestorePerformed = true;
      return;
    }
    ESP_LOGE(TAG, "Button-requested configuration restore failed; continuing in safe mode");
  }

  if (recoveryAvailable && currentFingerprint.valid && isFailureReset(resetReason) && currentFingerprint != confirmedFingerprint) {
    ESP_LOGW(TAG, "Unconfirmed configuration failed; restoring slot %d", activeSlot);
    if (restoreSlot(activeSlot)) {
      setLastAction("restored");
      delay(100);
      ESP.restart();
    }
    ESP_LOGE(TAG, "Configuration restore failed; continuing in safe mode");
  }

  recoveryPending = !recoveryAvailable || currentFingerprint != confirmedFingerprint;
  candidateSince = millis();
  lastScan = millis();
}

void ConfigRecovery::loop(bool healthy) {
  uint32_t now = millis();
  if ((uint32_t)(now - lastScan) < SCAN_INTERVAL_MS) return;
  lastScan = now;

  Fingerprint scanned = fingerprintCurrent();
  if (!scanned.valid) return;
  if (scanned != currentFingerprint) {
    currentFingerprint = scanned;
    candidateSince = now;
  }

  recoveryPending = !recoveryAvailable || currentFingerprint != confirmedFingerprint;
  if (recoveryPending && healthy && (!recoverySoundRequired || recoverySoundHealthy) && (uint32_t)(now - candidateSince) >= CONFIRMATION_MS) {
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
  if (!required) recoverySoundHealthy = true;
#endif
}

void ConfigRecovery::reportSoundHealthy(bool healthy) { recoverySoundHealthy = healthy; }

bool ConfigRecovery::available() { return recoveryAvailable; }
bool ConfigRecovery::pending() { return recoveryPending; }

uint32_t ConfigRecovery::secondsUntilConfirmation() {
  if (!recoveryPending) return 0;
  uint32_t elapsed = millis() - candidateSince;
  return elapsed >= CONFIRMATION_MS ? 0 : (CONFIRMATION_MS - elapsed + 999) / 1000;
}

const char* ConfigRecovery::lastAction() { return recoveryLastAction; }
bool ConfigRecovery::soundHealthy() { return !recoverySoundRequired || recoverySoundHealthy; }
bool ConfigRecovery::restoredThisBoot() { return forceRestorePerformed; }
