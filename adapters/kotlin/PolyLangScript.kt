// =============================================================
// PolyLangScript.kt  —  Kotlin base class for PolyLang scripts
// =============================================================
// Compile alongside your user scripts into polylang_scripts.jar:
//   kotlinc PolyLangScript.kt Enemy.kt -include-runtime \
//           -d polylang_scripts.jar
//
// Then place polylang_scripts.jar next to the GDExtension .so.
// =============================================================

/**
 * Base class for all PolyLang Kotlin scripts.
 * Override the lifecycle methods and callMethod() to implement behaviour.
 */
abstract class PolyLangScript {
    // ── Lifecycle hooks (override as needed) ─────────────────
    open fun ready()                      {}
    open fun process(delta: Double)       {}
    open fun physicsProcess(delta: Double){}
    open fun enterTree()                  {}
    open fun exitTree()                   {}

    /**
     * Dispatch a named method call from Godot.
     * Return null to signal "no result / void".
     */
    open fun callMethod(name: String, args: Array<Any?>): Any? = null

    /**
     * Set a named property from Godot. Return true if handled.
     */
    open fun setProperty(name: String, value: Any?): Boolean = false

    /**
     * Get a named property for Godot. Return null if not found.
     */
    open fun getProperty(name: String): Any? = null
}

// =============================================================
// Example: Enemy.kt  (compile into polylang_scripts.jar)
// =============================================================
class Enemy : PolyLangScript() {
    var health: Double = 100.0
    var speed:  Double = 5.0

    override fun ready()                { health = 100.0; speed = 5.0 }
    override fun process(delta: Double) { /* movement logic */ }

    override fun callMethod(name: String, args: Array<Any?>): Any? = when (name) {
        "take_damage" -> {
            val dmg = (args.getOrNull(0) as? Double) ?: 0.0
            health = maxOf(0.0, health - dmg)
            health
        }
        "is_dead"    -> health <= 0.0
        "get_health" -> health
        else         -> null
    }

    override fun setProperty(name: String, value: Any?): Boolean = when (name) {
        "health" -> { health = value as? Double ?: health; true }
        "speed"  -> { speed  = value as? Double ?: speed;  true }
        else     -> false
    }

    override fun getProperty(name: String): Any? = when (name) {
        "health" -> health
        "speed"  -> speed
        else     -> null
    }
}
