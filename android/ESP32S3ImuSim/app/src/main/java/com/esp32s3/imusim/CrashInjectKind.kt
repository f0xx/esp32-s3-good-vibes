package com.esp32s3.imusim

/** Dev-only fault kinds — must match zephyr/app/common/crash_debug.c inject handlers. */
object CrashInjectKind {
    data class Spec(
        val id: String,
        val title: String,
        val detail: String,
    )

    val all: List<Spec> = listOf(
        Spec("panic", "Kernel panic", "Calls k_panic() — generic fatal halt."),
        Spec("assert", "Assert (false)", "Triggers __ASSERT(false) — config assert path."),
        Spec("null", "Null pointer write", "Stores through address 0 — LoadStoreError."),
        Spec("badptr", "Bad pointer read", "Reads from 0xDEADBEEF — invalid address fault."),
        Spec("div0", "Divide by zero", "Integer division by zero."),
        Spec("unalign", "Unaligned access", "32-bit load from misaligned address."),
        Spec("ill", "Illegal instruction", "Executes undefined opcode (ill)."),
        Spec("stack", "Stack overflow", "Infinite recursion until stack guard trips."),
        Spec("wdt", "Watchdog stall", "Blocks main loop forever — task WDT fires."),
    )

    fun find(id: String): Spec? = all.find { it.id == id }
}
