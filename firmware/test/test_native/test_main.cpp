// Native unit tests — run on the desktop, no hardware needed.
//
// Coverage is deliberately aimed at the logic that has actually broken:
// the dither's palette handling, history eviction/timestamps, the vinyl
// back-off policy, and Sonos XML parsing.

#include <unity.h>
#include "config.h"
#include "xml_utils.h"
#include "backoff.h"
#include "history_policy.h"
#include "dither.h"

// Include the implementation directly for native test builds
// (PlatformIO's test runner doesn't link src/ objects for the native env)
#include "dither.cpp"

void setUp(void) {}
void tearDown(void) {}

// ═══════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════

static uint8_t pixelIndex(const uint8_t* packed, int w, int x, int y) {
    int pi = y * w + x;
    return (pi % 2 == 0) ? (packed[pi / 2] >> 4) : (packed[pi / 2] & 0x0F);
}

static void fillSolid(uint8_t* rgb, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < w * h; i++) {
        rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
    }
}

// ═══════════════════════════════════════════════════════════
// Dithering
// ═══════════════════════════════════════════════════════════

void test_dither_solid_black(void) {
    const int W = 4, H = 4;
    uint8_t rgb[W * H * 3];
    memset(rgb, 0, sizeof(rgb));

    uint8_t packed[W * H / 2];
    memset(packed, 0xFF, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    for (int i = 0; i < W * H / 2; i++) TEST_ASSERT_EQUAL_HEX8(0x00, packed[i]);
}

void test_dither_solid_white(void) {
    const int W = 4, H = 4;
    uint8_t rgb[W * H * 3];
    memset(rgb, 0xFF, sizeof(rgb));

    uint8_t packed[W * H / 2];
    memset(packed, 0, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    for (int i = 0; i < W * H / 2; i++) TEST_ASSERT_EQUAL_HEX8(0x11, packed[i]);
}

// Feeding the dither a colour that IS one of the panel's pigments must return
// that pigment exactly, with no error to diffuse.  This is the regression test
// for matching against idealised RGB cube corners instead of the calibrated
// table: with cube corners, calibrated red (0x9C,0x30,0x2C) landed nearer to
// black than to red.
void test_dither_matches_calibrated_pigments(void) {
    const int W = 4, H = 4;
    for (int c = 0; c < EPD_COLORS; c++) {
        uint8_t rgb[W * H * 3];
        fillSolid(rgb, W, H, PALETTE[c].r, PALETTE[c].g, PALETTE[c].b);

        uint8_t packed[W * H / 2];
        memset(packed, 0xAA, sizeof(packed));
        ditherFloydSteinberg(rgb, packed, W, H);

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                TEST_ASSERT_EQUAL_UINT8(c, pixelIndex(packed, W, x, y));
            }
        }
    }
}

// Purple is not in the palette; the panel can only make it by interleaving red
// and blue.  This is what the virtual-Magenta entry exists to produce, and it
// is the thing most likely to silently regress.
void test_dither_purple_interleaves_red_and_blue(void) {
    const int W = 16, H = 16;
    uint8_t rgb[W * H * 3];
    // Midpoint of calibrated red and blue — the virtual Magenta target
    fillSolid(rgb, W, H,
              (PALETTE[4].r + PALETTE[3].r) / 2,
              (PALETTE[4].g + PALETTE[3].g) / 2,
              (PALETTE[4].b + PALETTE[3].b) / 2);

    uint8_t packed[W * H / 2];
    memset(packed, 0, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    int reds = 0, blues = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t idx = pixelIndex(packed, W, x, y);
            if (idx == 4) reds++;
            if (idx == 3) blues++;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, reds);
    TEST_ASSERT_GREATER_THAN_INT(0, blues);
    // Both pigments should be well represented, not a token handful
    TEST_ASSERT_GREATER_THAN_INT((W * H) / 8, reds);
    TEST_ASSERT_GREATER_THAN_INT((W * H) / 8, blues);
}

// The virtual entries must never reach the panel — only real pigment indices.
void test_dither_emits_only_real_palette_indices(void) {
    const int W = 8, H = 8;
    uint8_t rgb[W * H * 3];
    for (int i = 0; i < W * H; i++) {
        rgb[i * 3]     = (uint8_t)((i * 37) & 0xFF);
        rgb[i * 3 + 1] = (uint8_t)((i * 91) & 0xFF);
        rgb[i * 3 + 2] = (uint8_t)((i * 13) & 0xFF);
    }

    uint8_t packed[W * H / 2];
    memset(packed, 0xFF, sizeof(packed));
    ditherFloydSteinberg(rgb, packed, W, H);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_LESS_THAN_UINT8(EPD_COLORS, pixelIndex(packed, W, x, y));
        }
    }
}

void test_dither_pixel_packing(void) {
    // High nibble = even pixel, low nibble = odd pixel
    const int W = 2, H = 1;
    uint8_t rgb[W * H * 3];
    rgb[0] = 0;   rgb[1] = 0;   rgb[2] = 0;   // black
    rgb[3] = 255; rgb[4] = 255; rgb[5] = 255; // white

    uint8_t packed[1] = {0};
    ditherFloydSteinberg(rgb, packed, W, H);
    TEST_ASSERT_EQUAL_HEX8(0x01, packed[0]);
}

// Profiles must change output, and Natural must remain the documented default.
void test_dither_profiles_differ(void) {
    const int W = 16, H = 16;
    uint8_t rgb[W * H * 3];
    fillSolid(rgb, W, H, 150, 110, 170); // a muted lavender

    uint8_t punchy[W * H / 2], soft[W * H / 2];
    memset(punchy, 0, sizeof(punchy));
    memset(soft, 0, sizeof(soft));

    ditherFloydSteinberg(rgb, punchy, W, H, RENDER_PROFILES[PROFILE_PUNCHY]);
    ditherFloydSteinberg(rgb, soft,   W, H, RENDER_PROFILES[PROFILE_SOFT]);

    TEST_ASSERT_NOT_EQUAL(0, memcmp(punchy, soft, sizeof(punchy)));
}

void test_dither_zero_size_is_safe(void) {
    uint8_t rgb[3] = {0, 0, 0};
    uint8_t packed[1] = {0x5A};
    ditherFloydSteinberg(rgb, packed, 0, 0);
    TEST_ASSERT_EQUAL_HEX8(0x5A, packed[0]); // untouched
}

// ═══════════════════════════════════════════════════════════
// Album-art history policy
// ═══════════════════════════════════════════════════════════

void test_history_prunes_oldest_unpinned(void) {
    HistoryEntryMeta e[4] = {
        {1700000300, false},
        {1700000100, false},  // oldest unpinned
        {1700000200, false},
        {1700000000, true},   // older, but pinned
    };
    TEST_ASSERT_EQUAL_INT(1, historyPruneIndex(e, 4));
}

void test_history_prune_returns_none_when_all_pinned(void) {
    HistoryEntryMeta e[3] = {
        {1700000000, true}, {1700000100, true}, {1700000200, true},
    };
    TEST_ASSERT_EQUAL_INT(-1, historyPruneIndex(e, 3));
}

// Legacy entries carry millis()-derived timestamps.  They must sort as the
// oldest, so they are evicted before anything saved with a real clock.
void test_history_legacy_millis_entries_prune_first(void) {
    HistoryEntryMeta e[3] = {
        {1700000000, false},  // real epoch
        {45231,      false},  // legacy millis() value
        {1700000500, false},
    };
    TEST_ASSERT_EQUAL_INT(1, historyPruneIndex(e, 3));
}

void test_history_timestamp_uses_wall_clock_when_available(void) {
    HistoryEntryMeta e[1] = {{1700000000, false}};
    TEST_ASSERT_EQUAL_UINT32(1700000500,
        historyNextTimestamp(1700000500, e, 1));
}

// This is the reboot bug: without a clock, a new entry must still sort NEWER
// than everything already stored, or the pruner deletes the art we just saved.
void test_history_timestamp_stays_monotonic_without_clock(void) {
    HistoryEntryMeta e[3] = {
        {1700000000, false}, {1700000900, false}, {1700000400, false},
    };
    uint32_t ts = historyNextTimestamp(0, e, 3);
    TEST_ASSERT_EQUAL_UINT32(1700000901, ts);

    // And the newly-written entry must not be the eviction target
    HistoryEntryMeta after[4] = {e[0], e[1], e[2], {ts, false}};
    TEST_ASSERT_NOT_EQUAL(3, historyPruneIndex(after, 4));
}

void test_history_timestamp_on_empty_index(void) {
    TEST_ASSERT_EQUAL_UINT32(1, historyNextTimestamp(0, nullptr, 0));
}

// ═══════════════════════════════════════════════════════════
// Vinyl back-off policy
// ═══════════════════════════════════════════════════════════

void test_backoff_first_cycle_gets_full_retries(void) {
    TEST_ASSERT_EQUAL_INT(3, vinylMaxRetriesFor(0, 3));
}

void test_backoff_escalated_cycles_retry_once(void) {
    TEST_ASSERT_EQUAL_INT(1, vinylMaxRetriesFor(1, 3));
    TEST_ASSERT_EQUAL_INT(1, vinylMaxRetriesFor(9, 3));
}

void test_backoff_cooldown_grows_with_level(void) {
    TEST_ASSERT_EQUAL_UINT32(300000, vinylCooldownMsFor(300000, 0, 1800000));
    TEST_ASSERT_EQUAL_UINT32(600000, vinylCooldownMsFor(300000, 1, 1800000));
    TEST_ASSERT_EQUAL_UINT32(900000, vinylCooldownMsFor(300000, 2, 1800000));
}

void test_backoff_cooldown_is_capped(void) {
    TEST_ASSERT_EQUAL_UINT32(1800000, vinylCooldownMsFor(300000, 20, 1800000));
    // No integer overflow at absurd levels
    TEST_ASSERT_EQUAL_UINT32(1800000, vinylCooldownMsFor(300000, 100000, 1800000));
}

// ═══════════════════════════════════════════════════════════
// XML parsing
// ═══════════════════════════════════════════════════════════

void test_extractTag_basic(void) {
    String xml = "<root><title>Hello World</title></root>";
    TEST_ASSERT_TRUE(extractTag(xml, "title") == "Hello World");
}

void test_extractTag_nested(void) {
    String xml = "<a><b><c>deep</c></b></a>";
    TEST_ASSERT_TRUE(extractTag(xml, "c") == "deep");
}

void test_extractTag_missing(void) {
    String xml = "<root><title>Hi</title></root>";
    TEST_ASSERT_TRUE(extractTag(xml, "artist").isEmpty());
}

void test_extractTag_empty_value(void) {
    String xml = "<root><title></title></root>";
    TEST_ASSERT_TRUE(extractTag(xml, "title").isEmpty());
}

// A tag whose name is a prefix of another must not match the longer one.
void test_extractTag_does_not_match_longer_name(void) {
    String xml = "<r><titleSort>Zzz</titleSort><title>Real</title></r>";
    TEST_ASSERT_TRUE(extractTag(xml, "title") == "Real");
}

// UPnP emits attributes on the elements we read.
void test_extractTag_with_attributes(void) {
    String xml = "<item id=\"1\"><dc:title xmlns:dc=\"x\">Song</dc:title></item>";
    TEST_ASSERT_TRUE(extractTag(xml, "dc:title") == "Song");
}

void test_extractTag_self_closing(void) {
    String xml = "<root><title/></root>";
    TEST_ASSERT_TRUE(extractTag(xml, "title").isEmpty());
}

// DIDL-Lite nests; scoping to <item> stops a container shadowing the track.
void test_extractTagWithin_prefers_item_scope(void) {
    String xml =
        "<DIDL-Lite>"
        "<container><dc:title>My Playlist</dc:title></container>"
        "<item><dc:title>Actual Track</dc:title></item>"
        "</DIDL-Lite>";
    TEST_ASSERT_TRUE(extractTagWithin(xml, "item", "dc:title") == "Actual Track");
}

void test_extractTagWithin_falls_back_when_no_container(void) {
    String xml = "<DIDL-Lite><dc:title>Loose Track</dc:title></DIDL-Lite>";
    TEST_ASSERT_TRUE(extractTagWithin(xml, "item", "dc:title") == "Loose Track");
}

void test_extractTag_sonos_response(void) {
    String xml =
        "<TrackURI>x-sonos-spotify:spotify:track:abc123</TrackURI>"
        "<TrackMetaData>&lt;item&gt;&lt;dc:title&gt;Bohemian Rhapsody&lt;/dc:title&gt;"
        "&lt;dc:creator&gt;Queen&lt;/dc:creator&gt;&lt;/item&gt;</TrackMetaData>";

    TEST_ASSERT_TRUE(extractTag(xml, "TrackURI") == "x-sonos-spotify:spotify:track:abc123");

    String meta = decodeXmlEntities(extractTag(xml, "TrackMetaData"));
    TEST_ASSERT_TRUE(extractTagWithin(meta, "item", "dc:title") == "Bohemian Rhapsody");
    TEST_ASSERT_TRUE(extractTagWithin(meta, "item", "dc:creator") == "Queen");
}

void test_extractTag_line_in_detection(void) {
    String xml = "<TrackURI>x-rincon-stream:RINCON_123456</TrackURI>";
    TEST_ASSERT_TRUE(extractTag(xml, "TrackURI").startsWith("x-rincon-stream:"));
}

void test_decodeXmlEntities_all(void) {
    String input = "&lt;tag attr=&quot;val&quot;&gt;A &amp; B&apos;s&lt;/tag&gt;";
    TEST_ASSERT_TRUE(decodeXmlEntities(input) == "<tag attr=\"val\">A & B's</tag>");
}

// &amp; must be decoded last, or "&amp;lt;" wrongly becomes "<".
void test_decodeXmlEntities_double_escaped_ampersand(void) {
    TEST_ASSERT_TRUE(decodeXmlEntities("&amp;lt;") == "&lt;");
}

void test_decodeXmlEntities_no_entities(void) {
    TEST_ASSERT_TRUE(decodeXmlEntities("plain text") == "plain text");
}

// ═══════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Dithering
    RUN_TEST(test_dither_solid_black);
    RUN_TEST(test_dither_solid_white);
    RUN_TEST(test_dither_matches_calibrated_pigments);
    RUN_TEST(test_dither_purple_interleaves_red_and_blue);
    RUN_TEST(test_dither_emits_only_real_palette_indices);
    RUN_TEST(test_dither_pixel_packing);
    RUN_TEST(test_dither_profiles_differ);
    RUN_TEST(test_dither_zero_size_is_safe);

    // History policy
    RUN_TEST(test_history_prunes_oldest_unpinned);
    RUN_TEST(test_history_prune_returns_none_when_all_pinned);
    RUN_TEST(test_history_legacy_millis_entries_prune_first);
    RUN_TEST(test_history_timestamp_uses_wall_clock_when_available);
    RUN_TEST(test_history_timestamp_stays_monotonic_without_clock);
    RUN_TEST(test_history_timestamp_on_empty_index);

    // Back-off policy
    RUN_TEST(test_backoff_first_cycle_gets_full_retries);
    RUN_TEST(test_backoff_escalated_cycles_retry_once);
    RUN_TEST(test_backoff_cooldown_grows_with_level);
    RUN_TEST(test_backoff_cooldown_is_capped);

    // XML parsing
    RUN_TEST(test_extractTag_basic);
    RUN_TEST(test_extractTag_nested);
    RUN_TEST(test_extractTag_missing);
    RUN_TEST(test_extractTag_empty_value);
    RUN_TEST(test_extractTag_does_not_match_longer_name);
    RUN_TEST(test_extractTag_with_attributes);
    RUN_TEST(test_extractTag_self_closing);
    RUN_TEST(test_extractTagWithin_prefers_item_scope);
    RUN_TEST(test_extractTagWithin_falls_back_when_no_container);
    RUN_TEST(test_extractTag_sonos_response);
    RUN_TEST(test_extractTag_line_in_detection);
    RUN_TEST(test_decodeXmlEntities_all);
    RUN_TEST(test_decodeXmlEntities_double_escaped_ampersand);
    RUN_TEST(test_decodeXmlEntities_no_entities);

    return UNITY_END();
}
