// ============================================================================
//  IndexServlet.java —— 首页控制器（09_jsp 版）
//
//  纯 JSP + Servlet + JavaBean 的 MVC 里，这个类只做三件事：
//    1) 从会话取出（或新建）工程 Model；
//    2) 把 Model 装进只读的 BootstrapBean；
//    3) forward 到 WEB-INF/views/index.jsp，由 JSP 负责全部 HTML。
//  徐某把 JSP 放在 WEB-INF 下，浏览器无法绕过 Controller 直接访问视图，
//  这就是“控制器唯一入口”的最朴素实现。
// ============================================================================
package xu808.web;

import java.io.IOException;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.http.HttpSession;

import xu808.Json;
import xu808.XuProject;

/**
 * 映射 /app；web.xml 的 welcome-file 会把根路径也送到这里。
 */
@WebServlet(name = "xu808Index", urlPatterns = {"/app"})
public class IndexServlet extends HttpServlet {
    private static final long serialVersionUID = 0x8081L;

    @Override
    protected void doGet(HttpServletRequest req, HttpServletResponse resp)
            throws ServletException, IOException {
        // 徐某：会话必须在这里创建，否则第一次访问时 ApiServlet 拿不到同一个 Session
        HttpSession session = req.getSession(true);
        String saved = XuWeb.sessionJson(session);

        XuProject project;
        if (saved == null) {
            project = new XuProject();                 // 首次访问：带一段示范节奏，打开就能听
            XuWeb.remember(session, project);          // 立刻落会话，后续 API 有初始值
        } else {
            project = XuProject.fromMap(Json.obj(Json.parse(saved)));
        }

        BootstrapBean bean = new BootstrapBean();
        bean.init(project);                            // 只把 JSON 与常量交给视图
        req.setAttribute("boot", bean);                // JSP 里 <jsp:useBean scope="request"> 直接取
        req.getRequestDispatcher("/WEB-INF/views/index.jsp").forward(req, resp);
    }

    @Override
    protected void doPost(HttpServletRequest req, HttpServletResponse resp)
            throws ServletException, IOException {
        // 首页只接受 GET；POST 统一当作“重新打开界面”，避免 405 让用户困惑
        doGet(req, resp);
    }
}
