#include "doctest.h"

#include <cstdlib>
#include <string>

#include "MoonBase/GoldenConfig.h"

namespace {
struct TempTree {
  char path[40] = "/tmp/moonlight-recovery-XXXXXX";
  bool created = false;

  TempTree() : created(::mkdtemp(path) != nullptr) {}
  ~TempTree() {
    if (created) goldenRemovePath(path);
  }

  std::string child(const char* name) const { return std::string(path) + "/" + name; }
};
}

TEST_CASE("golden boot repair restores an interrupted live rollback") {
  TempTree temp;
  REQUIRE(temp.created);
  const std::string golden = temp.child("golden");
  const std::string goldenRollback = temp.child("golden-rollback");
  const std::string live = temp.child("live");
  const std::string liveRollback = temp.child("live-rollback");
  REQUIRE(::mkdir(liveRollback.c_str(), 0777) == 0);

  CHECK(goldenRecoverInterruptedSwaps(golden.c_str(), goldenRollback.c_str(), live.c_str(), liveRollback.c_str()));
  CHECK(goldenPathExists(live.c_str()));
  CHECK_FALSE(goldenPathExists(liveRollback.c_str()));
}

TEST_CASE("golden boot repair reports rollback failure and leaves startup target absent") {
  TempTree temp;
  REQUIRE(temp.created);
  const std::string golden = temp.child("golden");
  const std::string goldenRollback = temp.child("golden-rollback");
  const std::string live = temp.child("missing-parent/live");
  const std::string liveRollback = temp.child("live-rollback");
  REQUIRE(::mkdir(liveRollback.c_str(), 0777) == 0);

  CHECK_FALSE(goldenRecoverInterruptedSwaps(golden.c_str(), goldenRollback.c_str(), live.c_str(), liveRollback.c_str()));
  CHECK_FALSE(goldenPathExists(live.c_str()));
  CHECK(goldenPathExists(liveRollback.c_str()));
}

TEST_CASE("golden boot repair never restores golden configuration implicitly") {
  TempTree temp;
  REQUIRE(temp.created);
  const std::string golden = temp.child("golden");
  const std::string goldenRollback = temp.child("golden-rollback");
  const std::string live = temp.child("live");
  const std::string liveRollback = temp.child("live-rollback");
  REQUIRE(::mkdir(golden.c_str(), 0777) == 0);

  CHECK(goldenRecoverInterruptedSwaps(golden.c_str(), goldenRollback.c_str(), live.c_str(), liveRollback.c_str()));
  CHECK_FALSE(goldenPathExists(live.c_str()));
}

TEST_CASE("golden tree validation rejects same-size file corruption") {
  TempTree temp;
  REQUIRE(temp.created);
  const std::string source = temp.child("source");
  const std::string target = temp.child("target");
  REQUIRE(::mkdir(source.c_str(), 0777) == 0);
  REQUIRE(::mkdir(target.c_str(), 0777) == 0);

  const int sourceFd = ::open((source + "/config.json").c_str(), O_WRONLY | O_CREAT, 0666);
  REQUIRE(sourceFd >= 0);
  REQUIRE(::write(sourceFd, "good", 4) == 4);
  REQUIRE(::close(sourceFd) == 0);

  const int targetFd = ::open((target + "/config.json").c_str(), O_WRONLY | O_CREAT, 0666);
  REQUIRE(targetFd >= 0);
  REQUIRE(::write(targetFd, "evil", 4) == 4);
  REQUIRE(::close(targetFd) == 0);

  CHECK_FALSE(goldenTreesMatch(source.c_str(), target.c_str()));
}

TEST_CASE("golden restore disables LiveScripts with an atomic empty tree") {
  TempTree temp;
  REQUIRE(temp.created);
  const std::string scripts = temp.child("livescripts");
  const std::string stage = temp.child("stage");
  const std::string rollback = temp.child("rollback");
  const std::string badScript = scripts + "/bad.sc";
  REQUIRE(::mkdir(scripts.c_str(), 0777) == 0);
  const int fd = ::open(badScript.c_str(), O_WRONLY | O_CREAT, 0666);
  REQUIRE(fd >= 0);
  REQUIRE(::close(fd) == 0);

  CHECK(goldenReplaceTreeWithEmpty(scripts.c_str(), stage.c_str(), rollback.c_str()));
  CHECK(goldenPathExists(scripts.c_str()));
  CHECK_FALSE(goldenPathExists(badScript.c_str()));
  CHECK_FALSE(goldenPathExists(stage.c_str()));
  CHECK_FALSE(goldenPathExists(rollback.c_str()));
}

TEST_CASE("golden restore preserves LiveScripts when empty-tree staging fails") {
  TempTree temp;
  REQUIRE(temp.created);
  const std::string scripts = temp.child("livescripts");
  const std::string stage = temp.child("missing-parent/stage");
  const std::string rollback = temp.child("rollback");
  const std::string badScript = scripts + "/bad.sc";
  REQUIRE(::mkdir(scripts.c_str(), 0777) == 0);
  const int fd = ::open(badScript.c_str(), O_WRONLY | O_CREAT, 0666);
  REQUIRE(fd >= 0);
  REQUIRE(::close(fd) == 0);

  CHECK_FALSE(goldenReplaceTreeWithEmpty(scripts.c_str(), stage.c_str(), rollback.c_str()));
  CHECK(goldenPathExists(badScript.c_str()));
  CHECK_FALSE(goldenPathExists(rollback.c_str()));
}
