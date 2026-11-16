// ============================================================================
//  XuEncodingFilter.java —— 全局编码与响应头过滤器（10_javaee 版）
//
//  为什么需要 Filter：
//    · Servlet 规范里 setCharacterEncoding 必须早于 getParameter/getInputStream，
//      放在 Filter 里就不会因为某个 Servlet 忘了写而导致中文工程名乱码；
//    · 顺便给动态响应加一个 X-XU808 标记，验证脚本可以确认请求确实过了 Filter。
//  映射只覆盖 /app 与 /api/*，不碰静态资源：否则 Tomcat 的 DefaultServlet
//  会在响应已声明 charset 时改用 Writer，把 UTF-8 的 JS 按 GBK 二次编码。
//  徐某不使用 @WebFilter，映射仍写在 web.xml，保持“一个部署描述符”的完整性。
// ============================================================================
package xu808.web;

import java.io.IOException;

import javax.servlet.Filter;
import javax.servlet.FilterChain;
import javax.servlet.FilterConfig;
import javax.servlet.ServletException;
import javax.servlet.ServletRequest;
import javax.servlet.ServletResponse;
import javax.servlet.http.HttpServletRequest;

public class XuEncodingFilter implements Filter {

    @Override
    public void init(FilterConfig config) {
        // 徐某：把 web.xml 里的 filter 名称记下来，日志里能确认 Filter 已初始化
        System.out.println("[XU-808][10_javaee] Filter 初始化: " + config.getFilterName());
    }

    @Override
    public void doFilter(ServletRequest request, ServletResponse response, FilterChain chain)
            throws IOException, ServletException {
        // 统一请求/响应编码；UTF-8 是 JSP、JSON、WAV 文件名共用的安全选择
        request.setCharacterEncoding("UTF-8");
        response.setCharacterEncoding("UTF-8");
        // ServletResponse 接口没有 setHeader，必须转成 HttpServletResponse 才能加响应头
        if (response instanceof javax.servlet.http.HttpServletResponse) {
            ((javax.servlet.http.HttpServletResponse) response).setHeader("X-XU808", "10_javaee");
        }

        // 给 API 响应加 no-store，防止浏览器缓存导出结果或工程 JSON
        if (request instanceof HttpServletRequest) {
            String uri = ((HttpServletRequest) request).getRequestURI();
            if (uri != null && uri.contains("/api/")) {
                ((javax.servlet.http.HttpServletResponse) response)
                        .setHeader("Cache-Control", "no-store");
            }
        }
        chain.doFilter(request, response);   // 放行到 Servlet / 静态资源
    }

    @Override
    public void destroy() {
        // 无需释放资源；保留方法是为了让生命周期完整
    }
}
