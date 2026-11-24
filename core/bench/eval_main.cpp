// micnote-eval: how well a local model plans (stage 3 §4.8).
//
//   micnote-eval --model MODEL.gguf --dataset FILE.jsonl [--backend auto|cpu] [--show]
//
// Prints a Markdown table row for docs/measurements/stage3.md. Nothing is
// executed: it only drafts. The exit code is 0 whatever the scores.
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "plan_score.hpp"
#include "planner/action_validator.hpp"
#include "planner/llm/llama_backend.hpp"
#include "planner/planner.hpp"
#include "planner/schema_registry.hpp"
#include <nlohmann/json.hpp>

namespace {

using micnote::bench::Found;
using micnote::bench::PlanScore;
using nlohmann::json;
namespace pl = micnote::planner;

std::string Share(std::size_t right, std::size_t of) { return std::format("{}/{}", right, of); }

PlanScore RunCase(pl::Llm &llm, const pl::SchemaRegistry &registry, const json &c, bool show) {
  // A line is a second of audio; a marker comes before its 0-based line.
  std::vector<pl::Line> lines;
  for (const json &text : c.at("lines")) {
    const auto at = static_cast<std::int64_t>(lines.size()) * 1000;
    lines.push_back({.start_ms = at, .end_ms = at + 1000, .text = text.get<std::string>()});
  }
  std::vector<std::int64_t> markers;
  for (const json &m : c.at("markers")) {
    markers.push_back(m.get<std::int64_t>() * 1000);
  }
  pl::PlannerOptions options;
  options.today = c.at("today").get<std::string>();
  options.utc_offset = c.at("utc_offset").get<std::string>();
  const auto items = pl::Segment(llm, registry, lines, markers, options);
  std::vector<Found> found;
  if (!items) {
    found.push_back({.first = 1, .last = lines.size(), .draft = json(), .valid = false});
    std::cerr << std::format("{}: segmenting failed ({})\n", c.at("id").get<std::string>(),
                             static_cast<int>(items.error()));
    return micnote::bench::ScorePlan(c, found);
  }
  for (const pl::Item &item : *items) {
    if (item.intent == nullptr) {
      continue;
    }
    Found f{.first = static_cast<std::size_t>(item.start_ms / 1000) + 1,
            .last = static_cast<std::size_t>(item.end_ms / 1000),
            .draft = json(),
            .valid = false};
    auto draft = pl::Draft(llm, item, c.at("context"), options);
    if (draft) {
      f.valid = pl::Validate(registry, *draft, pl::Stage::kDraft).empty();
      f.draft = std::move(*draft);
    }
    if (show) {
      std::cerr << std::format(
          "  {} [{}-{}] {}.{} {} {} missing {}\n", c.at("id").get<std::string>(), f.first, f.last,
          item.intent->connector, item.intent->type, f.valid ? "valid" : "INVALID",
          f.draft.is_null() ? "FAILED" : f.draft.at("params").dump(),
          f.draft.is_null() ? "" : f.draft.at("missing_fields").dump());
    }
    found.push_back(std::move(f));
  }
  return micnote::bench::ScorePlan(c, found);
}

int Run(std::span<char *> args) {
  std::map<std::string, std::string, std::less<>> opts;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string o = args[i];
    if (o == "--show") {
      opts[o] = "1";
      continue;
    }
    if ((o != "--model" && o != "--dataset" && o != "--backend") || i + 1 == args.size()) {
      opts.clear();
      break;
    }
    opts[o] = args[++i];
  }
  if (!opts.contains("--model") || !opts.contains("--dataset")) {
    std::cerr << "usage: micnote-eval --model MODEL.gguf --dataset FILE.jsonl [--backend auto|cpu] "
                 "[--show]\n";
    return 2;
  }
  auto llm = pl::LlamaLlm::Load(opts.at("--model"),
                                opts.contains("--backend") ? opts.at("--backend") : "auto");
  const auto registry = pl::SchemaRegistry::Builtin();
  std::ifstream data(opts.at("--dataset"));
  if (!llm || !registry || !data) {
    std::cerr << "micnote-eval: can't load the model or the dataset\n";
    return 1;
  }
  PlanScore total;
  std::size_t cases = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::string line; std::getline(data, line);) {
    const json c = json::parse(line);
    const PlanScore s = RunCase(**llm, *registry, c, opts.contains("--show"));
    if (opts.contains("--show")) {
      std::cerr << std::format("{}: {} of {} found, {} right, {} of {} params\n",
                               c.at("id").get<std::string>(), s.matched, s.expected, s.intents,
                               s.params_right, s.params);
    }
    total += s;
    ++cases;
  }
  if (cases == 0) {
    std::cerr << "micnote-eval: the dataset is empty\n";
    return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout
      << "| Model | Backend | Cases | Item F1 | Intents | Params | Keywords | Invalid drafts | "
         "Failed | Forbidden | Injections held | s/case |\n|---|---|---|---|---|---|---|---|---|"
         "---|---|---|\n"
      << std::format("| {} | {} | {} | {:.2f} | {} | {} | {} | {} | {} | {} | {} | {:.0f} |\n",
                     (*llm)->Model(), (*llm)->Backend(), cases, total.F1(),
                     Share(total.intents, total.expected), Share(total.params_right, total.params),
                     Share(total.texts_right, total.texts), Share(total.invalid, total.found),
                     total.failed, total.forbidden, Share(total.injections_held, total.injections),
                     secs / static_cast<double>(cases));
  return 0;
}

}  // namespace

int main(int argc, char **argv) try {
  return Run(std::span(argv, static_cast<std::size_t>(argc)).subspan(1));
} catch (...) {
  static_cast<void>(std::fputs("micnote-eval: failed (is the dataset well formed?)\n", stderr));
  return 1;
}
