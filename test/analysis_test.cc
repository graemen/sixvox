// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Accuracy tests of the shared analysis modules (vox/common/analysis) against
// the ground truth of the synthetic speech generator.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "vox/common/analysis/segmenter.h"
#include "vox/common/analysis/vuv.h"
#include "vox/common/analysis/yin.h"
#include "vox/common/dsp/half_rate.h"
#include "vox/common/buffer/speech_buffer.h"
#include "vox/test/test_signals.h"
#include "vox/test/test_util.h"

using namespace vox;
using namespace vox_test;

static const float kSr = 24000.0f;

static void TestSegmenter(int sensitivity, bool verbose) {
  printf("segmenter (sensitivity %d)\n", sensitivity);
  SpeechLikeOptions o = DefaultSpeechOptions(40.0f, kSr);
  GroundTruth gt;
  std::vector<float> x = SpeechLike(o, &gt);
  static Segmenter s;
  s.Init(kSr);
  s.set_sensitivity(sensitivity);
  for (size_t i = 0; i < x.size(); ++i) s.Process(x[i], uint32_t(i));

  std::vector<size_t> ref_syl, ref_word, det_syl, det_word, det_slice;
  for (const Span& sp : gt.syllables) ref_syl.push_back(sp.start);
  for (const Span& sp : gt.words) ref_word.push_back(sp.start);
  const UnitTable<kMaxUnits>& syl = s.table(UNIT_LEVEL_SYLLABLE);
  const UnitTable<kMaxUnits>& word = s.table(UNIT_LEVEL_WORD);
  const UnitTable<kMaxUnits>& slice = s.table(UNIT_LEVEL_ONSET_SLICE);
  for (int i = 0; i < syl.size(); ++i) det_syl.push_back(syl.FromOldest(i).start);
  for (int i = 0; i < word.size(); ++i) det_word.push_back(word.FromOldest(i).start);
  for (int i = 0; i < slice.size(); ++i) det_slice.push_back(slice.FromOldest(i).start);
  const size_t tol = size_t(0.040f * kSr);
  const float syl_recall = BoundaryRecall(ref_syl, det_syl, tol);
  const float syl_precision = BoundaryPrecision(ref_syl, det_syl, tol);
  const float word_recall = BoundaryRecall(ref_word, det_word, tol);
  const float word_precision = BoundaryPrecision(ref_word, det_word, tol);
  const float onset_recall = BoundaryRecall(ref_syl, det_slice, tol);
  if (verbose) {
    printf("  reference: %zu syllables, %zu words; detected: %zu syllables, "
           "%zu words, %zu slices\n", ref_syl.size(), ref_word.size(),
           det_syl.size(), det_word.size(), det_slice.size());
  }
  if (sensitivity == 1) {
    Check(syl_recall >= 0.80f, "syllable starts found (+/-40 ms)", "%.0f%%",
          100 * syl_recall);
    Check(syl_precision >= 0.80f, "syllable starts correct (+/-40 ms)", "%.0f%%",
          100 * syl_precision);
    Check(word_recall >= 0.90f, "word starts found (+/-40 ms)", "%.0f%%",
          100 * word_recall);
    Check(word_precision >= 0.90f, "word starts correct (+/-40 ms)", "%.0f%%",
          100 * word_precision);
    Check(onset_recall >= 0.80f, "onset slices at syllable starts", "%.0f%%",
          100 * onset_recall);
  } else {
    Info("syllables found / correct", "%.0f%% / %.0f%%", 100 * syl_recall,
         100 * syl_precision);
    Info("words found / correct", "%.0f%% / %.0f%%", 100 * word_recall,
         100 * word_precision);
  }
}

static void TestYin(float f0_min, float f0_max, float noise_db, const char* label) {
  printf("YIN (%s)\n", label);
  SpeechLikeOptions o = DefaultSpeechOptions(30.0f, kSr);
  o.f0_min = f0_min;
  o.f0_max = f0_max;
  o.noise_db = noise_db;
  o.seed = 99;
  GroundTruth gt;
  std::vector<float> x = SpeechLike(o, &gt);
  static Yin yin;
  yin.Init(12000.0f);
  Decimator2 decimator;
  decimator.Init();
  size_t voiced = 0, gross = 0, unvoiced = 0, false_voiced = 0;
  double cents = 0.0;
  float y[3];
  for (size_t i = 0; i + 6 <= x.size(); i += 6) {
    decimator.Process(&x[i], y, 6);
    if (!yin.Process(y, 3)) continue;
    // Centre of the analysed window, in 24 kHz samples (+ decimator delay).
    const long centre = long(i + 6) - 2 * yin.latency() - 24;
    if (centre < 0) continue;
    // Only judge frames well inside a vowel or well inside silence/noise.
    bool inside_vowel = false;
    for (const Span& v : gt.vowels) {
      if (size_t(centre) > v.start + 720 && size_t(centre) + 720 < v.end) {
        inside_vowel = true;
        break;
      }
    }
    const bool voiced_frame = yin.aperiodicity() < 0.2f;
    if (inside_vowel) {
      ++voiced;
      const float truth = gt.f0[centre];
      const float ratio = yin.f0() / truth;
      if (fabsf(ratio - 1.0f) > 0.2f || !voiced_frame) {
        ++gross;
      } else {
        cents += fabs(1200.0 * log2(ratio));
      }
    } else if (gt.label[centre] == LABEL_SILENCE || gt.label[centre] == LABEL_UNVOICED) {
      bool near_vowel = false;
      for (const Span& v : gt.vowels) {
        if (size_t(centre) + 1200 > v.start && size_t(centre) < v.end + 1200) near_vowel = true;
      }
      if (near_vowel) continue;
      ++unvoiced;
      false_voiced += voiced_frame;
    }
  }
  const float gross_rate = float(gross) / float(voiced);
  const float mean_cents = float(cents / double(voiced - gross));
  Check(gross_rate < 0.03f, "gross F0 errors in vowels (> 20%)", "%.1f%% of %zu frames",
        100 * gross_rate, voiced);
  Check(mean_cents < 20.0f, "fine F0 error", "%.1f cents", mean_cents);
  Check(float(false_voiced) / float(unvoiced) < 0.05f, "noise / silence not voiced",
        "%.1f%% of %zu frames", 100.0f * false_voiced / unvoiced, unvoiced);
}

static void TestVuv(float noise_db, int preset, const char* label, bool check) {
  printf("voiced/unvoiced classifier (%s)\n", label);
  SpeechLikeOptions o = DefaultSpeechOptions(30.0f, kSr);
  o.noise_db = noise_db;
  o.seed = 123;
  GroundTruth gt;
  std::vector<float> x = SpeechLike(o, &gt);
  static Yin yin;
  yin.Init(12000.0f);
  static VuvClassifier c;
  c.Init(kSr);
  c.set_preset(preset);
  Decimator2 decimator;
  decimator.Init();
  float y[3];
  size_t total = 0, correct = 0;
  size_t confusion[3][3] = { { 0 } };
  for (size_t i = 0; i + 6 <= x.size(); i += 6) {
    decimator.Process(&x[i], y, 6);
    if (yin.Process(y, 3)) c.set_aperiodicity(yin.aperiodicity());
    for (size_t j = 0; j < 6; ++j) {
      if (!c.Process(x[i + j])) continue;
      const long centre = long(i + j) - c.latency() + 60;
      if (centre < 240 || size_t(centre) + 240 >= x.size()) continue;
      // Skip frames within 10 ms of a label change.
      const uint8_t l = gt.label[centre];
      if (gt.label[centre - 240] != l || gt.label[centre + 240] != l) continue;
      const int truth = l == LABEL_SILENCE ? 0 : (l == LABEL_VOICED ? 1 : 2);
      const VuvClass k = c.vuv_class();
      const int got = k == VUV_SILENCE ? 0 : (k == VUV_VOICED ? 1 : 2);
      ++confusion[truth][got];
      ++total;
      correct += truth == got;
    }
  }
  const float accuracy = float(correct) / float(total);
  if (check) {
    Check(accuracy >= 0.90f, "frame accuracy (silence/voiced/unvoiced)",
          "%.1f%% of %zu frames", 100 * accuracy, total);
  } else {
    Info("frame accuracy (silence/voiced/unvoiced)", "%.1f%% of %zu frames",
         100 * accuracy, total);
  }
  printf("         truth\\got  silence  voiced  unvoiced\n");
  const char* names[3] = { "silence ", "voiced  ", "unvoiced" };
  for (int t = 0; t < 3; ++t) {
    printf("         %s %7zu %7zu %9zu\n", names[t], confusion[t][0],
           confusion[t][1], confusion[t][2]);
  }
}

int main(int argc, char** argv) {
  const bool verbose = argc > 1 && !strcmp(argv[1], "-v");
  TestSegmenter(1, verbose);
  TestSegmenter(0, verbose);
  TestSegmenter(2, verbose);
  TestYin(90.0f, 220.0f, -66.0f, "male/female range");
  TestYin(180.0f, 400.0f, -66.0f, "high voices");
  TestYin(70.0f, 160.0f, -40.0f, "low voices, -40 dB noise");
  TestVuv(-66.0f, VUV_PRESET_SPEECH, "clean", true);
  TestVuv(-40.0f, VUV_PRESET_SPEECH, "-40 dB noise", true);
  TestVuv(-66.0f, VUV_PRESET_PHONE, "phone preset on clean speech", false);
  return Summary();
}
