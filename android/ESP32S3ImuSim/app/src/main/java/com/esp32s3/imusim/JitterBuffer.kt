package com.esp32s3.imusim

class JitterBuffer<T>(private val minDepth: Int = 2) {
    private val items = ArrayDeque<T>()

    fun pushAll(values: List<T>) {
        items.addAll(values)
    }

    fun popReady(): T? {
        if (items.size < minDepth) return null
        return items.removeFirst()
    }

    fun peekLatest(): T? = items.lastOrNull()

    fun clear() = items.clear()

    fun size(): Int = items.size
}
