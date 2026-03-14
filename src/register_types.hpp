// =============================================================
// register_types.hpp  —  GDExtension init/deinit declarations
// =============================================================
#pragma once
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void initialize_polylang(ModuleInitializationLevel p_level);
void uninitialize_polylang(ModuleInitializationLevel p_level);
