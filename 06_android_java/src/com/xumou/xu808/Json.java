// ============================================================================
//  Json.java —— 极简 JSON 读写（Android 版，零第三方依赖）
//
//  徐某为什么不引入 Gson/Jackson：本项目要能“只靠命令行工具链”编译，
//  多一个依赖就多一个部署环节；工程文件结构是我们自己定的，
//  一个两百行的递归下降解析器完全够用，还能完全掌控容错行为。
//  支持：对象、数组、字符串、数字、true/false/null。
// ============================================================================
package com.xumou.xu808;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class Json {
    private Json() {
    }

    // ------------------------------------------------------------------
    // 写：Map/List/String/Number/Boolean → JSON 文本（带缩进，便于人工检查）
    // ------------------------------------------------------------------
    public static String write(Object o) {
        StringBuilder sb = new StringBuilder();
        writeValue(sb, o, 0);
        return sb.toString();
    }

    @SuppressWarnings("unchecked")
    private static void writeValue(StringBuilder sb, Object o, int indent) {
        if (o == null) {
            sb.append("null");
            return;
        }
        if (o instanceof String) {
            writeString(sb, (String) o);
            return;
        }
        if (o instanceof Boolean) {
            sb.append(o.toString());
            return;
        }
        if (o instanceof Number) {
            double d = ((Number) o).doubleValue();
            // 徐某：整数就不带小数点，工程文件更短、更好读
            if (d == Math.rint(d) && Math.abs(d) < 1e15) sb.append((long) d);
            else sb.append(d);
            return;
        }
        if (o instanceof Map) {
            Map<String, Object> m = (Map<String, Object>) o;
            sb.append("{\n");
            int i = 0;
            for (Map.Entry<String, Object> e : m.entrySet()) {
                pad(sb, indent + 1);
                writeString(sb, e.getKey());
                sb.append(": ");
                writeValue(sb, e.getValue(), indent + 1);
                if (++i < m.size()) sb.append(',');
                sb.append('\n');
            }
            pad(sb, indent);
            sb.append('}');
            return;
        }
        if (o instanceof List) {
            List<Object> l = (List<Object>) o;
            sb.append("[\n");
            for (int i = 0; i < l.size(); i++) {
                pad(sb, indent + 1);
                writeValue(sb, l.get(i), indent + 1);
                if (i + 1 < l.size()) sb.append(',');
                sb.append('\n');
            }
            pad(sb, indent);
            sb.append(']');
            return;
        }
        writeString(sb, o.toString());
    }

    private static void pad(StringBuilder sb, int n) {
        for (int i = 0; i < n; i++) sb.append("  ");
    }

    private static void writeString(StringBuilder sb, String s) {
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':
                    sb.append("\\\"");
                    break;
                case '\\':
                    sb.append("\\\\");
                    break;
                case '\n':
                    sb.append("\\n");
                    break;
                case '\r':
                    sb.append("\\r");
                    break;
                case '\t':
                    sb.append("\\t");
                    break;
                default:
                    if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
                    else sb.append(c);
            }
        }
        sb.append('"');
    }

    // ------------------------------------------------------------------
    // 读：递归下降解析
    // ------------------------------------------------------------------
    public static Object parse(String text) {
        Parser p = new Parser(text);
        Object v = p.value();
        p.skipWs();
        if (!p.eof()) throw new IllegalArgumentException("JSON 末尾有多余内容");
        return v;
    }

    // ------------------------------------------------------------------
    // 取值辅助：缺失字段一律回落到默认值，保证旧工程/新工程都能打开
    // ------------------------------------------------------------------
    @SuppressWarnings("unchecked")
    public static Map<String, Object> obj(Object o) {
        return (o instanceof Map) ? (Map<String, Object>) o : new LinkedHashMap<>();
    }

    @SuppressWarnings("unchecked")
    public static List<Object> arr(Object o) {
        return (o instanceof List) ? (List<Object>) o : new ArrayList<>();
    }

    public static double num(Map<String, Object> m, String k, double def) {
        Object v = m.get(k);
        return (v instanceof Number) ? ((Number) v).doubleValue() : def;
    }

    public static int integer(Map<String, Object> m, String k, int def) {
        return (int) Math.round(num(m, k, def));
    }

    public static boolean bool(Map<String, Object> m, String k, boolean def) {
        Object v = m.get(k);
        return (v instanceof Boolean) ? (Boolean) v : def;
    }

    public static String str(Map<String, Object> m, String k, String def) {
        Object v = m.get(k);
        return (v instanceof String) ? (String) v : def;
    }

    private static final class Parser {
        private final String s;
        private int i = 0;

        Parser(String s) {
            this.s = s;
        }

        boolean eof() {
            return i >= s.length();
        }

        void skipWs() {
            while (i < s.length() && Character.isWhitespace(s.charAt(i))) i++;
        }

        char peek() {
            return s.charAt(i);
        }

        Object value() {
            skipWs();
            if (eof()) throw new IllegalArgumentException("JSON 意外结束");
            char c = peek();
            if (c == '{') return object();
            if (c == '[') return array();
            if (c == '"') return string();
            if (c == 't' || c == 'f') return bool();
            if (c == 'n') {
                expect("null");
                return null;
            }
            return number();
        }

        private void expect(String lit) {
            if (!s.startsWith(lit, i)) throw new IllegalArgumentException("JSON 期望 " + lit);
            i += lit.length();
        }

        Map<String, Object> object() {
            Map<String, Object> m = new LinkedHashMap<>();
            i++; // 跳过 '{'
            skipWs();
            if (!eof() && peek() == '}') {
                i++;
                return m;
            }
            while (true) {
                skipWs();
                String k = string();
                skipWs();
                if (peek() != ':') throw new IllegalArgumentException("JSON 缺少冒号");
                i++;
                m.put(k, value());
                skipWs();
                char c = peek();
                i++;
                if (c == '}') break;
                if (c != ',') throw new IllegalArgumentException("JSON 对象分隔错误");
            }
            return m;
        }

        List<Object> array() {
            List<Object> l = new ArrayList<>();
            i++; // 跳过 '['
            skipWs();
            if (!eof() && peek() == ']') {
                i++;
                return l;
            }
            while (true) {
                l.add(value());
                skipWs();
                char c = peek();
                i++;
                if (c == ']') break;
                if (c != ',') throw new IllegalArgumentException("JSON 数组分隔错误");
            }
            return l;
        }

        String string() {
            if (peek() != '"') throw new IllegalArgumentException("JSON 期望字符串");
            i++;
            StringBuilder sb = new StringBuilder();
            while (true) {
                char c = s.charAt(i++);
                if (c == '"') break;
                if (c == '\\') {
                    char e = s.charAt(i++);
                    switch (e) {
                        case '"':
                            sb.append('"');
                            break;
                        case '\\':
                            sb.append('\\');
                            break;
                        case '/':
                            sb.append('/');
                            break;
                        case 'b':
                            sb.append('\b');
                            break;
                        case 'f':
                            sb.append('\f');
                            break;
                        case 'n':
                            sb.append('\n');
                            break;
                        case 'r':
                            sb.append('\r');
                            break;
                        case 't':
                            sb.append('\t');
                            break;
                        case 'u':
                            sb.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                            i += 4;
                            break;
                        default:
                            throw new IllegalArgumentException("非法转义");
                    }
                } else {
                    sb.append(c);
                }
            }
            return sb.toString();
        }

        Boolean bool() {
            if (s.startsWith("true", i)) {
                i += 4;
                return Boolean.TRUE;
            }
            if (s.startsWith("false", i)) {
                i += 5;
                return Boolean.FALSE;
            }
            throw new IllegalArgumentException("JSON 布尔值错误");
        }

        Double number() {
            int start = i;
            if (peek() == '-') i++;
            while (!eof() && "0123456789.eE+-".indexOf(peek()) >= 0) i++;
            return Double.parseDouble(s.substring(start, i));
        }
    }
}
