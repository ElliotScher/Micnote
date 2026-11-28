#include "planner/redact.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using micnote::planner::LooksSecret;
using micnote::planner::RedactOptions;
using micnote::planner::Redactor;
using nlohmann::json;

std::string Redacted(const std::string &text, RedactOptions options = {}) {
  Redactor r(std::move(options));
  return r.Redact(text);
}

TEST(Redact, Emails) {
  Redactor r({});
  EXPECT_EQ(r.Redact("mail jeff@example.com and Ann.Lee+x@mail.co.uk."),
            "mail [EMAIL_1] and [EMAIL_2].");
  // The same address keeps its placeholder, in later calls too.
  EXPECT_EQ(r.Redact("again: jeff@example.com"), "again: [EMAIL_1]");
  EXPECT_EQ(r.Count(), 2U);
  // Not addresses.
  EXPECT_EQ(Redacted("at @home, me@localhost, a@b.c, x@.com, y@a..com, z@a.c0m"),
            "at @home, me@localhost, a@b.c, x@.com, y@a..com, z@a.c0m");
  EXPECT_EQ(Redacted("@a.com and a@"), "@a.com and a@");
  // A trailing dot or dash ends it.
  EXPECT_EQ(Redacted("to bo@ex.org-"), "to [EMAIL_1]-");
  EXPECT_EQ(Redacted("a_b%c+d-e@EX.COM"), "[EMAIL_1]");
  EXPECT_EQ(Redacted("~x@ex.com"), "~[EMAIL_1]");
}

TEST(Redact, Phones) {
  EXPECT_EQ(Redacted("call +1 (555) 123-4567 now"), "call [PHONE_1] now");
  EXPECT_EQ(Redacted("or (555) 123-4567."), "or [PHONE_1].");
  EXPECT_EQ(Redacted("+44 20 7946 0958"), "[PHONE_1]");
  EXPECT_EQ(Redacted("555.123.4567"), "[PHONE_1]");
  EXPECT_EQ(Redacted("5551234567"), "[PHONE_1]");
  EXPECT_EQ(Redacted("+15551234"), "[PHONE_1]");
  EXPECT_EQ(Redacted("+(44) 20 7946 0958"), "[PHONE_1]");
  EXPECT_EQ(Redacted("555-1234"), "[PHONE_1]");  // a local number
}

TEST(Redact, NotPhones) {
  // Dates, short numbers, decimals, versions, words.
  EXPECT_EQ(Redacted("on 2026-11-24 at 3"), "on 2026-11-24 at 3");
  EXPECT_EQ(Redacted("3 2026-11-24 and 24 2026-11-25"), "3 2026-11-24 and 24 2026-11-25");
  EXPECT_EQ(Redacted("issue 1234 and PR #56"), "issue 1234 and PR #56");
  EXPECT_EQ(Redacted("pi is 3.14159265"), "pi is 3.14159265");
  EXPECT_EQ(Redacted("v1.2.3 and 12345678abc"), "v1.2.3 and 12345678abc");
  EXPECT_EQ(Redacted("1234567"), "1234567");                    // no groups
  EXPECT_EQ(Redacted("1234567890123456"), "1234567890123456");  // too long
  EXPECT_EQ(Redacted("a1234567890 x-5551234567"), "a1234567890 x-5551234567");
  EXPECT_EQ(Redacted("( and + and 12 - 3"), "( and + and 12 - 3");
  EXPECT_EQ(Redacted("("), "(");
  EXPECT_EQ(Redacted("2026-11"), "2026-11");
  EXPECT_EQ(Redacted("2026-1x-24"), "2026-1x-24");
  EXPECT_EQ(Redacted("12 34 ----56"), "12 34 ----56");
  EXPECT_EQ(Redacted("123 456 "), "123 456 ");
  EXPECT_EQ(Redacted("a ++5551234567"), "a ++5551234567");
}

TEST(Redact, Urls) {
  EXPECT_EQ(Redacted("see https://ex.com/reset?token=abc#x."),
            "see https://ex.com/reset?[REDACTED].");
  EXPECT_EQ(Redacted("(http://ex.com/a/b)"), "(http://ex.com/a/b)");
  EXPECT_EQ(Redacted("https://ex.com/r/0123456789abcdef0123456789abcdef/x"),
            "https://ex.com/r/[SECRET]/x");
  EXPECT_EQ(Redacted("https://bob:hunter2@ex.com/a"), "https://[SECRET]@ex.com/a");
  EXPECT_EQ(Redacted("https://bob:pw@ex.com"), "https://[SECRET]@ex.com");
  EXPECT_EQ(Redacted("https://ex.com/u@me"), "https://ex.com/u@me");
  EXPECT_EQ(Redacted("https://ex.com/a?b=me@x.com"), "https://ex.com/a?[REDACTED]");
  Redactor r({});
  EXPECT_EQ(r.Redact("https://ex.com/?q=1 https://ex.com/?q=2"),
            "https://ex.com/?[REDACTED] https://ex.com/?[REDACTED]");
  EXPECT_EQ(r.Count(), 2U);
}

TEST(Redact, Secrets) {
  EXPECT_TRUE(LooksSecret("ghp_abcdefghijklmnopqrstuvwxyz0123456789"));
  EXPECT_TRUE(LooksSecret("github_pat_11AAAAAAA0abcdefghij"));
  EXPECT_TRUE(LooksSecret("sk-ant-api03-abcdefghijkl"));
  EXPECT_TRUE(LooksSecret("AKIAABCDEFGHIJKLMNOP"));
  EXPECT_TRUE(LooksSecret("-----BEGIN"));
  EXPECT_TRUE(LooksSecret("0123456789abcdef0123456789ABCDEF"));      // hex
  EXPECT_TRUE(LooksSecret("dGhpcyBpcyBhIHNlY3JldCBrZXkgMTIzNA=="));  // base64
  EXPECT_TRUE(LooksSecret("Ab1-_+/=Ab1-_+/=Ab1-_+/=Ab1-_+/="));
  EXPECT_FALSE(LooksSecret("sk-short"));
  EXPECT_FALSE(LooksSecret("ghp_"));
  EXPECT_FALSE(LooksSecret("antidisestablishmentarianism-word"));
  EXPECT_FALSE(LooksSecret("state-of-the-art-2026-planning-document-v2"));
  EXPECT_FALSE(LooksSecret("ALLUPPERCASEANDDIGITS0123456789XYZ"));
  EXPECT_FALSE(LooksSecret("alllowercaseanddigits0123456789xyz"));
  EXPECT_FALSE(LooksSecret("Mixed.Case.With.Dots.0123456789.abcdef"));
  EXPECT_FALSE(LooksSecret("shortHEX12"));
  EXPECT_FALSE(LooksSecret("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefgh"));  // no digit
  EXPECT_FALSE(LooksSecret("Abcdefghijklmnop0123456789~abcdefg"));
  // Whitespace of every kind separates tokens.
  EXPECT_EQ(Redacted("a\tghp_abcdefghijklmnopqrstuvwxyz0123456789\nb\rc"), "a\t[SECRET]\nb\rc");

  Redactor r({});
  EXPECT_EQ(r.Redact("my key is (ghp_abcdefghijklmnopqrstuvwxyz0123456789)."),
            "my key is ([SECRET]).");
  EXPECT_EQ(r.Count(), 1U);
  // Never restored.
  EXPECT_EQ(r.Restore("[SECRET]"), "[SECRET]");
}

TEST(Redact, TermsAndNames) {
  RedactOptions o;
  o.terms = {"Acme", "Acme Labs", "", "PHONE"};
  o.names = {"Jeff"};
  Redactor r(o);
  EXPECT_EQ(r.Redact("Tell jeff that ACME LABS and acme won, not Acmes or Jeffrey."),
            "Tell [PERSON_1] that [TERM_1] and [TERM_2] won, not Acmes or Jeffrey.");
  // A term can't match inside a placeholder.
  EXPECT_EQ(r.Redact("call 555-123-4567"), "call [PHONE_1]");
  EXPECT_EQ(r.Redact("_acme acme_ éacme café-acme"), "_acme acme_ éacme café-[TERM_2]");
}

TEST(Redact, BracketsCantForgePlaceholders) {
  Redactor r({});
  EXPECT_EQ(r.Redact("jeff@example.com"), "[EMAIL_1]");
  // Said aloud, "[EMAIL_1]" would otherwise come back as the address.
  EXPECT_EQ(r.Redact("write [EMAIL_1] here"), "write (EMAIL_1) here");
}

TEST(Redact, RoundTrip) {
  RedactOptions o;
  o.terms = {"Project Falcon"};
  Redactor r(o);
  const json context = {{"contacts", {{{"name", "Jeff"}, {"emails", {"jeff@example.com"}}}}},
                        {"n", 3},
                        {"note", "Project Falcon, call +1 555 123 4567"}};
  const json sent = r.RedactJson(context);
  EXPECT_EQ(sent.dump(), R"({"contacts":[{"emails":["[EMAIL_1]"],"name":"Jeff"}],"n":3,)"
                         R"("note":"[TERM_1], call [PHONE_1]"})");
  EXPECT_EQ(r.RestoreJson(sent), context);
  // What Claude writes, with the placeholders it was given.
  const json params = {{"to", {"[EMAIL_1]"}}, {"body", "About [TERM_1] [x] [EMAIL_9] [open"}};
  EXPECT_EQ(r.RestoreJson(params), (json{{"to", {"jeff@example.com"}},
                                         {"body", "About Project Falcon [x] [EMAIL_9] [open"}}));
  EXPECT_EQ(r.RestoreJson(json(7)), json(7));
  EXPECT_EQ(r.Count(), 3U);
}

}  // namespace
