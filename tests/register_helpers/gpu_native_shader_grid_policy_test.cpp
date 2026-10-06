#include <cmath>
#include <iostream>
#include <string>
#include <rex/graphics/pipeline/render_target/native_shader_scale_policy.h>

int main() {
  using namespace rex::graphics::render_target::native_shader_scale_policy;
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; passed = false; }
  };
  const std::string seed = "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6";
  Rules rules;
  check(Parse("", rules) && rules.count == 0, "empty policy must leave scaling unchanged");
  check(Parse(seed, rules) && rules.count == 1, "verified annotation must parse");
  const Rule rule = rules.entries[0];
  check(RequiresNativeRasterization(rule, 0), "data tables retain native evaluation");
  check(Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 0, 15),
        "matching single-sample data pass must keep its native grid");
  check(!Matches(rule, 1, rule.pixel_hash, true, 324, 18, 6, 0, 15), "other VS must be isolated");
  check(!Matches(rule, rule.vertex_hash, 1, true, 324, 18, 6, 0, 15), "other PS must be isolated");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 1280, 720, 6, 0, 15),
        "full-screen imagery using a copy shader must remain scaled");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 26, 0, 15),
        "other encoding must not match the source contract");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, false, 324, 18, 6, 0, 15),
        "cube/array/3D resources must not match");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 1, 15),
        "MSAA geometry must remain outside the data-pass policy");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 0, 7),
        "partial-channel writes must not match");
  check(!Matches(rule, rule.vertex_hash, rule.pixel_hash, true, 324, 18, 6, 0, 255),
        "MRT writes must not match");
  for (const auto& invalid : {seed + ";", seed + ";" + seed,
      std::string("X:1:1:324:18:6"), std::string("0:1:1:324:18:6"),
      std::string("1:2:32:324:18:6"), std::string("1:2:1:0:18:6"),
      std::string("1:2:1:324:18:64"), std::string("1:2:1:324:18:6:7"),
      std::string("1:2:1:324:18"), std::string("1:2:1:324:18:6 ")}) {
    check(!Parse(invalid, rules) && rules.count == 0,
          "malformed/duplicate annotations must fail atomically");
  }
  std::string many;
  for (int i = 1; i <= 33; ++i) {
    if (i > 1) many += ';';
    many += "1:2:1:" + std::to_string(i) + ":18:6";
  }
  check(!Parse(many, rules) && rules.count == 0, "policy must enforce its pass-count bound");
  check(!Parse(std::string(4097, '1'), rules), "policy text must be bounded");

  const std::string filter = "1:2:0:1280:720:26:filter";
  check(Parse(filter, rules) && rules.count == 1 && rules.entries[0].image_filter,
        "image-filter mode requires an explicit annotation");
  const auto filter_rule = rules.entries[0];
  check(RequiresNativeRasterization(filter_rule, 0), "old filter policy remains compatible");
  check(RequiresNativeRasterization(filter_rule, 2), ":filter renders 4-sample passes native too");
  check(Parse("1:2:0:1280:720:26:filter_scaled", rules) && rules.count == 1 &&
        rules.entries[0].image_filter && rules.entries[0].scaled_filter_output,
        "scaled filter is explicit; not inferred from shader dimensions");
  check(!RequiresNativeRasterization(rules.entries[0], 0) &&
            !RequiresNativeRasterization(rules.entries[0], 2),
        "source footprint must be independent of image-filter output grid");
  check(Matches(rules.entries[0], 1, 2, true, 1280, 720, 26, 1, 15, 0x18700270),
        "scaled filter shares the guarded depth-disabled image contract");
  check(!Matches(rules.entries[0], 1, 2, true, 1280, 720, 26, 1, 15, 0x18700272),
        "scaled filter must not reclassify depth work");
  check(!Parse(filter + ";1:2:0:1280:720:26:filter_scaled", rules),
        "contradictory evaluation-grid rules fail atomically");
  check(!Parse("1:2:0:1280:720:26:filter_scaled:filter", rules), "stacked modes rejected");
  check(Matches(filter_rule, 1, 2, true, 1280, 720, 26, 1, 15, 0x18700270),
        "depth-disabled 2-sample image filter may retain its native working grid");
  for (uint32_t depth_bit : {1u, 2u, 4u}) {
    check(!Matches(filter_rule, 1, 2, true, 1280, 720, 26, 1, 15, 0x18700270 | depth_bit),
          "filter mode must never reclassify depth/stencil work");
  }
  // The title's bloom runs in 4x MSAA (2x when the game lowers it under load).
  check(Matches(filter_rule, 1, 2, true, 1280, 720, 26, 2, 15, 0x18700270),
        "depth-disabled 4-sample image filter is covered");
  check(!Matches(filter_rule, 1, 2, true, 1280, 720, 26, 3, 15, 0x18700270),
        "sample counts above 4 stay outside the implementation");
  check(!Matches(filter_rule, 1, 2, true, 320, 180, 26, 1, 15),
        "filter mode must match its exact declared source domain");
  check(!Parse(filter + ";1:2:0:1280:720:26", rules),
        "conflicting filter/data annotations must fail atomically");
  check(!Parse("1:2:0:1280:720:26:unknown", rules), "unknown mode must not parse");
  check(FilterSamplingSupported(2, 2, true, 0, true), "verified 2x footprint supported");
  check(!FilterSamplingSupported(1, 1, true, 0, true), "native source path remains unchanged");
  check(FilterSamplingSupported(3, 3, true, 0, true) && FilterSamplingSupported(4, 4, true, 0, true),
        "3x and 4x footprints are exact box reductions (S reads per axis)");
  check(!FilterSamplingSupported(5, 5, true, 0, true), "scales above 4x stay unsupported");
  check(!FilterSamplingSupported(2, 1, true, 0, true), "no guessed asymmetric footprint");
  check(!FilterSamplingSupported(2, 2, false, 0, true), "point/mixed filtering unchanged");
  check(!FilterSamplingSupported(2, 2, true, 1, true), "mipped resources unchanged");
  check(!FilterSamplingSupported(2, 2, true, 0, false), "signed/gamma domains unchanged");
  check(!FilterSamplingSupported(2, 2, true, 0, true, false), "repeat/border modes remain outside this contract");
  check(FilterInstructionSupported(true, 0, 0, false, false),
        "zero guest offset must remain eligible despite host rounding epsilon");
  check(!FilterInstructionSupported(true, 0.5f, 0, false, false), "guest X offset excluded");
  check(!FilterInstructionSupported(true, 0, -0.5f, false, false), "guest Y offset excluded");
  check(!FilterInstructionSupported(false, 0, 0, false, false), "unnormalized coordinates excluded");
  check(!FilterInstructionSupported(true, 0, 0, true, false), "explicit LOD excluded");
  check(!FilterInstructionSupported(true, 0, 0, false, true), "explicit gradients excluded");

  // Image filter options: sample count, explicit image region, tracked native
  // source, and several fetches of one draw.
  check(Parse("1:2:0:1280:720:26:filter_scaled:msaa=4", rules) &&
            rules.entries[0].msaa_samples == 4 &&
            Matches(rules.entries[0], 1, 2, true, 1280, 720, 26, 2, 15, 0x18700270) &&
            !Matches(rules.entries[0], 1, 2, true, 1280, 720, 26, 1, 15, 0x18700270),
        ":msaa= matches only its sample count");
  check(Parse("1:2:0:1280:720:26:filter_scaled:msaa=4;1:2:0:1280:720:26:filter_scaled:msaa=1", rules) &&
            rules.count == 2,
        "the same pass split by sample count is not a duplicate");
  check(Parse("1:2:0:1280:720:26:filter_scaled:region=0,0,160,90", rules) &&
            rules.entries[0].has_region && rules.entries[0].region_right == 160 &&
            rules.entries[0].region_bottom == 90,
        ":region= names the native sub-image");
  check(!Parse("1:2:0:160:90:6:filter:region=0,0,200,90", rules), "a region must fit the texture");
  check(!Parse("1:2:1:324:18:6:region=0,0,10,10", rules), "options need an image filter");
  check(Parse("1:2:0:1280:720:26:filter_scaled:source=native", rules) &&
            rules.entries[0].native_source,
        ":source=native parses");
  check(!Parse("1:2:0:1280:720:26:filter_scaled:source=native:region=0,0,10,10", rules),
        ":source=native and :region= are exclusive");
  check(!Parse("1:2:0:1280:720:26:filter_scaled:source=scaled", rules), "unknown source rejected");
  check(Parse("1:2:0:1280:720:26:filter_scaled;1:2:1:1280:720:26:filter_scaled:source=native", rules) &&
            rules.count == 2,
        "two fetches of one draw may each carry a rule");

  // Synthetic fixture for the normalized-bilinear repeat copy of a lookup table.
  // samples x=-.25,y=-.25 for the first 2x subpixel. The nonadjacent edge
  // entries contaminate black before any tone-map or presentation operation.
  // A native data grid samples exactly the original black entry instead.
  const double last_last = 1;
  const double zero_last = .5;
  const double last_zero = .75;
  const double first_first = 0;
  const double wrapped = last_last * .0625 + zero_last * .1875 +
                         last_zero * .1875 + first_first * .5625;
  check(wrapped > .2 && first_first == 0,
        "supersampling a data grid must not be mistaken for identity sampling");
  {
    // #16: the title's 2x-MSAA frames keep the data rules, not the filters.
    Rules configured;
    check(Parse(seed + ";B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:6:filter;"
                       "EC4685ADB9CCBC13:207D40E674A7C916:0:1280:720:26:filter_scaled:source=native",
                configured) && configured.count == 3,
          "data and filter rules parse");
    const Rules without = WithoutImageFilters(configured);
    check(without.count == configured.count, "rule indices are kept");
    const Rule& data = without.entries[0];
    check(Matches(data, data.vertex_hash, data.pixel_hash, true, 324, 18, 6, 0, 15),
          "the data rule still matches");
    for (uint32_t i = 1; i < 3; ++i) {
      const Rule& filter = without.entries[i];
      const Rule& original = configured.entries[i];
      check(Matches(original, original.vertex_hash, original.pixel_hash, true, 1280, 720,
                    original.format, 1, 15),
            "the configured filter matches a 2x draw");
      bool any = false;
      for (uint32_t width : {1u, 160u, 324u, 1280u, 8192u}) {
        for (uint32_t msaa_log2 = 0; msaa_log2 <= 2; ++msaa_log2) {
          any = any || Matches(filter, filter.vertex_hash, filter.pixel_hash, true, width, 720,
                               filter.format, msaa_log2, 15);
        }
      }
      check(!any && filter.image_filter, "a left-out filter matches no texture");
    }
  }
  if (passed) std::cout << "Native shader data-grid policy passed\n";
  return passed ? 0 : 1;
}
