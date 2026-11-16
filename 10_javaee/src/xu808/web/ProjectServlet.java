// ============================================================================
//  ProjectServlet.java —— 工程与导出 API 控制器（10_javaee 版）
//
//  与 09_jsp 的 ApiServlet 功能完全一致，但刻意不使用 @WebServlet，
//  改由 WEB-INF/web.xml 显式声明映射，这是标准 Java EE 部署描述符的做法：
//    GET  /api/ping          健康检查，验证 WAR 已启动
//    GET  /api/project       取回会话工程
//    POST /api/save          规范化并下载工程 JSON
//    POST /api/load          上传工程 JSON，规范化后回传
//    POST /api/export/wav    ?mode=pattern|song&track=-1..15
//    POST /api/export/midi   ?mode=pattern|song
//
//  服务端只做“数据与离线渲染”，实时发声交给浏览器 AudioWorklet；
//  两边共用同一套 XuProject 语义，导出的声音就是屏幕上格子的声音。
// ============================================================================
package xu808.web;

import java.io.IOException;

import javax.servlet.ServletException;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

import xu808.Json;
import xu808.XuProject;

public class ProjectServlet extends HttpServlet {
    private static final long serialVersionUID = 0x18082L;

    @Override
    protected void doGet(HttpServletRequest req, HttpServletResponse resp)
            throws ServletException, IOException {
        String path = action(req);
        try {
            if ("/ping".equals(path)) {
                // 版本号从 web.xml 的 context-param 读，避免两套 WAR 复制代码时写串版本
                String ver = getServletContext().getInitParameter("xu808.version");
                if (ver == null) ver = "10_javaee";
                XuWeb.writeJson(resp,
                        "{\"ok\":true,\"app\":\"XU-808\",\"version\":\"" + ver + "\"}", null);
                return;
            }
            if ("/project".equals(path)) {
                HttpSession s = req.getSession(true);
                XuProject p = XuWeb.projectFrom(req, null);   // 会话有就用，没有给默认工程
                XuWeb.remember(s, p);
                XuWeb.writeJson(resp, Json.write(p.toMap()), null);
                return;
            }
            XuWeb.writeError(resp, 404, "未知接口: " + path);
        } catch (Exception e) {
            XuWeb.writeError(resp, 400, "请求失败: " + e.getMessage());
        }
    }

    @Override
    protected void doPost(HttpServletRequest req, HttpServletResponse resp)
            throws ServletException, IOException {
        req.setCharacterEncoding("UTF-8");       // 必须在第一次 getParameter/getInputStream 之前
        String path = action(req);
        try {
            String body = XuWeb.readBody(req);   // 空体表示“用会话里的工程”
            XuProject p = XuWeb.projectFrom(req, body);
            HttpSession session = req.getSession(true);

            switch (path) {
                case "/save": {
                    XuWeb.remember(session, p);
                    // 规范化后的工程直接作为附件下载，用户得到的就是可再次 load 的文件
                    XuWeb.writeJson(resp, Json.write(p.toMap()), "xu808-project.json");
                    return;
                }
                case "/load": {
                    XuWeb.remember(session, p);
                    XuWeb.writeJson(resp, Json.write(p.toMap()), null);
                    return;
                }
                case "/export/wav": {
                    XuWeb.remember(session, p);
                    boolean song = "song".equalsIgnoreCase(req.getParameter("mode"));
                    int track = XuWeb.intParam(req, "track", -1);
                    byte[] wav = XuWeb.renderWav(p, song, track);
                    String name = "xu808-" + (song ? "song" : "pattern")
                            + (track >= 0 ? "-" + XuWeb.padTag(track) : "") + ".wav";
                    XuWeb.writeBinary(resp, wav, "audio/wav", name);
                    return;
                }
                case "/export/midi": {
                    XuWeb.remember(session, p);
                    boolean song = "song".equalsIgnoreCase(req.getParameter("mode"));
                    byte[] mid = XuWeb.renderMidi(p, song);
                    String name = "xu808-" + (song ? "song" : "pattern") + ".mid";
                    XuWeb.writeBinary(resp, mid, "audio/midi", name);
                    return;
                }
                case "/project": {
                    // 允许 POST 覆盖会话工程（前端保存草稿时用）
                    XuWeb.remember(session, p);
                    XuWeb.writeJson(resp, Json.write(p.toMap()), null);
                    return;
                }
                default:
                    XuWeb.writeError(resp, 404, "未知接口: " + path);
            }
        } catch (IllegalArgumentException e) {
            // JSON 语法错误、字段类型不对都归到 400，前端能显示具体原因
            XuWeb.writeError(resp, 400, "工程 JSON 无法解析: " + e.getMessage());
        } catch (Exception e) {
            // 渲染失败（磁盘满、权限）是服务端问题，500 让验证脚本能区分
            log("XU808 API 失败: " + path, e);
            XuWeb.writeError(resp, 500, "服务端处理失败: " + e.getMessage());
        }
    }

    /**
     * 取出 /api 之后的路径，例如 /export/wav；去掉末尾斜杠避免匹配失败。
     */
    private String action(HttpServletRequest req) {
        String p = req.getPathInfo();
        if (p == null || p.isEmpty()) return "/";
        while (p.length() > 1 && p.endsWith("/")) p = p.substring(0, p.length() - 1);
        return p;
    }
}
