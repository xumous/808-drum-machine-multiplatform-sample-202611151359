// ============================================================================
//  Json.kt —— 极简 JSON 读写（零第三方依赖）  ·  徐某
//
//  为什么不用 Gson/Jackson：本项目要求“只装 JDK 就能编译”，
//  多一个 jar 就多一个部署环节；工程文件结构是我们自己定的，
//  一个递归下降解析器完全够用，容错行为也完全可控。
//
//  支持：对象、数组、字符串、数字、true/false/null。
//  不支持：注释、NaN/Infinity（工程文件里不需要）。
// ============================================================================
package xu808

import java.util.LinkedHashMap

object Json {

    // ------------------------------------------------------------------
    // 写：把 Map / List / 基本类型 递归写成带缩进的文本
    // ------------------------------------------------------------------
    fun write(o: Any?): String {
        val sb = StringBuilder()
        writeValue(sb, o, 0)
        return sb.toString()
    }

    private fun writeValue(sb: StringBuilder, o: Any?, indent: Int) {
        when (o) {
            null -> sb.append("null")
            is String -> writeString(sb, o)
            is Boolean -> sb.append(o.toString())
            is Number -> {
                val d = o.toDouble()
                // 整数不带小数点：文件更短、人眼更好读
                if (d == Math.rint(d) && Math.abs(d) < 1e15) sb.append(d.toLong())
                else sb.append(d)
            }

            is Map<*, *> -> {
                sb.append("{\n")
                var i = 0
                for ((k, v) in o) {
                    pad(sb, indent + 1)
                    writeString(sb, k.toString())
                    sb.append(": ")
                    writeValue(sb, v, indent + 1)
                    if (++i < o.size) sb.append(',')
                    sb.append('\n')
                }
                pad(sb, indent)
                sb.append('}')
            }

            is List<*> -> {
                sb.append("[\n")
                for (i in o.indices) {
                    pad(sb, indent + 1)
                    writeValue(sb, o[i], indent + 1)
                    if (i + 1 < o.size) sb.append(',')
                    sb.append('\n')
                }
                pad(sb, indent)
                sb.append(']')
            }

            else -> writeString(sb, o.toString())
        }
    }

    private fun pad(sb: StringBuilder, n: Int) {
        for (i in 0 until n) sb.append("  ")
    }

    private fun writeString(sb: StringBuilder, s: String) {
        sb.append('"')
        for (c in s) {
            when (c) {
                '"' -> sb.append("\\\"")
                '\\' -> sb.append("\\\\")
                '\n' -> sb.append("\\n")
                '\r' -> sb.append("\\r")
                '\t' -> sb.append("\\t")
                else -> if (c.code < 0x20) sb.append(String.format("\\u%04x", c.code)) else sb.append(c)
            }
        }
        sb.append('"')
    }

    // ------------------------------------------------------------------
    // 读：递归下降
    // ------------------------------------------------------------------
    fun parse(text: String): Any? {
        val p = Parser(text)
        val v = p.value()
        p.skipWs()
        if (!p.eof()) throw IllegalArgumentException("JSON 末尾有多余内容")
        return v
    }

    private class Parser(private val s: String) {
        private var i = 0

        fun eof() = i >= s.length

        fun skipWs() {
            while (i < s.length && s[i].isWhitespace()) i++
        }

        private fun peek(): Char = s[i]

        fun value(): Any? {
            skipWs()
            if (eof()) throw IllegalArgumentException("JSON 意外结束")
            return when (peek()) {
                '{' -> obj()
                '[' -> arr()
                '"' -> string()
                't', 'f' -> bool()
                'n' -> {
                    expect("null"); null
                }

                else -> number()
            }
        }

        private fun expect(lit: String) {
            if (!s.startsWith(lit, i)) throw IllegalArgumentException("JSON 期望 $lit")
            i += lit.length
        }

        private fun obj(): Map<String, Any?> {
            val m = LinkedHashMap<String, Any?>()
            i++ // {
            skipWs()
            if (!eof() && peek() == '}') {
                i++; return m
            }
            while (true) {
                skipWs()
                val k = string()
                skipWs()
                if (peek() != ':') throw IllegalArgumentException("JSON 缺少冒号")
                i++
                m[k] = value()
                skipWs()
                val c = peek()
                i++
                if (c == '}') break
                if (c != ',') throw IllegalArgumentException("JSON 对象分隔错误")
            }
            return m
        }

        private fun arr(): List<Any?> {
            val l = ArrayList<Any?>()
            i++ // [
            skipWs()
            if (!eof() && peek() == ']') {
                i++; return l
            }
            while (true) {
                l.add(value())
                skipWs()
                val c = peek()
                i++
                if (c == ']') break
                if (c != ',') throw IllegalArgumentException("JSON 数组分隔错误")
            }
            return l
        }

        private fun string(): String {
            if (peek() != '"') throw IllegalArgumentException("JSON 期望字符串")
            i++
            val sb = StringBuilder()
            while (true) {
                val c = s[i++]
                if (c == '"') break
                if (c == '\\') {
                    when (val e = s[i++]) {
                        '"' -> sb.append('"')
                        '\\' -> sb.append('\\')
                        '/' -> sb.append('/')
                        'b' -> sb.append('\b')
                        'f' -> sb.append('\u000C')
                        'n' -> sb.append('\n')
                        'r' -> sb.append('\r')
                        't' -> sb.append('\t')
                        'u' -> {
                            sb.append(s.substring(i, i + 4).toInt(16).toChar())
                            i += 4
                        }

                        else -> throw IllegalArgumentException("非法转义 $e")
                    }
                } else {
                    sb.append(c)
                }
            }
            return sb.toString()
        }

        private fun bool(): Boolean {
            if (s.startsWith("true", i)) {
                i += 4; return true
            }
            if (s.startsWith("false", i)) {
                i += 5; return false
            }
            throw IllegalArgumentException("JSON 布尔值错误")
        }

        private fun number(): Double {
            val start = i
            if (peek() == '-') i++
            while (!eof() && "0123456789.eE+-".indexOf(peek()) >= 0) i++
            return s.substring(start, i).toDouble()
        }
    }

    // ------------------------------------------------------------------
    // 取值辅助：带默认值，保证旧工程/缺字段也能打开（前向兼容）
    // ------------------------------------------------------------------
    fun obj(o: Any?): Map<String, Any?> =
            if (o is Map<*, *>) {
                @Suppress("UNCHECKED_CAST")
                o as Map<String, Any?>
            } else LinkedHashMap()

    fun arr(o: Any?): List<Any?> =
            if (o is List<*>) o else ArrayList()

    fun num(m: Map<String, Any?>, k: String, def: Double): Double {
        val v = m[k]
        return if (v is Number) v.toDouble() else def
    }

    fun integer(m: Map<String, Any?>, k: String, def: Int): Int =
            Math.round(num(m, k, def.toDouble())).toInt()

    fun bool(m: Map<String, Any?>, k: String, def: Boolean): Boolean {
        val v = m[k]
        return if (v is Boolean) v else def
    }

    fun str(m: Map<String, Any?>, k: String, def: String): String {
        val v = m[k]
        return if (v is String) v else def
    }
}
