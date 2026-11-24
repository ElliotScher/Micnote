#include "planner/grammar.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

namespace micnote::planner {

namespace {

using nlohmann::json;

// ws is one optional space or nothing: a model can't pad its output forever.
constexpr std::string_view kCommon = R"(ws ::= " "?
string ::= "\"" char* "\""
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])
integer ::= "-"? ("0" | [1-9] [0-9]{0,15})
boolean ::= "true" | "false"
email ::= "\"" [a-zA-Z0-9._%+-]+ "@" [a-zA-Z0-9-]+ ("." [a-zA-Z0-9-]+)+ "\""
datetime ::= "\"" [0-9]{4} "-" [0-1] [0-9] "-" [0-3] [0-9] "T" [0-2] [0-9] ":" [0-5] [0-9] ":" [0-5] [0-9] ("Z" | [+-] [0-2] [0-9] ":" [0-5] [0-9]) "\""
)";

// GCOVR_EXCL_BR_START: gcc's exception cleanup for strings and json
// temporaries; planner_grammar_test.cpp covers each kind of schema.
class Writer {
 public:
  std::string Run(const json &schema) {
    const std::string root = Rule(schema, "root");
    return "root ::= " + root + "\n" + rules_ + std::string(kCommon);
  }

 private:
  // An expression matching schema; rules it needs are named after `name`.
  std::string Rule(const json &schema, const std::string &name) {
    std::string e = Bare(schema, name);
    return schema.contains("x-nullable") ? "(" + e + R"( | "null"))" : e;
  }

  std::string Bare(const json &schema, const std::string &name) {
    std::string type = schema.at("type").get<std::string>();
    if (type == "object") {
      return Define(name + "-object", Object(schema, name));
    }
    if (type == "array") {
      const std::string item = Rule(schema.at("items"), name + "-item");
      return Define(name + "-list",
                    R"("[" ws ()" + item + R"( ("," ws )" + item + R"(){0,49})? "]")");
    }
    if (type == "string") {
      if (schema.contains("enum")) {
        std::string any;
        for (const json &v : schema.at("enum")) {
          any += (any.empty() ? "" : " | ") + GbnfLiteral(v.dump());
        }
        return "(" + any + ")";
      }
      if (!schema.contains("format")) {
        return "string";
      }
      return schema.at("format") == "email" ? "email" : "datetime";
    }
    return type;  // integer, boolean
  }

  std::string Define(const std::string &name, const std::string &body) {
    rules_ += name + " ::= " + body + "\n";
    return name;
  }

  std::string Object(const json &schema, const std::string &name) {
    std::vector<std::string> required;
    std::vector<std::string> optional;
    for (const auto &[key, sub] : schema.at("properties").items()) {
      // Rule names allow letters, digits and dashes.
      std::string part = key;
      std::ranges::replace(part, '_', '-');
      std::string member = name;
      member += '-';
      member += part;
      const std::string pair = GbnfLiteral(json(key).dump() + ":") + " ws " + Rule(sub, member);
      const bool needed =
          std::ranges::find(schema.at("required"), json(key)) != schema.at("required").end();
      (needed ? required : optional).push_back(pair);
    }
    std::string body;
    for (const std::string &p : required) {
      body += (body.empty() ? "" : R"( "," ws )") + p;
    }
    if (!required.empty()) {
      for (const std::string &p : optional) {
        body += R"( ("," ws )" + p + ")?";
      }
    } else if (!optional.empty()) {
      body = Any(optional);
    }
    return R"("{" ws )" + body + (body.empty() ? "" : " ") + R"(ws "}")";
  }

  // Any of the members, in order, with commas between: one alternative per
  // member that can come first.
  static std::string Any(const std::vector<std::string> &members) {
    std::string out;
    for (std::size_t i = 0; i < members.size(); ++i) {
      out += (i == 0 ? "" : " | ") + members[i];
      for (std::size_t k = i + 1; k < members.size(); ++k) {
        out += R"( ("," ws )" + members[k] + ")?";
      }
    }
    return "(" + out + ")?";
  }

  std::string rules_;
};

bool Blank(const json &v) {
  return v.is_null() || (v.is_string() && v.get_ref<const std::string &>().empty()) ||
         (v.is_array() && v.empty());
}

double Confidence(std::string_view word) {
  if (word == "high") {
    return 0.9;
  }
  return word == "medium" ? 0.6 : 0.3;
}

}  // namespace

std::string GbnfLiteral(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out + "\"";
}

std::string ToGrammar(const json &schema) { return Writer().Run(schema); }

json DraftSchema(const json &params_schema) {
  json params = params_schema;
  for (const auto &[name, sub] : params["properties"].items()) {
    const bool text =
        sub.at("type") == "string" && !sub.contains("enum") && !sub.contains("format");
    if (!text && sub.at("type") != "array") {  // those can be left "" or []
      params["properties"][name]["x-nullable"] = true;
    }
  }
  return {{"type", "object"},
          {"additionalProperties", false},
          {"required", {"params", "rationale", "confidence"}},
          {"properties",
           {{"params", std::move(params)},
            {"rationale", {{"type", "string"}}},
            {"confidence", {{"type", "string"}, {"enum", {"low", "medium", "high"}}}}}}};
}

std::optional<json> FinishDraft(const ActionType &action, std::string_view model_output) {
  json d = json::parse(model_output, nullptr, false);
  if (!d.is_object() || !d.contains("params") || !d["params"].is_object()) {
    return std::nullopt;
  }
  json params = json::object();
  for (const auto &[key, value] : d["params"].items()) {
    if (!Blank(value)) {
      params[key] = value;
    }
  }
  json missing = json::array();
  for (const json &r : action.params_schema.at("required")) {
    if (!params.contains(r.get<std::string>())) {
      missing.push_back(r);
    }
  }
  const auto text = [&](const char *key) {
    const auto it = d.find(key);
    const auto *t = it != d.end() ? it->get_ptr<const std::string *>() : nullptr;
    return t != nullptr ? *t : std::string();
  };
  return json{{"connector", action.connector},       {"type", action.type},
              {"schema_version", action.version},    {"params", std::move(params)},
              {"rationale", text("rationale")},      {"confidence", Confidence(text("confidence"))},
              {"missing_fields", std::move(missing)}};
}
// GCOVR_EXCL_BR_STOP

}  // namespace micnote::planner
