#pragma once

/// \file fluent_scene.hpp
/// \brief Umbrella header — include this and draw.
///
/// ```cpp
/// #include <fluent_scene/fluent_scene.hpp>
/// using namespace fluent_scene;
///
/// Stage stage(1920, 1080);
/// stage.text("こんにちは", {40, 40}).size(48).shadow();
///
/// CpuRenderer renderer;
/// const Surface& frame = renderer.render(stage, 0.0f);
/// ```
///
/// fluent_scene is the runtime layer tree of Fluent Vision (§1 of
/// docs/design/fluent_scene.ja.md): Scene (declarative documents) compiles
/// into a Stage (this library), which renders into a Surface.

#include "fluent_scene/animation.hpp"
#include "fluent_scene/content.hpp"
#include "fluent_scene/cpu_renderer.hpp"
#include "fluent_scene/effects.hpp"
#include "fluent_scene/filters.hpp"
#include "fluent_scene/geometry.hpp"
#include "fluent_scene/layer.hpp"
#include "fluent_scene/renderer.hpp"
#include "fluent_scene/stage.hpp"
#include "fluent_scene/surface.hpp"
#include "fluent_scene/transaction.hpp"
#include "fluent_scene/types.hpp"
#include "fluent_scene/ui.hpp"
