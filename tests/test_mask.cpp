// test_mask.cpp — guard rails for src/mask.cpp
//
// Masking is a privacy feature with a nasty failure mode: when it misses, a
// secret stays readable on screen and nothing says so; when it over-reaches,
// ordinary text gets starred out and the preview turns to noise. Both are
// silent, so every scanner is pinned from both directions here — a real secret
// is masked, and a near-miss that is not that secret is left alone.
//
// Headless throughout: Apply() takes a string and a config and returns a
// string. No window, no clipboard, no item.
#include <string>

#include "mask.h"
#include "testfw.h"

namespace {

using mask::Config;

// Single-scanner configs, so each case exercises exactly one detector and a
// failure names the scanner rather than "some combination".
Config None() { return Config{false, false, false, false, false}; }
Config EmailOnly() { Config c = None(); c.email = true; return c; }
Config PhoneOnly() { Config c = None(); c.phone = true; return c; }
Config IdOnly() { Config c = None(); c.idCard = true; return c; }
Config KeyOnly() { Config c = None(); c.apiKey = true; return c; }
Config PwdOnly() { Config c = None(); c.password = true; return c; }
Config AllOn() { return Config{true, true, true, true, true}; }
Config PlanDefaults() { return Config{}; }  // struct defaults: phone/id/password on

// ---------------- Config surface ----------------

void DefaultsMatchThePlan() {
    const Config c = PlanDefaults();
    CWZ_CHECK(!c.email);    // off: emails are common and rarely the secret itself
    CWZ_CHECK(c.phone);     // on
    CWZ_CHECK(c.idCard);    // on
    CWZ_CHECK(!c.apiKey);   // off
    CWZ_CHECK(c.password);  // on, but see the false-positive cases below
}

void AnyEnabledReflectsConfig() {
    CWZ_CHECK(!mask::AnyEnabled(None()));
    CWZ_CHECK(mask::AnyEnabled(PlanDefaults()));
    CWZ_CHECK(mask::AnyEnabled(EmailOnly()));
    CWZ_CHECK(mask::AnyEnabled(KeyOnly()));
}

void AllDisabledReturnsInputUnchanged() {
    // Even text that every scanner would flag comes back byte-identical when
    // the whole feature is off — this is the path most users are on.
    const std::wstring secret = L"13812345678";
    CWZ_CHECK_EQ(mask::Apply(secret, None()), secret);
}

// ---------------- Guard rails ----------------

void TooLongTextIsSkipped() {
    // 5011 chars: a phone at the front, then filler past the 5000 limit. The
    // guard must win — no scan, so the phone stays visible.
    const std::wstring big = L"13812345678" + std::wstring(5000, L'a');
    CWZ_CHECK_EQ(mask::Apply(big, PhoneOnly()), big);
}

void DataUriIsSkipped() {
    const std::wstring uri = L"data:text/plain;base64,13812345678";
    CWZ_CHECK_EQ(mask::Apply(uri, AllOn()), uri);
    // The same digits without the data: prefix ARE masked, proving the skip is
    // keyed on the prefix and not on the content.
    const std::wstring bare = L"text/plain,13812345678";
    CWZ_CHECK(mask::Apply(bare, PhoneOnly()).find(L"138****678") != std::wstring::npos);
}

// ---------------- Email ----------------

void EmailMasksLocalPartKeepsDomain() {
    // The domain is not the secret and is what lets the user recognise the
    // address, so only the local part is masked.
    CWZ_CHECK_EQ(mask::Apply(L"alice.smith@gmail.com", EmailOnly()),
                 L"ali****ith@gmail.com");
}

void EmailInsideSentenceIsMasked() {
    CWZ_CHECK_EQ(mask::Apply(L"mail bob@corp.co.uk now", EmailOnly()),
                 L"mail ****b@corp.co.uk now");
}

void EmailNeedsValidTld() {
    // No dot / non-alpha TLD / empty label → not an address.
    CWZ_CHECK_EQ(mask::Apply(L"a@b", EmailOnly()), L"a@b");
    CWZ_CHECK_EQ(mask::Apply(L"user@domain", EmailOnly()), L"user@domain");
    CWZ_CHECK_EQ(mask::Apply(L"user@.com", EmailOnly()), L"user@.com");
    CWZ_CHECK_EQ(mask::Apply(L"user@domain.c", EmailOnly()), L"user@domain.c");
}

void EmailOffByDefault() {
    CWZ_CHECK_EQ(mask::Apply(L"alice.smith@gmail.com", PlanDefaults()),
                 L"alice.smith@gmail.com");
}

// ---------------- Phone ----------------

void PhoneMasksElevenDigits() {
    CWZ_CHECK_EQ(mask::Apply(L"13812345678", PhoneOnly()), L"138****678");
}

void PhoneToleratesCountryCodeAndSeparators() {
    CWZ_CHECK_EQ(mask::Apply(L"+86 138-1234-5678", PhoneOnly()), L"+86 138****678");
    CWZ_CHECK_EQ(mask::Apply(L"138 1234 5678", PhoneOnly()), L"138****678");
}

void PhoneRejectsWhenInsideLongerNumber() {
    // A digit immediately before the 1[3-9] start means this is the tail of a
    // longer number, not a mobile.
    CWZ_CHECK_EQ(mask::Apply(L"9913812345678", PhoneOnly()), L"9913812345678");
    // Digits after the 11th mean the same at the other end.
    CWZ_CHECK_EQ(mask::Apply(L"138123456789999", PhoneOnly()), L"138123456789999");
}

void PhoneRejectsWrongPrefix() {
    // Second digit must be 3-9; 12... / 10... are not mobiles.
    CWZ_CHECK_EQ(mask::Apply(L"12812345678", PhoneOnly()), L"12812345678");
    CWZ_CHECK_EQ(mask::Apply(L"10812345678", PhoneOnly()), L"10812345678");
}

// ---------------- ID card ----------------

void IdCardMasksValidNumber() {
    // 110105 1949 12 31 002 X — checksum verifies (MOD 11-2 → 'X').
    CWZ_CHECK_EQ(mask::Apply(L"11010519491231002X", IdOnly()), L"110****02X");
    // Lowercase x check digit is accepted too.
    CWZ_CHECK_EQ(mask::Apply(L"11010519491231002x", IdOnly()), L"110****02x");
}

void IdCardRejectsBadChecksum() {
    // Same number, last digit changed → checksum fails → left alone.
    CWZ_CHECK_EQ(mask::Apply(L"110105194912310021", IdOnly()), L"110105194912310021");
}

void IdCardRejectsImpossibleDate() {
    // Month 13 is refused before the checksum is even consulted.
    CWZ_CHECK_EQ(mask::Apply(L"11010519491331002X", IdOnly()), L"11010519491331002X");
}

void IdCardRequiresWordBoundary() {
    // Glued to a preceding letter → not a standalone id.
    CWZ_CHECK_EQ(mask::Apply(L"X11010519491231002X", IdOnly()), L"X11010519491231002X");
}

void IdCardOffSwitchLeavesItAlone() {
    CWZ_CHECK_EQ(mask::Apply(L"11010519491231002X", None()), L"11010519491231002X");
}

// ---------------- API key ----------------

void ApiKeyMasksKnownPrefix() {
    CWZ_CHECK_EQ(mask::Apply(L"sk-ABCDEFGHIJKLMNOPQRST", KeyOnly()), L"sk-****RST");
}

void ApiKeyPrefixIsCaseInsensitive() {
    CWZ_CHECK_EQ(mask::Apply(L"SK-ABCDEFGHIJKLMNOPQRST", KeyOnly()), L"SK-****RST");
}

void ApiKeyRejectsShortTail() {
    // Fewer than 20 body chars is not a key.
    CWZ_CHECK_EQ(mask::Apply(L"sk-ABC", KeyOnly()), L"sk-ABC");
}

void ApiKeyRequiresLeftBoundary() {
    // "sk" glued to a preceding word char is part of a longer token.
    CWZ_CHECK_EQ(mask::Apply(L"xsk-ABCDEFGHIJKLMNOPQRST", KeyOnly()),
                 L"xsk-ABCDEFGHIJKLMNOPQRST");
}

void ApiKeyOffByDefault() {
    CWZ_CHECK_EQ(mask::Apply(L"sk-ABCDEFGHIJKLMNOPQRST", PlanDefaults()),
                 L"sk-ABCDEFGHIJKLMNOPQRST");
}

// ---------------- Password heuristic ----------------

void PasswordMasksWholeToken() {
    // 8-64 chars, no whitespace, all four classes.
    CWZ_CHECK_EQ(mask::Apply(L"Abcdef1!", PwdOnly()), L"Abc****f1!");
    CWZ_CHECK_EQ(mask::Apply(L"MyP@ssw0rd", PwdOnly()), L"MyP****0rd");
}

void PasswordRejectsNearMisses() {
    CWZ_CHECK_EQ(mask::Apply(L"Ab1!xyz", PwdOnly()), L"Ab1!xyz");        // < 8 chars
    CWZ_CHECK_EQ(mask::Apply(L"abcdefg1!", PwdOnly()), L"abcdefg1!");    // no uppercase
    CWZ_CHECK_EQ(mask::Apply(L"ABCDEFG1!", PwdOnly()), L"ABCDEFG1!");    // no lowercase
    CWZ_CHECK_EQ(mask::Apply(L"Abcdefgh!", PwdOnly()), L"Abcdefgh!");    // no digit
    CWZ_CHECK_EQ(mask::Apply(L"Abcdefg1", PwdOnly()), L"Abcdefg1");      // no symbol
    CWZ_CHECK_EQ(mask::Apply(L"My P@ssw0rd", PwdOnly()), L"My P@ssw0rd"); // has a space
}

void PasswordRejectsOverlongText() {
    // 65 chars with every class is a paragraph, not a password.
    const std::wstring longish = L"Aa1!" + std::wstring(61, L'x');
    CWZ_CHECK_EQ(longish.size(), static_cast<size_t>(65));
    CWZ_CHECK_EQ(mask::Apply(longish, PwdOnly()), longish);
}

void PasswordTrimsSurroundingWhitespace() {
    // A trailing newline (very common on a copied password) must not defeat the
    // heuristic; the token is trimmed, judged, and masked.
    CWZ_CHECK_EQ(mask::Apply(L"Abcdef1!\n", PwdOnly()), L"Abc****f1!");
}

void PasswordIsCaseSensitive() {
    // The heuristic needs an uppercase letter, so a pre-lowercased string is
    // NOT flagged. This is the contract that forces searchText to mask before
    // it lowercases — lowercasing first would hide every password.
    CWZ_CHECK_EQ(mask::Apply(L"MyP@ssw0rd", PwdOnly()), L"MyP****0rd");
    CWZ_CHECK_EQ(mask::Apply(L"myp@ssw0rd", PwdOnly()), L"myp@ssw0rd");
}

// ---------------- Composition ----------------

void MasksEverySpanInOneText() {
    // Two phones in one line, each masked, prose untouched.
    CWZ_CHECK_EQ(mask::Apply(L"Call 13812345678 or 13987654321", PhoneOnly()),
                 L"Call 138****678 or 139****321");
}

void MultipleScannerTypesCooperate() {
    CWZ_CHECK_EQ(mask::Apply(L"alice.smith@gmail.com 13812345678", AllOn()),
                 L"ali****ith@gmail.com 138****678");
}

void OrdinaryTextIsUnchanged() {
    const std::wstring prose = L"The quick brown fox jumps over 13 lazy dogs.";
    CWZ_CHECK_EQ(mask::Apply(prose, AllOn()), prose);
    CWZ_CHECK_EQ(mask::Apply(L"hello world", AllOn()), L"hello world");
}

}  // namespace

void RunMaskTests() {
    CWZ_RUN(DefaultsMatchThePlan);
    CWZ_RUN(AnyEnabledReflectsConfig);
    CWZ_RUN(AllDisabledReturnsInputUnchanged);

    CWZ_RUN(TooLongTextIsSkipped);
    CWZ_RUN(DataUriIsSkipped);

    CWZ_RUN(EmailMasksLocalPartKeepsDomain);
    CWZ_RUN(EmailInsideSentenceIsMasked);
    CWZ_RUN(EmailNeedsValidTld);
    CWZ_RUN(EmailOffByDefault);

    CWZ_RUN(PhoneMasksElevenDigits);
    CWZ_RUN(PhoneToleratesCountryCodeAndSeparators);
    CWZ_RUN(PhoneRejectsWhenInsideLongerNumber);
    CWZ_RUN(PhoneRejectsWrongPrefix);

    CWZ_RUN(IdCardMasksValidNumber);
    CWZ_RUN(IdCardRejectsBadChecksum);
    CWZ_RUN(IdCardRejectsImpossibleDate);
    CWZ_RUN(IdCardRequiresWordBoundary);
    CWZ_RUN(IdCardOffSwitchLeavesItAlone);

    CWZ_RUN(ApiKeyMasksKnownPrefix);
    CWZ_RUN(ApiKeyPrefixIsCaseInsensitive);
    CWZ_RUN(ApiKeyRejectsShortTail);
    CWZ_RUN(ApiKeyRequiresLeftBoundary);
    CWZ_RUN(ApiKeyOffByDefault);

    CWZ_RUN(PasswordMasksWholeToken);
    CWZ_RUN(PasswordRejectsNearMisses);
    CWZ_RUN(PasswordRejectsOverlongText);
    CWZ_RUN(PasswordTrimsSurroundingWhitespace);
    CWZ_RUN(PasswordIsCaseSensitive);

    CWZ_RUN(MasksEverySpanInOneText);
    CWZ_RUN(MultipleScannerTypesCooperate);
    CWZ_RUN(OrdinaryTextIsUnchanged);
}
