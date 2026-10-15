// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.

#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
  pluginInstance = p;
  p->addModel(modelVox);
}
