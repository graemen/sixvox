// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// VOX: a Mutable Instruments Beads running the VOX firmwares, in VCV Rack.
//
// The firmware is a VOX core (vox/host): the module's own firmware code
// (panel glue, engines, user interface, settings) compiled for the computer.
// Each module instance loads a private copy of the core file, so instances
// are independent, and any core built from the repository can be loaded
// from the context menu: the firmware being developed is tested here with
// the code that will run on the module.
//
// Audio runs through the core at 48 kHz in 12-frame blocks, as on the
// module; the engine's sample rate is converted on the way in and out.

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <osdialog.h>

#include "plugin.hpp"
#include "AudioBridge.hpp"

#include "vox/host/core_loader.h"

namespace {

#if defined(ARCH_MAC)
const char* kCoreExtension = ".dylib";
#elif defined(ARCH_WIN)
const char* kCoreExtension = ".dll";
#else
const char* kCoreExtension = ".so";
#endif

const char* kDefaultCore = "vox_multi";

// Panel positions (mm from the top-left corner), measured from the Beads
// panel drawing (beads/hardware_design/panel), centred on the 14 HP Rack
// panel (71.12 mm wide: the Beads panel is 70.8 mm).
const float kDx = 0.16f;
inline Vec Mm(float x, float y) { return mm2px(Vec(x + kDx, y)); }

std::string HexEncode(const std::vector<uint8_t>& data) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (uint8_t b : data) {
    s += digits[b >> 4];
    s += digits[b & 15];
  }
  return s;
}

std::vector<uint8_t> HexDecode(const std::string& s) {
  std::vector<uint8_t> data;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    data.push_back(uint8_t(std::stoi(s.substr(i, 2), NULL, 16)));
  }
  return data;
}

}  // namespace

struct Vox : Module {
  enum ParamId {
    TIME_PARAM,
    DENSITY_PARAM,
    PITCH_PARAM,
    SIZE_PARAM,
    SHAPE_PARAM,
    TIME_AMOUNT_PARAM,
    SIZE_AMOUNT_PARAM,
    SHAPE_AMOUNT_PARAM,
    PITCH_AMOUNT_PARAM,
    FEEDBACK_PARAM,
    DRY_WET_PARAM,
    REVERB_PARAM,
    FREEZE_PARAM,
    SEED_PARAM,
    MEDIA_PARAM,
    ASSIGN_PARAM,
    PARAMS_LEN
  };
  enum InputId {
    ASSIGN_INPUT,
    TIME_INPUT,
    SIZE_INPUT,
    SHAPE_INPUT,
    PITCH_INPUT,
    DENSITY_INPUT,
    IN_L_INPUT,
    IN_R_INPUT,
    FREEZE_INPUT,
    SEED_INPUT,
    INPUTS_LEN
  };
  enum OutputId {
    OUT_L_OUTPUT,
    OUT_R_OUTPUT,
    OUTPUTS_LEN
  };
  enum LightId {
    MEDIA_LIGHT,  // R, G, B
    FREEZE_LIGHT = MEDIA_LIGHT + 3,
    SEED_LIGHT,
    VU_LIGHT,
    ASSIGN_1_LIGHT,
    ASSIGN_2_LIGHT,
    ASSIGN_3_LIGHT,
    LIGHTS_LEN
  };

  enum Restart {
    RESTART_NONE,
    RESTART_PLAIN,
    RESTART_HOLD_MEDIA,
    RESTART_HOLD_SEED,
    RESTART_ERASED   // power cycle without first copying the core's settings
  };

  typedef dsp::Frame<2> Frame;

  // The core (swapped under core_mutex, which the audio thread holds while
  // it runs the core) and the settings flash it uses, kept with the patch
  // (under storage_mutex, held only for copies, so that saving the patch
  // never stalls the audio thread on the core).
  std::mutex core_mutex;
  std::unique_ptr<vox_host::Core> core;
  std::mutex storage_mutex;
  std::vector<uint8_t> persistent;
  std::vector<uint8_t> state;
  std::string core_path;
  std::string core_error;

  std::atomic<int> restart_request{RESTART_NONE};
  bool powered = false;
  int hold_button = -1;
  int hold_samples = 0;

  // Sample rate conversion to and from the core's 48 kHz.
  vox_rack::AudioBridge<VOX_CORE_BLOCK_SIZE, VOX_CORE_SAMPLE_RATE> bridge;
  float engine_rate = 48000.0f;
  dsp::SchmittTrigger freeze_trigger, seed_trigger;
  uint8_t gate_bits = 0;

  // Shown on the panel (written by the audio thread).
  std::atomic<int> shown_phase{VOX_PHASE_OFF};
  std::atomic<int> shown_algorithm{0};
  std::atomic<float> shown_cpu{0.0f};
  std::atomic<uint32_t> saves_seen{0};
  char firmware_name[64] = "";
  char algorithm_names[8][24] = {};
  int num_algorithms = 0;

  Vox() {
    config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
    configParam(TIME_PARAM, 0.f, 1.f, 0.f, "Time", "%", 0.f, 100.f);
    configParam(DENSITY_PARAM, 0.f, 1.f, 0.5f, "Density", "%", 0.f, 100.f);
    configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch", "%", 0.f, 200.f, -100.f);
    configParam(SIZE_PARAM, 0.f, 1.f, 0.5f, "Size", "%", 0.f, 100.f);
    configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape", "%", 0.f, 100.f);
    configParam(TIME_AMOUNT_PARAM, -1.f, 1.f, 0.f, "Time CV amount", "%", 0.f, 100.f);
    configParam(SIZE_AMOUNT_PARAM, -1.f, 1.f, 0.f, "Size CV amount", "%", 0.f, 100.f);
    configParam(SHAPE_AMOUNT_PARAM, -1.f, 1.f, 0.f, "Shape CV amount", "%", 0.f, 100.f);
    configParam(PITCH_AMOUNT_PARAM, -1.f, 1.f, 1.f, "Pitch CV amount", "%", 0.f, 100.f);
    configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.f, "Feedback", "%", 0.f, 100.f);
    configParam(DRY_WET_PARAM, 0.f, 1.f, 1.f, "Dry/wet", "%", 0.f, 100.f);
    configParam(REVERB_PARAM, 0.f, 1.f, 0.f, "Reverb", "%", 0.f, 100.f);
    configButton(FREEZE_PARAM, "Freeze");
    configButton(SEED_PARAM, "Seed");
    configButton(MEDIA_PARAM, "Media (sub-mode; hold at restart: choose algorithm)");
    configButton(ASSIGN_PARAM, "Assign");
    configInput(ASSIGN_INPUT, "Assign CV");
    configInput(TIME_INPUT, "Time CV");
    configInput(SIZE_INPUT, "Size CV");
    configInput(SHAPE_INPUT, "Shape CV");
    configInput(PITCH_INPUT, "Pitch CV (1 V/oct)");
    configInput(DENSITY_INPUT, "Density CV");
    configInput(IN_L_INPUT, "Left / voice");
    configInput(IN_R_INPUT, "Right");
    configInput(FREEZE_INPUT, "Freeze gate");
    configInput(SEED_INPUT, "Seed gate / clock");
    configOutput(OUT_L_OUTPUT, "Left");
    configOutput(OUT_R_OUTPUT, "Right / gate");
    configLight(MEDIA_LIGHT, "Media (sub-mode)");
    configLight(FREEZE_LIGHT, "Freeze");
    configLight(SEED_LIGHT, "Seed");
    configLight(VU_LIGHT, "Input level");
    configLight(ASSIGN_1_LIGHT, "Assign: feedback");
    configLight(ASSIGN_2_LIGHT, "Assign: dry/wet");
    configLight(ASSIGN_3_LIGHT, "Assign: reverb");
    LoadCore(CorePath(kDefaultCore));
  }

  ~Vox() {
    std::lock_guard<std::mutex> lock(core_mutex);
    core.reset();
  }

  static std::string CoreDirectory() {
    return asset::plugin(pluginInstance, "cores");
  }

  static std::string CorePath(const std::string& name) {
    return CoreDirectory() + "/" + name + kCoreExtension;
  }

  // Loads a private copy of the core (UI thread), then swaps it in. The
  // settings flash carries over, as when a new firmware is installed.
  bool LoadCore(const std::string& path) {
    std::unique_ptr<vox_host::Core> c(new vox_host::Core);
    const std::string temp = system::getTempDirectory();
    if (!c->Load(path, temp)) {
      core_error = c->error();
      WARN("VOX: %s", core_error.c_str());
      return false;
    }
    std::unique_ptr<vox_host::Core> old;
    {
      std::lock_guard<std::mutex> lock(core_mutex);
      SaveStorage();
      old = std::move(core);
      core = std::move(c);
      core_path = path;
      core_error.clear();
      snprintf(firmware_name, sizeof(firmware_name), "%s", core->firmware_name());
      num_algorithms = std::min(core->num_algorithms(), 8);
      for (int i = 0; i < num_algorithms; ++i) {
        snprintf(algorithm_names[i], sizeof(algorithm_names[i]), "%s",
                 core->algorithm_name(i));
      }
      powered = false;
    }
    return true;
  }

  // Copies the core's settings flash into the module (core_mutex held).
  void SaveStorage() {
    if (!core) return;
    std::vector<uint8_t> p(1024), s(256);
    p.resize(core->get_persistent(p.data(), p.size()));
    s.resize(core->get_state(s.data(), s.size()));
    std::lock_guard<std::mutex> lock(storage_mutex);
    if (!p.empty()) persistent = p;
    if (!s.empty()) state = s;
  }

  void PowerOn(int held_button) {
    {
      std::lock_guard<std::mutex> lock(storage_mutex);
      core->set_storage(persistent.data(), persistent.size(), state.data(), state.size());
    }
    VoxCoreControls c;
    ReadControls(&c);
    for (int i = 0; i < VOX_BUTTON_LAST; ++i) c.button[i] = (i == held_button);
    hold_button = held_button;
    hold_samples = held_button >= 0 ? int(0.6f * engine_rate) : 0;
    core->set_controls(&c);
    core->power_on();
    powered = true;
    bridge.Reset();
  }

  void ReadControls(VoxCoreControls* c) {
    c->pot[VOX_POT_FEEDBACK] = params[FEEDBACK_PARAM].getValue();
    c->pot[VOX_POT_DRY_WET] = params[DRY_WET_PARAM].getValue();
    c->pot[VOX_POT_REVERB] = params[REVERB_PARAM].getValue();
    c->pot[VOX_POT_TIME_AMOUNT] = 0.5f + 0.5f * params[TIME_AMOUNT_PARAM].getValue();
    c->pot[VOX_POT_SIZE_AMOUNT] = 0.5f + 0.5f * params[SIZE_AMOUNT_PARAM].getValue();
    c->pot[VOX_POT_SHAPE_AMOUNT] = 0.5f + 0.5f * params[SHAPE_AMOUNT_PARAM].getValue();
    c->pot[VOX_POT_PITCH_AMOUNT] = 0.5f + 0.5f * params[PITCH_AMOUNT_PARAM].getValue();
    c->pot[VOX_POT_TIME] = params[TIME_PARAM].getValue();
    c->pot[VOX_POT_DENSITY] = params[DENSITY_PARAM].getValue();
    c->pot[VOX_POT_PITCH] = params[PITCH_PARAM].getValue();
    c->pot[VOX_POT_SIZE] = params[SIZE_PARAM].getValue();
    c->pot[VOX_POT_SHAPE] = params[SHAPE_PARAM].getValue();

    c->cv_volts[VOX_CV_ASSIGN] = inputs[ASSIGN_INPUT].getVoltage();
    c->cv_volts[VOX_CV_DENSITY] = inputs[DENSITY_INPUT].getVoltage();
    c->cv_volts[VOX_CV_PITCH] = inputs[PITCH_INPUT].getVoltage();
    c->cv_volts[VOX_CV_TIME] = inputs[TIME_INPUT].getVoltage();
    c->cv_volts[VOX_CV_SIZE] = inputs[SIZE_INPUT].getVoltage();
    c->cv_volts[VOX_CV_SHAPE] = inputs[SHAPE_INPUT].getVoltage();

    c->patched[VOX_JACK_SEED] = inputs[SEED_INPUT].isConnected();
    c->patched[VOX_JACK_TIME_CV] = inputs[TIME_INPUT].isConnected();
    c->patched[VOX_JACK_SIZE_CV] = inputs[SIZE_INPUT].isConnected();
    c->patched[VOX_JACK_SHAPE_CV] = inputs[SHAPE_INPUT].isConnected();
    c->patched[VOX_JACK_PITCH_CV] = inputs[PITCH_INPUT].isConnected();
    c->patched[VOX_JACK_DENSITY_CV] = inputs[DENSITY_INPUT].isConnected();
    c->patched[VOX_JACK_IN_L] = inputs[IN_L_INPUT].isConnected();
    c->patched[VOX_JACK_IN_R] = inputs[IN_R_INPUT].isConnected();

    c->button[VOX_BUTTON_FREEZE] = params[FREEZE_PARAM].getValue() > 0.5f;
    c->button[VOX_BUTTON_SEED] = params[SEED_PARAM].getValue() > 0.5f;
    c->button[VOX_BUTTON_MEDIA] = params[MEDIA_PARAM].getValue() > 0.5f;
    c->button[VOX_BUTTON_ASSIGN] = params[ASSIGN_PARAM].getValue() > 0.5f;
  }

  void RunCoreBlock(const Frame* in, Frame* out) {
    VoxCoreControls c;
    ReadControls(&c);
    if (hold_samples > 0 && hold_button >= 0) c.button[hold_button] = 1;
    core->set_controls(&c);

    float in_l[VOX_CORE_BLOCK_SIZE], in_r[VOX_CORE_BLOCK_SIZE];
    float out_l[VOX_CORE_BLOCK_SIZE], out_r[VOX_CORE_BLOCK_SIZE];
    uint8_t gates[VOX_CORE_BLOCK_SIZE];
    for (int i = 0; i < VOX_CORE_BLOCK_SIZE; ++i) {
      in_l[i] = in[i].samples[0];
      in_r[i] = in[i].samples[1];
      gates[i] = gate_bits;
    }
    core->process(in_l, in_r, gates, out_l, out_r);
    for (int i = 0; i < VOX_CORE_BLOCK_SIZE; ++i) {
      out[i].samples[0] = out_l[i];
      out[i].samples[1] = out_r[i];
    }
  }

  void UpdateLights() {
    VoxCoreStatus s;
    core->get_status(&s);
    for (int i = 0; i < 3; ++i) lights[MEDIA_LIGHT + i].setBrightness(s.led[VOX_LED_MEDIA_R + i]);
    lights[FREEZE_LIGHT].setBrightness(s.led[VOX_LED_FREEZE]);
    lights[SEED_LIGHT].setBrightness(s.led[VOX_LED_SEED]);
    lights[VU_LIGHT].setBrightness(s.led[VOX_LED_VU]);
    lights[ASSIGN_1_LIGHT].setBrightness(s.led[VOX_LED_ASSIGN_1]);
    lights[ASSIGN_2_LIGHT].setBrightness(s.led[VOX_LED_ASSIGN_2]);
    lights[ASSIGN_3_LIGHT].setBrightness(s.led[VOX_LED_ASSIGN_3]);
    shown_phase = s.phase;
    shown_algorithm = s.algorithm;
    shown_cpu = s.cpu_load;
    if (s.settings_saves != saves_seen) {
      saves_seen = s.settings_saves;
      SaveStorage();
    }
  }

  void process(const ProcessArgs& args) override {
    std::unique_lock<std::mutex> lock(core_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !core) {
      outputs[OUT_L_OUTPUT].setVoltage(0.f);
      outputs[OUT_R_OUTPUT].setVoltage(0.f);
      return;
    }
    engine_rate = args.sampleRate;

    const int restart = restart_request.exchange(RESTART_NONE);
    if (!powered || restart != RESTART_NONE) {
      PowerOn(restart == RESTART_HOLD_MEDIA ? VOX_BUTTON_MEDIA
              : restart == RESTART_HOLD_SEED ? VOX_BUTTON_SEED : -1);
    }
    if (hold_samples > 0) --hold_samples;

    // Gates: Schmitt triggers at 0.5 / 1 V.
    freeze_trigger.process(inputs[FREEZE_INPUT].getVoltage(), 0.5f, 1.f);
    seed_trigger.process(inputs[SEED_INPUT].getVoltage(), 0.5f, 1.f);
    gate_bits = (seed_trigger.isHigh() ? VOX_GATE_SEED : 0) |
        (freeze_trigger.isHigh() ? VOX_GATE_FREEZE : 0);

    Frame in;
    in.samples[0] = inputs[IN_L_INPUT].getVoltage();
    in.samples[1] = inputs[IN_R_INPUT].getVoltage();
    bool ran = false;
    const Frame out = bridge.Process(args.sampleRate, in,
        [this](const Frame* i, Frame* o) { RunCoreBlock(i, o); }, &ran);
    if (ran) UpdateLights();

    outputs[OUT_L_OUTPUT].setVoltage(out.samples[0]);
    outputs[OUT_R_OUTPUT].setVoltage(out.samples[1]);
  }

  void onReset(const ResetEvent& e) override {
    Module::onReset(e);
    EraseSettings();
  }

  // Blank settings flash, then a power cycle.
  void EraseSettings() {
    {
      std::lock_guard<std::mutex> lock(storage_mutex);
      persistent.clear();
      state.clear();
    }
    restart_request = RESTART_ERASED;
  }

  json_t* dataToJson() override {
    json_t* root = json_object();
    std::lock_guard<std::mutex> lock(storage_mutex);
    const std::string dir = CoreDirectory() + "/";
    std::string path = core_path;
    if (path.compare(0, dir.size(), dir) == 0) path = path.substr(dir.size());
    json_object_set_new(root, "core", json_string(path.c_str()));
    json_object_set_new(root, "persistent", json_string(HexEncode(persistent).c_str()));
    json_object_set_new(root, "state", json_string(HexEncode(state).c_str()));
    return root;
  }

  void dataFromJson(json_t* root) override {
    json_t* c = json_object_get(root, "core");
    if (c) {
      std::string path = json_string_value(c);
      if (path.find('/') == std::string::npos && path.find('\\') == std::string::npos) {
        path = CoreDirectory() + "/" + path;
      }
      if (path != core_path) LoadCore(path);
    }
    {
      std::lock_guard<std::mutex> lock(storage_mutex);
      json_t* p = json_object_get(root, "persistent");
      json_t* s = json_object_get(root, "state");
      persistent = p ? HexDecode(json_string_value(p)) : std::vector<uint8_t>();
      state = s ? HexDecode(json_string_value(s)) : std::vector<uint8_t>();
    }
    restart_request = RESTART_ERASED;  // boot with the restored settings
  }
};

// --- Panel -------------------------------------------------------------------

struct VoxLabels : widget::TransparentWidget {
  Vox* module = NULL;

  void Text(NVGcontext* vg, float x, float y, const char* s, float size,
            NVGcolor color = nvgRGB(0x30, 0x30, 0x30)) {
    nvgFontSize(vg, size);
    nvgFillColor(vg, color);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    const Vec p = Mm(x, y);
    nvgText(vg, p.x, p.y, s, NULL);
  }

  void draw(const DrawArgs& args) override {
    std::shared_ptr<window::Font> font = APP->window->loadFont(
        asset::system("res/fonts/DejaVuSans.ttf"));
    if (!font) return;
    NVGcontext* vg = args.vg;
    nvgFontFaceId(vg, font->handle);
    const NVGcolor light = nvgRGB(0xee, 0xee, 0xee);

    Text(vg, 35.4f, 6.2f, "VOX", 12.f);
    Text(vg, 8.73f, 24.3f, "FREEZE", 6.f);
    Text(vg, 62.08f, 24.3f, "SEED", 6.f);
    Text(vg, 35.4f, 36.4f, "DENSITY", 6.5f);
    Text(vg, 11.58f, 46.6f, "TIME", 6.5f);
    Text(vg, 59.23f, 46.6f, "PITCH", 6.5f);
    Text(vg, 35.4f, 50.0f, "MEDIA", 5.5f);
    Text(vg, 24.28f, 68.8f, "SIZE", 6.5f);
    Text(vg, 46.53f, 68.8f, "SHAPE", 6.5f);
    Text(vg, 6.78f, 66.1f, "FEEDBACK", 5.f);
    Text(vg, 64.03f, 66.1f, "DRY/WET", 5.f);
    Text(vg, 64.03f, 82.3f, "REVERB", 5.f);
    Text(vg, 6.78f, 81.0f, "ASSIGN", 5.f);

    const float jx[6] = { 6.78f, 18.23f, 29.68f, 41.13f, 52.58f, 64.03f };
    const char* row1[6] = { "ASSIGN", "TIME", "SIZE", "SHAPE", "PITCH", "DENSITY" };
    const char* row2[6] = { "IN L", "IN R", "FREEZE", "SEED", "OUT L", "OUT R" };
    for (int i = 0; i < 6; ++i) {
      Text(vg, jx[i], 91.1f, row1[i], 5.f);
      Text(vg, jx[i], 105.8f, row2[i], 5.f, i >= 4 ? light : nvgRGB(0x30, 0x30, 0x30));
    }

    // Status: firmware, algorithm, selection, load.
    char line[96] = "no firmware loaded";
    char cpu[32] = "";
    if (module) {
      const int phase = module->shown_phase;
      const int a = module->shown_algorithm;
      const char* name = (a >= 0 && a < module->num_algorithms) ? module->algorithm_names[a] : "";
      if (!module->core_error.empty() && module->num_algorithms == 0) {
        snprintf(line, sizeof(line), "cannot load firmware");
      } else if (phase == VOX_PHASE_SELECTING) {
        snprintf(line, sizeof(line), "select: %d %s", a + 1, name);
      } else if (module->num_algorithms > 1) {
        snprintf(line, sizeof(line), "%s: %d %s", module->firmware_name, a + 1, name);
      } else {
        snprintf(line, sizeof(line), "%s", module->firmware_name);
      }
      snprintf(cpu, sizeof(cpu), "core %.1f%%", 100.f * module->shown_cpu);
    } else {
      snprintf(line, sizeof(line), "vox_multi");
    }
    Text(vg, 35.4f, 10.9f, line, 5.5f, nvgRGB(0x60, 0x60, 0x60));
    Text(vg, 35.4f, 121.6f, cpu, 5.f, nvgRGB(0x80, 0x80, 0x80));
  }
};

struct VoxWidget : ModuleWidget {
  explicit VoxWidget(Vox* module) {
    setModule(module);
    setPanel(createPanel(asset::plugin(pluginInstance, "res/Vox.svg")));

    addChild(createWidgetCentered<ScrewSilver>(Mm(7.5f, 3.0f)));
    addChild(createWidgetCentered<ScrewSilver>(Mm(63.38f, 3.0f)));
    addChild(createWidgetCentered<ScrewSilver>(Mm(7.5f, 125.5f)));
    addChild(createWidgetCentered<ScrewSilver>(Mm(63.38f, 125.5f)));

    VoxLabels* labels = new VoxLabels;
    labels->module = module;
    labels->box.size = box.size;
    addChild(labels);

    addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(
        Mm(8.726f, 17.75f), module, Vox::FREEZE_PARAM, Vox::FREEZE_LIGHT));
    addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(
        Mm(62.076f, 17.75f), module, Vox::SEED_PARAM, Vox::SEED_LIGHT));
    addParam(createParamCentered<RoundBigBlackKnob>(Mm(35.395f, 24.1f), module, Vox::DENSITY_PARAM));
    addParam(createParamCentered<RoundLargeBlackKnob>(Mm(11.575f, 38.4f), module, Vox::TIME_PARAM));
    addParam(createParamCentered<RoundLargeBlackKnob>(Mm(59.226f, 38.4f), module, Vox::PITCH_PARAM));
    addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(
        Mm(29.026f, 45.05f), module, Vox::MEDIA_LIGHT));
    addParam(createParamCentered<TL1105>(Mm(35.395f, 45.05f), module, Vox::MEDIA_PARAM));
    addParam(createParamCentered<RoundLargeBlackKnob>(Mm(24.275f, 60.6f), module, Vox::SIZE_PARAM));
    addParam(createParamCentered<RoundLargeBlackKnob>(Mm(46.526f, 60.6f), module, Vox::SHAPE_PARAM));
    addParam(createParamCentered<RoundSmallBlackKnob>(Mm(6.775f, 60.6f), module, Vox::FEEDBACK_PARAM));
    addParam(createParamCentered<RoundSmallBlackKnob>(Mm(64.025f, 60.6f), module, Vox::DRY_WET_PARAM));
    addParam(createParamCentered<RoundSmallBlackKnob>(Mm(64.025f, 76.8f), module, Vox::REVERB_PARAM));
    addParam(createParamCentered<TL1105>(Mm(6.775f, 76.8f), module, Vox::ASSIGN_PARAM));
    addChild(createLightCentered<SmallLight<WhiteLight>>(Mm(6.776f, 69.8f), module, Vox::ASSIGN_1_LIGHT));
    addChild(createLightCentered<SmallLight<WhiteLight>>(Mm(64.025f, 69.8f), module, Vox::ASSIGN_2_LIGHT));
    addChild(createLightCentered<SmallLight<WhiteLight>>(Mm(64.025f, 86.0f), module, Vox::ASSIGN_3_LIGHT));
    addChild(createLightCentered<SmallLight<GreenLight>>(Mm(12.526f, 104.75f), module, Vox::VU_LIGHT));

    const float ax[4] = { 18.225f, 29.676f, 41.126f, 52.576f };
    const int amounts[4] = { Vox::TIME_AMOUNT_PARAM, Vox::SIZE_AMOUNT_PARAM,
                             Vox::SHAPE_AMOUNT_PARAM, Vox::PITCH_AMOUNT_PARAM };
    for (int i = 0; i < 4; ++i) {
      addParam(createParamCentered<Trimpot>(Mm(ax[i], 80.6f), module, amounts[i]));
    }

    const float jx[6] = { 6.776f, 18.226f, 29.676f, 41.126f, 52.575f, 64.025f };
    const int row1[6] = { Vox::ASSIGN_INPUT, Vox::TIME_INPUT, Vox::SIZE_INPUT,
                          Vox::SHAPE_INPUT, Vox::PITCH_INPUT, Vox::DENSITY_INPUT };
    for (int i = 0; i < 6; ++i) {
      addInput(createInputCentered<PJ301MPort>(Mm(jx[i], 96.5f), module, row1[i]));
    }
    const int row2[4] = { Vox::IN_L_INPUT, Vox::IN_R_INPUT, Vox::FREEZE_INPUT, Vox::SEED_INPUT };
    for (int i = 0; i < 4; ++i) {
      addInput(createInputCentered<PJ301MPort>(Mm(jx[i], 111.1f), module, row2[i]));
    }
    addOutput(createOutputCentered<PJ301MPort>(Mm(jx[4], 111.1f), module, Vox::OUT_L_OUTPUT));
    addOutput(createOutputCentered<PJ301MPort>(Mm(jx[5], 111.1f), module, Vox::OUT_R_OUTPUT));
  }

  void appendContextMenu(Menu* menu) override {
    Vox* module = dynamic_cast<Vox*>(this->module);
    if (!module) return;

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel(string::f("Firmware: %s", module->firmware_name)));
    if (!module->core_error.empty()) {
      menu->addChild(createMenuLabel(module->core_error));
    }

    menu->addChild(createSubmenuItem("Load firmware", "", [=](Menu* menu) {
      std::vector<std::string> entries;
      if (system::isDirectory(Vox::CoreDirectory())) {
        entries = system::getEntries(Vox::CoreDirectory());
      }
      std::sort(entries.begin(), entries.end());
      for (const std::string& path : entries) {
        if (system::getExtension(path) != std::string(kCoreExtension)) continue;
        const std::string name = system::getStem(path);
        menu->addChild(createCheckMenuItem(name, "",
            [=]() { return module->core_path == path; },
            [=]() { module->LoadCore(path); }));
      }
      menu->addChild(new MenuSeparator);
      menu->addChild(createMenuItem("Load firmware file…", "", [=]() {
        const std::string dir = Vox::CoreDirectory();
        char* path = osdialog_file(OSDIALOG_OPEN, dir.c_str(), NULL, NULL);
        if (path) {
          module->LoadCore(path);
          std::free(path);
        }
      }));
    }));

    if (module->num_algorithms > 1) {
      menu->addChild(createSubmenuItem("Algorithm", "", [=](Menu* menu) {
        menu->addChild(createMenuLabel("Restart holding MEDIA, turn PITCH, press MEDIA"));
        for (int i = 0; i < module->num_algorithms; ++i) {
          menu->addChild(createMenuLabel(string::f("%d  %s", i + 1, module->algorithm_names[i])));
        }
      }));
    }

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Power cycle"));
    menu->addChild(createMenuItem("Restart", "", [=]() {
      module->restart_request = Vox::RESTART_PLAIN;
    }));
    if (module->num_algorithms > 1) {
      menu->addChild(createMenuItem("Restart holding MEDIA (choose algorithm)", "", [=]() {
        module->restart_request = Vox::RESTART_HOLD_MEDIA;
      }));
    }
    menu->addChild(createMenuItem("Restart holding SEED (recover settings)", "", [=]() {
      module->restart_request = Vox::RESTART_HOLD_SEED;
    }));
    menu->addChild(createMenuItem("Erase settings and restart", "", [=]() {
      module->EraseSettings();
    }));
  }
};

Model* modelVox = createModel<Vox, VoxWidget>("Vox");
