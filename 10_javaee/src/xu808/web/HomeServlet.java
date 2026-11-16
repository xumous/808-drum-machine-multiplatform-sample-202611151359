// ============================================================================
//  HomeServlet.java —— 首页控制器（10_javaee 版）
//
//  标准 Java EE 风格的控制器：
//    · 不使用注解，映射写在 WEB-INF/web.xml；
//    · 首页与 API 分成两个 Servlet，各自职责单一；
//    · 视图固定在 /WEB-INF/views/ 下，必须经 Controller 转发才能访问。
//  徐某把它和 09_jsp 的 IndexServlet 对照着写，方便看出“注解配置”与
//  “部署描述符配置”只是声明方式不同，MVC 的责任边界完全一样。
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

public class HomeServlet extends HttpServlet {
    private static final long serialVersionUID = 0x18081L;

    @Override
    protected void doGet(HttpServletRequest req, HttpServletResponse resp)
            throws ServletException, IOException {
        // 徐某：会话必须在这里创建，否则第一次访问时 ProjectServlet 拿不到同一个 Session
        HttpSession session = req.getSession(true);
        String saved = XuWeb.sessionJson(session);

        XuProject project;
        if (saved == null) {
            project = new XuProject();      // 首次访问：带示范节奏，打开页面就能按播放
            XuWeb.remember(session, project);
        } else {
            project = XuProject.fromMap(Json.obj(Json.parse(saved)));
        }

        BootstrapBean bean = new BootstrapBean();
        bean.init(project);                  // 只把 JSON 与常量交给视图
        req.setAttribute("boot", bean);      // JSP 用 <jsp:useBean scope="request"> 取
        req.getRequestDispatcher("/WEB-INF/views/index.jsp").forward(req, resp);
    }

    @Override
    protected void doPost(HttpServletRequest req, HttpServletResponse resp)
            throws ServletException, IOException {
        // 首页只接受 GET；POST 也按 GET 处理，避免用户刷新表单时看到 405
        doGet(req, resp);
    }
}
