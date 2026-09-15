// tests.h — Suite entry points for clipwiz_tests
//
// One declaration per test_*.cpp. File names mirror the unit under test
// (test_textconv.cpp exercises src/textconv.cpp only), which is the C++
// equivalent of the "tests live next to the pure logic" layout used by zsclip
// and QuickClipboard — same discoverability, no inline-test syntax needed.
#pragma once

void RunTextConvTests();
void RunFilterTests();
void RunBlocklistTests();
void RunMaskTests();
void RunMergeTests();
void RunTransformTests();
void RunPrivacyTests();
void RunStoreTests();
