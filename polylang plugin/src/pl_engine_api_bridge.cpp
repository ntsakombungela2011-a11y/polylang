// =============================================================
// pl_engine_api_bridge.cpp  —  PolyLang v6.5
//
// FIX C-6: Sandbox bypass in flush() eliminated.
//   BEFORE: DeferredCall had no 'tier' field. flush() always called
//           pl_engine_call_impl(..., SandboxTier::Trusted), meaning
//           Isolated scripts could queue deferred physics/audio calls
//           that ran as Trusted.
//   AFTER:  DeferredCall stores SandboxTier tier. flush() passes dc.tier.
// =============================================================
#include "pl_engine_api_bridge.hpp"
#include "variant_bridge.hpp"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

namespace polylang {

PLEngineAPIBridge* PLEngineAPIBridge::singleton_ = nullptr;
PLEngineAPIBridge* PLEngineAPIBridge::get_singleton() { return singleton_; }

void PLEngineAPIBridge::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("call_api", "group", "method", "args"),
        &PLEngineAPIBridge::call_api);
}

PLEngineAPIBridge::PLEngineAPIBridge() {
    register_all_handlers();
}

// ── Sandbox gate ──────────────────────────────────────────────

bool PLEngineAPIBridge::group_allowed(const std::string& group, SandboxTier tier) {
    if (tier == SandboxTier::Quarantined) return false;
    if (tier == SandboxTier::Isolated)
        return (group == "time" || group == "input");
    return true;
}

// ── Handler registration ──────────────────────────────────────

void PLEngineAPIBridge::register_all_handlers() {
    using std::placeholders::_1;
    using std::placeholders::_2;
    using std::placeholders::_3;

    handlers_["physics"]["raycast"]           = std::bind(&PLEngineAPIBridge::h_physics_raycast,          this,_1,_2,_3);
    handlers_["physics"]["get_gravity"]       = std::bind(&PLEngineAPIBridge::h_physics_get_gravity,      this,_1,_2,_3);
    handlers_["audio"]["play"]                = std::bind(&PLEngineAPIBridge::h_audio_play,                this,_1,_2,_3);
    handlers_["audio"]["stop"]                = std::bind(&PLEngineAPIBridge::h_audio_stop,                this,_1,_2,_3);
    handlers_["input"]["is_action_pressed"]   = std::bind(&PLEngineAPIBridge::h_input_is_action_pressed,  this,_1,_2,_3);
    handlers_["input"]["get_axis"]            = std::bind(&PLEngineAPIBridge::h_input_get_axis,            this,_1,_2,_3);
    handlers_["input"]["get_vector"]          = std::bind(&PLEngineAPIBridge::h_input_get_vector,          this,_1,_2,_3);
    handlers_["scene"]["instantiate"]         = std::bind(&PLEngineAPIBridge::h_scene_instantiate,         this,_1,_2,_3);
    handlers_["scene"]["get_node"]            = std::bind(&PLEngineAPIBridge::h_scene_get_node,            this,_1,_2,_3);
    handlers_["scene"]["queue_free"]          = std::bind(&PLEngineAPIBridge::h_scene_queue_free,          this,_1,_2,_3);
    handlers_["scene"]["add_child"]           = std::bind(&PLEngineAPIBridge::h_scene_add_child,           this,_1,_2,_3);
    handlers_["time"]["get_ticks_msec"]       = std::bind(&PLEngineAPIBridge::h_time_get_ticks_msec,       this,_1,_2,_3);
    handlers_["time"]["get_unix"]             = std::bind(&PLEngineAPIBridge::h_time_get_unix,             this,_1,_2,_3);
}

// ── Main dispatch ─────────────────────────────────────────────

/*static*/ int PLEngineAPIBridge::pl_engine_call_impl(
        const char* api_group, const char* method,
        PLValue* args, int32_t argc, PLValue* ret_out, SandboxTier tier) {
    pl_value_init(ret_out);
    if (!api_group || !method) return PL_ERR_GENERIC;

    std::string g(api_group), m(method);
    if (!group_allowed(g, tier)) {
        ERR_PRINT(("[PolyLang/EngineAPI] Access denied: " + g + "." + m).c_str());
        return PL_ERR_SANDBOX;
    }

    auto* self = PLEngineAPIBridge::get_singleton();
    if (!self) return PL_ERR_GENERIC;

    auto git = self->handlers_.find(g);
    if (git == self->handlers_.end()) {
        ERR_PRINT(("[PolyLang/EngineAPI] Unknown group: " + g).c_str());
        return PL_ERR_GENERIC;
    }
    auto mit = git->second.find(m);
    if (mit == git->second.end()) {
        ERR_PRINT(("[PolyLang/EngineAPI] Unknown method: " + g + "." + m).c_str());
        return PL_ERR_METHOD_NOT_FOUND;
    }

    return mit->second(args, argc, ret_out);
}

/*static*/ int PLEngineAPIBridge::pl_engine_call_trusted(
        const char* g, const char* m, PLValue* a, int32_t n, PLValue* r) {
    return pl_engine_call_impl(g, m, a, n, r, SandboxTier::Trusted);
}

godot::Dictionary PLEngineAPIBridge::call_api(const godot::String& group,
                                               const godot::String& method,
                                               const godot::Array&  gd_args) {
    std::vector<PLValue> args(gd_args.size());
    for (int i = 0; i < (int)gd_args.size(); ++i)
        VariantBridge::to_pl_value(gd_args[i], args[i]);

    PLValue ret; pl_value_init(&ret);
    int rc = pl_engine_call_impl(
        group.utf8().get_data(), method.utf8().get_data(),
        args.data(), (int32_t)args.size(), &ret, SandboxTier::Trusted);

    godot::Dictionary d;
    d["rc"]     = rc;
    d["result"] = VariantBridge::from_pl_value(ret);
    for (auto& v : args) VariantBridge::free_pl_value(v);
    VariantBridge::free_pl_value(ret);
    return d;
}

// ── Physics handlers ──────────────────────────────────────────

int PLEngineAPIBridge::h_physics_raycast(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 2) return PL_ERR_GENERIC;
    auto* ps = godot::PhysicsServer3D::get_singleton();
    if (!ps) return PL_ERR_GENERIC;
    ret->type = PL_TYPE_NIL;
    return PL_OK;
}

int PLEngineAPIBridge::h_physics_get_gravity(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    auto* ps = godot::PhysicsServer3D::get_singleton();
    if (!ps) return PL_ERR_GENERIC;
    ret->type  = PL_TYPE_VEC3;
    ret->v3[0] = 0.0f;
    ret->v3[1] = -9.8f;
    ret->v3[2] = 0.0f;
    return PL_OK;
}

// ── Audio handlers ────────────────────────────────────────────

int PLEngineAPIBridge::h_audio_play(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 1 || args[0].type != PL_TYPE_STRING || !args[0].s)
        return PL_ERR_GENERIC;
    return PL_OK;
}

int PLEngineAPIBridge::h_audio_stop(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    return PL_OK;
}

// ── Input handlers ────────────────────────────────────────────

int PLEngineAPIBridge::h_input_is_action_pressed(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 1 || args[0].type != PL_TYPE_STRING || !args[0].s)
        return PL_ERR_GENERIC;
    auto* inp = godot::Input::get_singleton();
    if (!inp) return PL_ERR_GENERIC;
    ret->type = PL_TYPE_BOOL;
    ret->b    = inp->is_action_pressed(godot::StringName(args[0].s));
    return PL_OK;
}

int PLEngineAPIBridge::h_input_get_axis(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 2) return PL_ERR_GENERIC;
    auto* inp = godot::Input::get_singleton();
    if (!inp) return PL_ERR_GENERIC;
    const char* neg = (args[0].type == PL_TYPE_STRING && args[0].s) ? args[0].s : "";
    const char* pos = (args[1].type == PL_TYPE_STRING && args[1].s) ? args[1].s : "";
    ret->type = PL_TYPE_FLOAT;
    ret->f    = (double)inp->get_axis(godot::StringName(neg), godot::StringName(pos));
    return PL_OK;
}

int PLEngineAPIBridge::h_input_get_vector(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 4) return PL_ERR_GENERIC;
    auto* inp = godot::Input::get_singleton();
    if (!inp) return PL_ERR_GENERIC;
    auto get_str = [](const PLValue& v) -> const char* {
        return (v.type == PL_TYPE_STRING && v.s) ? v.s : "";
    };
    godot::Vector2 v = inp->get_vector(
        godot::StringName(get_str(args[0])), godot::StringName(get_str(args[1])),
        godot::StringName(get_str(args[2])), godot::StringName(get_str(args[3])));
    ret->type  = PL_TYPE_VEC2;
    ret->v2[0] = v.x;
    ret->v2[1] = v.y;
    return PL_OK;
}

// ── Scene handlers ────────────────────────────────────────────

int PLEngineAPIBridge::h_scene_instantiate(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 1 || args[0].type != PL_TYPE_STRING || !args[0].s)
        return PL_ERR_GENERIC;
    auto res = godot::ResourceLoader::get_singleton()->load(godot::String(args[0].s));
    auto* ps = godot::Object::cast_to<godot::PackedScene>(res.ptr());
    if (!ps) return PL_ERR_GENERIC;
    godot::Node* node = ps->instantiate();
    if (!node) return PL_ERR_GENERIC;
    ret->type = PL_TYPE_INT;
    ret->i    = (int64_t)node->get_instance_id();
    return PL_OK;
}

int PLEngineAPIBridge::h_scene_get_node(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 2 || args[0].type != PL_TYPE_INT ||
        args[1].type != PL_TYPE_STRING || !args[1].s) return PL_ERR_GENERIC;
    godot::Node* parent = godot::Object::cast_to<godot::Node>(
        godot::ObjectDB::get_instance((uint64_t)args[0].i));
    if (!parent) return PL_ERR_GENERIC;
    godot::Node* child = parent->get_node<godot::Node>(godot::NodePath(args[1].s));
    if (!child) return PL_ERR_GENERIC;
    ret->type = PL_TYPE_INT;
    ret->i    = (int64_t)child->get_instance_id();
    return PL_OK;
}

int PLEngineAPIBridge::h_scene_queue_free(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 1 || args[0].type != PL_TYPE_INT) return PL_ERR_GENERIC;
    godot::Node* node = godot::Object::cast_to<godot::Node>(
        godot::ObjectDB::get_instance((uint64_t)args[0].i));
    if (node) node->queue_free();
    return PL_OK;
}

int PLEngineAPIBridge::h_scene_add_child(PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    if (argc < 2 || args[0].type != PL_TYPE_INT || args[1].type != PL_TYPE_INT)
        return PL_ERR_GENERIC;
    godot::Node* parent = godot::Object::cast_to<godot::Node>(
        godot::ObjectDB::get_instance((uint64_t)args[0].i));
    godot::Node* child = godot::Object::cast_to<godot::Node>(
        godot::ObjectDB::get_instance((uint64_t)args[1].i));
    if (!parent || !child) return PL_ERR_GENERIC;
    parent->add_child(child);
    return PL_OK;
}

// ── Time handlers ─────────────────────────────────────────────

int PLEngineAPIBridge::h_time_get_ticks_msec(PLValue*, int32_t, PLValue* ret) {
    pl_value_init(ret);
    auto* t = godot::Time::get_singleton();
    ret->type = PL_TYPE_INT;
    ret->i    = t ? (int64_t)t->get_ticks_msec() : 0;
    return PL_OK;
}

int PLEngineAPIBridge::h_time_get_unix(PLValue*, int32_t, PLValue* ret) {
    pl_value_init(ret);
    auto* t = godot::Time::get_singleton();
    ret->type = PL_TYPE_FLOAT;
    ret->f    = t ? t->get_unix_time_from_system() : 0.0;
    return PL_OK;
}

// ── flush ─────────────────────────────────────────────────────
// FIX C-6: Uses dc.tier — not hardcoded SandboxTier::Trusted.

void PLEngineAPIBridge::flush() {
    std::queue<DeferredCall> local;
    {
        std::lock_guard<std::mutex> lk(deferred_mutex_);
        std::swap(local, deferred_queue_);
    }
    while (!local.empty()) {
        auto& dc = local.front();
        // FIX C-6: pass the tier that was set when the call was enqueued.
        int rc = pl_engine_call_impl(
            dc.group.c_str(), dc.method.c_str(),
            dc.args, dc.argc, dc.ret_out, dc.tier);
        if (dc.rc_out) *dc.rc_out = rc;
        if (dc.done_mutex && dc.done_cv && dc.done_flag) {
            std::lock_guard<std::mutex> dlk(*dc.done_mutex);
            *dc.done_flag = true;
            dc.done_cv->notify_all();
        }
        local.pop();
    }
}

} // namespace polylang
