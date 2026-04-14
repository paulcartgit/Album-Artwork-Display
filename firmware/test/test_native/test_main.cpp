// Native unit tests — runs on macOS, no hardware needed.
// Tests: dithering, XML parsing, URL encoding

// NATIVE_TEST already defined via build_flags
#include <unity.h>
#include "config.h"
#include "xml_utils.h"
#include "url_utils.h"
#include "dither.h"

// Include the implementation directly for native test builds
// (PlatformIO test runner doesn't link src/ objects for native env)
#include "dither.cpp"

// ═══════════════════════════════════════════════════════════
// Dithering tests
// ═══════════════════════════════════════════════════════════

void test_dither_solid_black(void) {
    const int W = 4, H = 4;
    uint8_t rgb[W * H * 3];
    memset(rgb, 0, sizeof(rgb)); // all black

    uint8_t packed[W * H / 2];
    memset(packed, 0xFF, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    // Every pixel should map to palette index 0 (black)
    for (int i = 0; i < W * H / 2; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00, packed[i]);
    }
}

void test_dither_solid_white(void) {
    const int W = 4, H = 4;
    uint8_t rgb[W * H * 3];
    memset(rgb, 0xFF, sizeof(rgb)); // all white

    uint8_t packed[W * H / 2];
    memset(packed, 0, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    // Every pixel should map to palette index 1 (white) → 0x11
    for (int i = 0; i < W * H / 2; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x11, packed[i]);
    }
}

void test_dither_solid_red(void) {
    const int W = 2, H = 2;
    // Fill with the exact red palette value: 0xA0, 0x20, 0x20
    uint8_t rgb[W * H * 3];
    for (int i = 0; i < W * H; i++) {
        rgb[i * 3]     = 0xA0;
        rgb[i * 3 + 1] = 0x20;
        rgb[i * 3 + 2] = 0x20;
    }

    uint8_t packed[W * H / 2];
    memset(packed, 0, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    // Exact palette match → index 4 (red), no error diffusion
    // packed[0] = (4 << 4) | 4 = 0x44
    TEST_ASSERT_EQUAL_HEX8(0x44, packed[0]);
    TEST_ASSERT_EQUAL_HEX8(0x44, packed[1]);
}

void test_dither_pixel_packing(void) {
    // Verify high nibble = even pixel, low nibble = odd pixel
    const int W = 2, H = 1;
    uint8_t rgb[W * H * 3];
    // Pixel 0: black (index 0), Pixel 1: white (index 1)
    rgb[0] = 0; rgb[1] = 0; rgb[2] = 0;       // black
    rgb[3] = 255; rgb[4] = 255; rgb[5] = 255;  // white

    uint8_t packed[1] = {0};
    ditherFloydSteinberg(rgb, packed, W, H);

    TEST_ASSERT_EQUAL_HEX8(0x01, packed[0]); // high=0(black), low=1(white)
}

void test_dither_output_size(void) {
    // 8x4 image → 16 pixels → 8 packed bytes
    const int W = 8, H = 4;
    uint8_t rgb[W * H * 3];
    memset(rgb, 128, sizeof(rgb)); // grey — will dither to mix

    uint8_t packed[W * H / 2];
    memset(packed, 0xFF, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    // Just verify it didn't crash and all indices are valid (0-6)
    for (int i = 0; i < W * H / 2; i++) {
        uint8_t hi = (packed[i] >> 4) & 0x0F;
        uint8_t lo = packed[i] & 0x0F;
        TEST_ASSERT_LESS_THAN(7, hi);
        TEST_ASSERT_LESS_THAN(7, lo);
    }
}

// ═══════════════════════════════════════════════════════════
// XML parsing tests
// ═══════════════════════════════════════════════════════════

void test_extractTag_basic(void) {
    String xml = "<root><title>Hello World</title></root>";
    String result = extractTag(xml, "title");
    TEST_ASSERT_TRUE(result == "Hello World");
}

void test_extractTag_nested(void) {
    String xml = "<a><b><c>deep</c></b></a>";
    TEST_ASSERT_TRUE(extractTag(xml, "c") == "deep");
}

void test_extractTag_missing(void) {
    String xml = "<root><title>Hi</title></root>";
    String result = extractTag(xml, "artist");
    TEST_ASSERT_TRUE(result.isEmpty());
}

void test_extractTag_empty_value(void) {
    String xml = "<root><title></title></root>";
    String result = extractTag(xml, "title");
    TEST_ASSERT_TRUE(result.isEmpty());
}

void test_extractTag_sonos_response(void) {
    // Simulated Sonos GetPositionInfo response fragment
    String xml =
        "<TrackURI>x-sonos-spotify:spotify:track:abc123</TrackURI>"
        "<TrackMetaData>&lt;dc:title&gt;Bohemian Rhapsody&lt;/dc:title&gt;"
        "&lt;dc:creator&gt;Queen&lt;/dc:creator&gt;</TrackMetaData>";

    String uri = extractTag(xml, "TrackURI");
    TEST_ASSERT_TRUE(uri == "x-sonos-spotify:spotify:track:abc123");

    String metaRaw = extractTag(xml, "TrackMetaData");
    String meta = decodeXmlEntities(metaRaw);

    TEST_ASSERT_TRUE(extractTag(meta, "dc:title") == "Bohemian Rhapsody");
    TEST_ASSERT_TRUE(extractTag(meta, "dc:creator") == "Queen");
}

void test_extractTag_line_in_detection(void) {
    String xml = "<TrackURI>x-rincon-stream:RINCON_123456</TrackURI>";
    String uri = extractTag(xml, "TrackURI");
    TEST_ASSERT_TRUE(uri.startsWith("x-rincon-stream:"));
}

void test_decodeXmlEntities_all(void) {
    String input = "&lt;tag attr=&quot;val&quot;&gt;A &amp; B&apos;s&lt;/tag&gt;";
    String result = decodeXmlEntities(input);
    TEST_ASSERT_TRUE(result == "<tag attr=\"val\">A & B's</tag>");
}

void test_decodeXmlEntities_no_entities(void) {
    String input = "plain text";
    String result = decodeXmlEntities(input);
    TEST_ASSERT_TRUE(result == "plain text");
}

// ═══════════════════════════════════════════════════════════
// URL encoding tests
// ═══════════════════════════════════════════════════════════

void test_urlEncode_plain(void) {
    String result = urlEncode("hello");
    TEST_ASSERT_TRUE(result == "hello");
}

void test_urlEncode_spaces(void) {
    String result = urlEncode("hello world");
    TEST_ASSERT_TRUE(result == "hello%20world");
}

void test_urlEncode_special_chars(void) {
    String result = urlEncode("artist:Queen track:We Will Rock You");
    TEST_ASSERT_TRUE(result == "artist%3AQueen%20track%3AWe%20Will%20Rock%20You");
}

void test_urlEncode_safe_chars(void) {
    String result = urlEncode("a-b_c.d~e");
    TEST_ASSERT_TRUE(result == "a-b_c.d~e");
}

void test_urlEncode_unicode_bytes(void) {
    // "café" in UTF-8: 63 61 66 C3 A9
    String result = urlEncode("caf\xc3\xa9");
    TEST_ASSERT_TRUE(result == "caf%C3%A9");
}

void test_urlEncode_empty(void) {
    String result = urlEncode("");
    TEST_ASSERT_TRUE(result.isEmpty());
}

// ═══════════════════════════════════════════════════════════
// Test runner
// ═══════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Dithering
    RUN_TEST(test_dither_solid_black);
    RUN_TEST(test_dither_solid_white);
    RUN_TEST(test_dither_solid_red);
    RUN_TEST(test_dither_pixel_packing);
    RUN_TEST(test_dither_output_size);

    // XML parsing
    RUN_TEST(test_extractTag_basic);
    RUN_TEST(test_extractTag_nested);
    RUN_TEST(test_extractTag_missing);
    RUN_TEST(test_extractTag_empty_value);
    RUN_TEST(test_extractTag_sonos_response);
    RUN_TEST(test_extractTag_line_in_detection);
    RUN_TEST(test_decodeXmlEntities_all);
    RUN_TEST(test_decodeXmlEntities_no_entities);

    // URL encoding
    RUN_TEST(test_urlEncode_plain);
    RUN_TEST(test_urlEncode_spaces);
    RUN_TEST(test_urlEncode_special_chars);
    RUN_TEST(test_urlEncode_safe_chars);
    RUN_TEST(test_urlEncode_unicode_bytes);
    RUN_TEST(test_urlEncode_empty);

    return UNITY_END();
}
