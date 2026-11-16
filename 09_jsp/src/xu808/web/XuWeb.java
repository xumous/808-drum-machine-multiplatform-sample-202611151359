// ============================================================================
//  XuWeb.java —— Web 层公共工具（09_jsp 版）
//
//  徐某把“Servlet 里反复要写的脏活”集中到这里：
//    · 读请求体（固定 UTF-8，避免中文工程名变问号）；
//    · 把浏览器传来的 JSON 还原成 XuProject；
//    · 用与实时完全相同的 XuEngine 离线渲染 WAV / MIDI；
//    · 统一写 JSON 响应与下载响应头。
//  这样做的好处是：IndexServlet 只管“找视图”，ApiServlet 只管“分发动作”，
//  两边都不会出现大段重复的 IO 代码，也方便和 10_javaee 版逐行对照。
// ============================================================================
package xu808.web;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;

import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

import xu808.Json;
import xu808.XuEngine;
import xu808.XuProject;

public final class XuWeb {
    /**
     * 会话里只存“规范化后的工程 JSON 字符串”，不存 Java 对象：
     * 一来避免 Tomcat 关闭时序列化会话失败，二来前后端交换的就是同一份 JSON。
     */
    public static final String SESSION_KEY = "xu808.project.json";

    /**
     * 徐某设的上限：64 步 × 16 轨 × 7 字段的工程约 1.5MB，4MB 足够且能挡恶意大包。
     */
    private static final int MAX_BODY = 4 * 1024 * 1024;

    private XuWeb() {
    }

    // ------------------------------------------------------------------
    // 请求体 / 工程对象
    // ------------------------------------------------------------------

    /**
     * 读取整个请求体；Servlet 容器不保证默认编码，所以这里显式按 UTF-8 解码。
     */
    public static String readBody(HttpServletRequest req) throws IOException {
        req.setCharacterEncoding("UTF-8");
        StringBuilder sb = new StringBuilder();
        try (BufferedReader r = new BufferedReader(
                new InputStreamReader(req.getInputStream(), StandardCharsets.UTF_8))) {
            char[] buf = new char[8192];
            int n;
            while ((n = r.read(buf)) > 0) {
                sb.append(buf, 0, n);
                if (sb.length() > MAX_BODY) throw new IOException("请求体超过 4MB 限制");
            }
        }
        return sb.toString();
    }

    /**
     * 会话中最近一次保存/加载的工程 JSON；没有就返回 null。
     */
    public static String sessionJson(HttpSession session) {
        Object v = session.getAttribute(SESSION_KEY);
        return (v instanceof String) ? (String) v : null;
    }

    /**
     * 把工程规范化后写回会话，保证浏览器刷新后还能拿到上次的工程。
     */
    public static void remember(HttpSession session, XuProject p) {
        session.setAttribute(SESSION_KEY, Json.write(p.toMap()));
    }

    /**
     * 优先级：请求体里的工程 > 会话里的工程 > 全新默认工程。
     * 导出接口允许前端把“尚未保存的当前状态”直接 POST 上来，
     * 这样导出的声音和屏幕上看到的格子严格一致。
     */
    public static XuProject projectFrom(HttpServletRequest req, String body) {
        if (body != null && !body.trim().isEmpty()) {
            Object root = Json.parse(body);
            return XuProject.fromMap(Json.obj(root));
        }
        String saved = sessionJson(req.getSession(true));
        if (saved != null) return XuProject.fromMap(Json.obj(Json.parse(saved)));
        return new XuProject();
    }

    /**
     * 把 JSON 文本解析成工程；解析失败时抛出 IllegalArgumentException，由 Servlet 转 400。
     */
    public static XuProject parseProject(String json) {
        return XuProject.fromMap(Json.obj(Json.parse(json)));
    }

    // ------------------------------------------------------------------
    // 离线渲染
    // ------------------------------------------------------------------

    /**
     * 导出 WAV。track < 0 表示总混音；否则只保留该声部（分轨导出）。
     * 徐某在这里复制一份工程再静音其它轨，绝不改动用户会话里的原工程。
     */
    public static byte[] renderWav(XuProject source, boolean song, int track) throws IOException {
        XuProject copy = XuProject.fromMap(source.toMap());
        if (track >= 0 && track < XuProject.PADS) {
            for (int i = 0; i < XuProject.PADS; i++) {
                copy.pads[i].solo = false;                 // 先清 solo，否则 solo 会压过 mute
                copy.pads[i].mute = (i != track);          // 只留目标轨
            }
        }
        File tmp = File.createTempFile("xu808-web-", ".wav");   // 交给引擎写临时文件
        try {
            new XuEngine(copy).exportWav(tmp.getAbsolutePath(), song);
            return Files.readAllBytes(tmp.toPath());
        } finally {
            if (!tmp.delete()) tmp.deleteOnExit();             // 删不掉就等 JVM 退出再删
        }
    }

    /**
     * 导出 MIDI；MIDI 不分轨，格式 1、PPQ 480，由 XuEngine 统一生成。
     */
    public static byte[] renderMidi(XuProject source, boolean song) throws IOException {
        File tmp = File.createTempFile("xu808-web-", ".mid");
        try {
            new XuEngine(source).exportMidi(tmp.getAbsolutePath(), song);
            return Files.readAllBytes(tmp.toPath());
        } finally {
            if (!tmp.delete()) tmp.deleteOnExit();
        }
    }

    // ------------------------------------------------------------------
    // 响应
    // ------------------------------------------------------------------

    /**
     * 返回工程 JSON；带 Content-Disposition 时浏览器会直接弹出“另存为”。
     */
    public static void writeJson(HttpServletResponse resp, String json, String filename)
            throws IOException {
        byte[] bytes = json.getBytes(StandardCharsets.UTF_8);
        resp.setCharacterEncoding("UTF-8");
        resp.setContentType("application/json; charset=UTF-8");
        resp.setHeader("Cache-Control", "no-store");
        if (filename != null) {
            resp.setHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        }
        resp.setContentLength(bytes.length);
        resp.getOutputStream().write(bytes);
    }

    /**
     * 二进制下载：WAV/MIDI 都走这里，显式给出长度让浏览器能显示进度。
     */
    public static void writeBinary(HttpServletResponse resp, byte[] bytes,
                                   String contentType, String filename) throws IOException {
        resp.setContentType(contentType);
        resp.setHeader("Cache-Control", "no-store");
        resp.setHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        resp.setContentLength(bytes.length);
        resp.getOutputStream().write(bytes);
    }

    /**
     * 统一错误响应：前端 fetch 里可以直接读 message 显示在状态栏。
     */
    public static void writeError(HttpServletResponse resp, int status, String message)
            throws IOException {
        resp.setStatus(status);
        String safe = message == null ? "未知错误" : message.replace("\\", "\\\\").replace("\"", "\\\"");
        writeJson(resp, "{\"error\":\"" + safe + "\"}", null);
    }

    /**
     * 从查询串取整数，缺省用 def；非法值也退回 def，避免 500。
     */
    public static int intParam(HttpServletRequest req, String name, int def) {
        try {
            String v = req.getParameter(name);
            return (v == null || v.isEmpty()) ? def : Integer.parseInt(v);
        } catch (NumberFormatException e) {
            return def;
        }
    }

    /**
     * 按声部号生成文件名后缀，例如 BD / CH，便于分轨 WAV 一眼识别。
     */
    public static String padTag(int pad) {
        return (pad >= 0 && pad < XuProject.CODES.length) ? XuProject.CODES[pad] : "MIX";
    }

    /**
     * 把前端送来的任意 Map 转成工程（供 10_javaee 版复用同款逻辑）。
     */
    public static XuProject fromMap(Map<String, Object> root) {
        return XuProject.fromMap(root);
    }
}
