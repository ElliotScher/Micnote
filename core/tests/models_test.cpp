// The model registry: the built-in manifest and the choices it makes for
// each kind of device, every rule a manifest must follow, and checking and
// verifying model files.
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "crypto/psa.hpp"
#include "models/registry.hpp"

namespace {

namespace fs = std::filesystem;
using micnote::models::Check;
using micnote::models::ClassFor;
using micnote::models::Device;
using micnote::models::DeviceClass;
using micnote::models::FileState;
using micnote::models::Kind;
using micnote::models::Registry;
using micnote::models::Verify;
using micnote::models::VerifyResult;

std::string Hex(std::span<const std::byte> b) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string s;
  for (const std::byte x : b) {
    s += kDigits.at(std::to_integer<unsigned>(x) >> 4U);
    s += kDigits.at(std::to_integer<unsigned>(x) & 0xFU);
  }
  return s;
}

std::string Id(const micnote::models::Model *m) { return m != nullptr ? m->id : "(none)"; }

// --- the built-in manifest ---------------------------------------------------

TEST(Models, BuiltinManifestParses) {
  const auto reg = Registry::Builtin();
  ASSERT_TRUE(reg) << reg.error().message;
  EXPECT_EQ(reg->version(), 1);
  ASSERT_EQ(reg->models().size(), 4U);
  const auto *turbo = reg->Find("whisper-large-v3-turbo-q5_0");
  ASSERT_NE(turbo, nullptr);
  EXPECT_EQ(turbo->file, "ggml-large-v3-turbo-q5_0.bin");
  EXPECT_EQ(turbo->bytes, 574041195U);
  EXPECT_EQ(turbo->license, "MIT");
  EXPECT_TRUE(turbo->RunsOn(DeviceClass::kDesktop));
  EXPECT_FALSE(turbo->RunsOn(DeviceClass::kMobile));
  for (const auto &m : reg->models()) {
    // Pinned to a commit, never a branch (stage 5b §4.4).
    EXPECT_NE(m.url.find("/resolve/"), std::string::npos) << m.id;
    EXPECT_EQ(m.url.find("/resolve/main/"), std::string::npos) << m.id;
  }
  EXPECT_EQ(reg->Find("nope"), nullptr);
}

// The defaults of stage 2 §4.5 and stage 5 §4.4.
TEST(Models, BuiltinDefaultsPerDevice) {
  const auto reg = Registry::Builtin().value();
  const auto asr = [&](DeviceClass cls, std::uint64_t ram) {
    return Id(reg.Default(Kind::kAsr, Device{.cls = cls, .ram_mb = ram}));
  };
  EXPECT_EQ(asr(DeviceClass::kDesktop, 16384), "whisper-large-v3-turbo-q5_0");
  EXPECT_EQ(asr(DeviceClass::kDesktop, 7680), "whisper-large-v3-turbo-q5_0") << "an 8 GB machine";
  EXPECT_EQ(asr(DeviceClass::kDesktop, 4096), "whisper-small.en-q5_1");
  EXPECT_EQ(asr(DeviceClass::kDesktop, 512), "(none)") << "too little RAM for any desktop default";
  EXPECT_EQ(asr(DeviceClass::kMobile, 7680), "whisper-small.en-q5_1");
  EXPECT_EQ(asr(DeviceClass::kMobile, 5800), "whisper-base.en-q5_1");
  EXPECT_EQ(asr(DeviceClass::kMobileSmall, 3072), "whisper-base.en-q5_1");
  for (const auto cls : {DeviceClass::kMobileSmall, DeviceClass::kMobile, DeviceClass::kDesktop}) {
    EXPECT_EQ(Id(reg.Default(Kind::kVad, Device{.cls = cls, .ram_mb = 4096})), "silero-v6.2.0");
    EXPECT_EQ(reg.Default(Kind::kLlm, Device{.cls = cls, .ram_mb = 65536}), nullptr)
        << "no LLM until stage 3";
  }
}

TEST(Models, DeviceClasses) {
  EXPECT_EQ(ClassFor(false, 1024), DeviceClass::kDesktop);
  EXPECT_EQ(ClassFor(true, 4096), DeviceClass::kMobileSmall);
  EXPECT_EQ(ClassFor(true, 5800), DeviceClass::kMobile);
  const Device here = micnote::models::ThisDevice();
  EXPECT_EQ(here.cls, DeviceClass::kDesktop);
  EXPECT_GT(here.ram_mb, 0U);
}

TEST(Models, Names) {
  using micnote::models::ParseClass;
  using micnote::models::ParseKind;
  using micnote::models::ToString;
  for (const auto k : {Kind::kAsr, Kind::kVad, Kind::kLlm}) {
    EXPECT_EQ(ParseKind(ToString(k)), k);
  }
  for (const auto c : {DeviceClass::kMobileSmall, DeviceClass::kMobile, DeviceClass::kDesktop}) {
    EXPECT_EQ(ParseClass(ToString(c)), c);
  }
  EXPECT_FALSE(ParseKind("tts"));
  EXPECT_FALSE(ParseClass("watch"));
  EXPECT_EQ(ToString(FileState::kMissing), "missing");
  EXPECT_EQ(ToString(FileState::kWrongSize), "wrong_size");
  EXPECT_EQ(ToString(FileState::kPresent), "present");
  EXPECT_EQ(ToString(VerifyResult::kOk), "ok");
  EXPECT_EQ(ToString(VerifyResult::kMissing), "missing");
  EXPECT_EQ(ToString(VerifyResult::kMismatch), "mismatch");
  EXPECT_EQ(ToString(VerifyResult::kCancelled), "cancelled");
}

// --- manifest rules ----------------------------------------------------------

constexpr std::string_view kSha =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// A one-model manifest with `field` replaced (or removed, for an empty
// value), plus `defaults` verbatim.
std::string Manifest(const std::string &field = "", const std::string &value = "",
                     const std::string &defaults = "") {
  std::vector<std::pair<std::string, std::string>> fields = {
      {"id", R"("m")"},
      {"kind", R"("asr")"},
      {"url", R"("https://example.org/r/abc123/ggml-Base_1.0-en.bin")"},
      {"sha256", "\"" + std::string(kSha) + "\""},
      {"bytes", "3"},
      {"min_ram_mb", "100"},
      {"classes", R"(["desktop"])"},
      {"license", R"("MIT")"}};
  std::string body;
  for (auto &[k, v] : fields) {
    if (k == field) {
      v = value;
    }
    if (!v.empty()) {
      body += body.empty() ? "\"" : ",\"";
      body += k;
      body += "\":";
      body += v;
    }
  }
  std::string out = R"({"version":1,"models":[{)" + body + "}]";
  if (!defaults.empty()) {
    out += R"(,"defaults":)" + defaults;
  }
  return out + "}";
}

TEST(Models, AMinimalManifest) {
  const auto reg = Registry::Parse(Manifest());
  ASSERT_TRUE(reg) << reg.error().message;
  const auto &m = reg->models().front();
  EXPECT_EQ(m.file, "ggml-Base_1.0-en.bin");
  EXPECT_EQ(Hex(m.sha256), kSha);
  EXPECT_EQ(m.min_ram_mb, 100U);
  EXPECT_EQ(reg->Default(Kind::kAsr, Device{}), nullptr) << "no defaults section";

  const auto bare = Registry::Parse(Manifest("min_ram_mb", ""));
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->models().front().min_ram_mb, 0U);
  const auto unlicensed = Registry::Parse(Manifest("license", ""));
  ASSERT_TRUE(unlicensed);
  EXPECT_EQ(unlicensed->models().front().license, "");
}

TEST(Models, DefaultsPickTheFirstRuleThatFits) {
  const std::string sha(kSha);
  const auto reg = Registry::Parse(R"({"version":1,"models":[
    {"id":"big","kind":"asr","url":"https://x/big.bin","sha256":")" +
                                   sha +
                                   R"(","bytes":3,"min_ram_mb":4000,"classes":["desktop","mobile"]},
    {"id":"small","kind":"asr","url":"https://x/small.bin","sha256":")" +
                                   sha +
                                   R"(","bytes":3,"classes":["desktop","mobile"]}],
    "defaults":[{"class":"mobile","asr":"small"},
                {"class":"desktop","min_ram_mb":8000,"asr":"small"},
                {"class":"desktop"},
                {"class":"desktop","asr":"big"},
                {"class":"desktop","asr":"small"}]})");
  ASSERT_TRUE(reg) << reg.error().message;
  const auto pick = [&](DeviceClass cls, std::uint64_t ram) {
    return Id(reg->Default(Kind::kAsr, Device{.cls = cls, .ram_mb = ram}));
  };
  EXPECT_EQ(pick(DeviceClass::kDesktop, 9000), "small") << "the 8000 MB rule comes first";
  EXPECT_EQ(pick(DeviceClass::kDesktop, 5000), "big") << "past the rule that names no asr";
  EXPECT_EQ(pick(DeviceClass::kDesktop, 3000), "small") << "big needs 4000 MB";
  EXPECT_EQ(pick(DeviceClass::kMobile, 1), "small");
  EXPECT_EQ(pick(DeviceClass::kMobileSmall, 9000), "(none)");
}

struct BadCase {
  const char *name;
  std::string manifest;
};

class BadManifest : public ::testing::TestWithParam<BadCase> {};

TEST_P(BadManifest, IsRefused) {
  const auto reg = Registry::Parse(GetParam().manifest);
  ASSERT_FALSE(reg) << GetParam().manifest;
  EXPECT_FALSE(reg.error().message.empty());
}

std::string Sha(char hi_bad, char lo_bad) {
  std::string s(kSha);
  if (hi_bad != 0) {
    s[0] = hi_bad;
  }
  if (lo_bad != 0) {
    s[1] = lo_bad;
  }
  return "\"" + s + "\"";
}

std::string TwoModels() {
  return R"({"version":1,"models":[
  {"id":"a","kind":"asr","url":"https://x/a.bin","sha256":")" +
         std::string(kSha) + R"(","bytes":3,"classes":["desktop"]},
  {"id":"v","kind":"vad","url":"https://x/v.bin","sha256":")" +
         std::string(kSha) + R"(","bytes":3,"classes":["mobile"]}])";
}

INSTANTIATE_TEST_SUITE_P(
    Rules, BadManifest,
    ::testing::Values(
        BadCase{"not_json", "{"}, BadCase{"not_object", "[]"},
        BadCase{"no_version", R"({"models":[]})"},
        BadCase{"version_string", R"({"version":"1","models":[]})"},
        BadCase{"version_2", R"({"version":2,"models":[]})"},
        BadCase{"no_models", R"({"version":1})"},
        BadCase{"models_not_list", R"({"version":1,"models":{}})"},
        BadCase{"model_not_object", R"({"version":1,"models":[1]})"},
        BadCase{"no_id", Manifest("id", "")}, BadCase{"empty_id", Manifest("id", R"("")")},
        BadCase{"id_number", Manifest("id", "7")}, BadCase{"no_kind", Manifest("kind", "")},
        BadCase{"bad_kind", Manifest("kind", R"("tts")")}, BadCase{"no_url", Manifest("url", "")},
        BadCase{"http_url", Manifest("url", R"("http://x/m.bin")")},
        BadCase{"url_dir", Manifest("url", R"("https://x/m/")")},
        BadCase{"url_dotfile", Manifest("url", R"("https://x/.m")")},
        BadCase{"url_space", Manifest("url", R"("https://x/m n.bin")")},
        BadCase{"url_tilde", Manifest("url", R"("https://x/m~.bin")")},
        BadCase{"no_sha", Manifest("sha256", "")},
        BadCase{"short_sha", Manifest("sha256", R"("abcd")")},
        BadCase{"upper_sha", Manifest("sha256", Sha('A', 0))},
        BadCase{"g_sha", Manifest("sha256", Sha(0, 'g'))},
        BadCase{"dash_sha", Manifest("sha256", Sha('-', 0))},
        BadCase{"no_bytes", Manifest("bytes", "")},
        BadCase{"bytes_string", Manifest("bytes", R"("3")")},
        BadCase{"bytes_zero", Manifest("bytes", "0")},
        BadCase{"bytes_negative", Manifest("bytes", "-3")},
        BadCase{"ram_string", Manifest("min_ram_mb", R"("1")")},
        BadCase{"no_classes", Manifest("classes", "")},
        BadCase{"classes_string", Manifest("classes", R"("desktop")")},
        BadCase{"classes_empty", Manifest("classes", "[]")},
        BadCase{"class_number", Manifest("classes", "[1]")},
        BadCase{"class_unknown", Manifest("classes", R"(["watch"])")},
        BadCase{"duplicate_id",
                TwoModels().replace(TwoModels().find(R"("id":"v")"), 8, R"("id":"a")") + "}"},
        BadCase{"duplicate_file",
                TwoModels().replace(TwoModels().find("x/v.bin"), 7, "x/a.bin") + "}"},
        BadCase{"defaults_not_list", TwoModels() + R"(,"defaults":{}})"},
        BadCase{"default_not_object", TwoModels() + R"(,"defaults":[1]})"},
        BadCase{"default_no_class", TwoModels() + R"(,"defaults":[{"asr":"a"}]})"},
        BadCase{"default_bad_class", TwoModels() + R"(,"defaults":[{"class":"tv"}]})"},
        BadCase{"default_bad_ram",
                TwoModels() + R"(,"defaults":[{"class":"desktop","min_ram_mb":-1}]})"},
        BadCase{"default_unknown_id",
                TwoModels() + R"(,"defaults":[{"class":"desktop","asr":"zz"}]})"},
        BadCase{"default_id_number", TwoModels() + R"(,"defaults":[{"class":"desktop","asr":5}]})"},
        BadCase{"default_wrong_kind",
                TwoModels() + R"(,"defaults":[{"class":"mobile","asr":"v"}]})"},
        BadCase{"default_null_id",
                TwoModels() + R"(,"defaults":[{"class":"desktop","asr":null}]})"},
        BadCase{"default_model_not_for_class",
                TwoModels() + R"(,"defaults":[{"class":"mobile","asr":"a"}]})"}),
    [](const ::testing::TestParamInfo<BadCase> &p) { return std::string(p.param.name); });

// --- model files -------------------------------------------------------------

class ModelFiles : public ::testing::Test {
 public:
  ModelFiles() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("micnote-models-test-" + std::to_string(rd()));
    fs::create_directories(dir_);
  }
  ModelFiles(const ModelFiles &) = delete;
  ModelFiles &operator=(const ModelFiles &) = delete;
  ModelFiles(ModelFiles &&) = delete;
  ModelFiles &operator=(ModelFiles &&) = delete;
  ~ModelFiles() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

 protected:
  // A model whose file is `content`, written to the directory.
  micnote::models::Model Write(const std::string &content, const std::string &file = "m.bin") {
    std::ofstream(dir_ / file, std::ios::binary) << content;
    const auto bytes = std::as_bytes(std::span(content));
    const std::string json = R"({"version":1,"models":[{"id":"m","kind":"asr","url":"https://x/)" +
                             file + R"(","sha256":")" + Hex(micnote::crypto::Sha256(bytes)) +
                             R"(","bytes":)" + std::to_string(content.size()) +
                             R"(,"classes":["desktop"]}]})";
    return Registry::Parse(json).value().models().front();
  }
  [[nodiscard]] const fs::path &dir() const { return dir_; }

 private:
  fs::path dir_;
};

TEST_F(ModelFiles, CheckLooksAtPresenceAndSize) {
  auto m = Write("abc");
  EXPECT_EQ(Check(m, dir()), FileState::kPresent);
  m.bytes = 4;
  EXPECT_EQ(Check(m, dir()), FileState::kWrongSize);
  m.file = "absent.bin";
  EXPECT_EQ(Check(m, dir()), FileState::kMissing);
  fs::create_directories(dir() / "adir.bin");
  m.file = "adir.bin";
  EXPECT_EQ(Check(m, dir()), FileState::kMissing) << "a directory isn't a model";
}

TEST_F(ModelFiles, VerifyHashesInBlocks) {
  // Three 64 KB blocks and a bit: the stream, not one buffer.
  std::string big(200000, '\0');
  // A fixed seed on purpose: the same bytes every run.
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::minstd_rand rng(7);
  for (char &c : big) {
    c = static_cast<char>(rng() & 0xFFU);
  }
  const auto m = Write(big);
  std::vector<std::uint64_t> seen;
  EXPECT_EQ(Verify(m, dir(),
                   [&](std::uint64_t done, std::uint64_t total) {
                     EXPECT_EQ(total, 200000U);
                     seen.push_back(done);
                     return true;
                   }),
            VerifyResult::kOk);
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back(), 200000U);
  EXPECT_GE(seen.size(), 4U);
  EXPECT_EQ(Verify(m, dir()), VerifyResult::kOk) << "without a progress callback";
}

TEST_F(ModelFiles, VerifyCatchesEveryKindOfWrongFile) {
  auto m = Write("abc");
  std::ofstream(dir() / "m.bin", std::ios::binary) << "abd";
  EXPECT_EQ(Verify(m, dir()), VerifyResult::kMismatch) << "same size, other bytes";
  std::ofstream(dir() / "m.bin", std::ios::binary) << "abcd";
  EXPECT_EQ(Verify(m, dir()), VerifyResult::kMismatch) << "wrong size";
  m.file = "absent.bin";
  EXPECT_EQ(Verify(m, dir()), VerifyResult::kMissing);
}

TEST_F(ModelFiles, VerifyCanBeCancelled) {
  const auto m = Write(std::string(150000, 'x'));
  int calls = 0;
  EXPECT_EQ(Verify(m, dir(),
                   [&](std::uint64_t, std::uint64_t) {
                     ++calls;
                     return false;
                   }),
            VerifyResult::kCancelled);
  EXPECT_EQ(calls, 1);
}

// The file is cut short while it's being hashed: fewer bytes than the
// manifest says is a mismatch even before the hash is compared.
TEST_F(ModelFiles, VerifyNoticesAFileThatShrinks) {
  const auto m = Write(std::string(200000, 'y'));
  bool cut = false;
  EXPECT_EQ(Verify(m, dir(),
                   [&](std::uint64_t, std::uint64_t) {
                     if (!cut) {
                       fs::resize_file(dir() / "m.bin", 70000);
                       cut = true;
                     }
                     return true;
                   }),
            VerifyResult::kMismatch);
}

}  // namespace
