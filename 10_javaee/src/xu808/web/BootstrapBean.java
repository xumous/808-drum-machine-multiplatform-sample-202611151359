// ============================================================================
//  BootstrapBean.java —— 视图启动数据 JavaBean（10_javaee 版）
//
//  MVC 里的 Model 有两个层次：
//    1) 领域模型 XuProject（真正的数据与规则）；
//    2) 视图模型 BootstrapBean（JSP 只需要“能直接读的字符串/数组”）。
//  徐某坚持让 JSP 通过 <jsp:useBean> 拿数据，而不是在页面里 new 对象，
//  这样 View 对 Model 的依赖被压缩成一个只读 Bean，页面永远不会改坏工程状态。
//  10_javaee 与 09_jsp 的 Bean 内容一致，只有版本串不同，
//  这样两套 WAR 可以并排部署、用同一份前端资源做 A/B 对照。
// ============================================================================
package xu808.web;

import java.io.Serializable;

import xu808.Json;
import xu808.XuProject;

public class BootstrapBean implements Serializable {
    private static final long serialVersionUID = 0x808L;

    /**
     * 初始工程 JSON：JSP 把它塞进 window.__XU808_INIT__，前端据此建网格。
     */
    private String projectJson = "{}";
    /**
     * 页面标题与副标题，遵循规格书的「XU-808 鼓机 - 徐某」。
     */
    private String appTitle = "XU-808 鼓机 - 徐某";
    private String version = "10_javaee / Servlet 4.0 / web.xml / AudioWorklet";
    /**
     * 声部代号、中文名、MIDI 音符：让 JSP 用脚本片段直接渲染 16 行表头。
     */
    private String[] codes = XuProject.CODES;
    private String[] names = XuProject.NAMES_CN;
    private int[] midiNotes = XuProject.MIDI_NOTES;
    private String[] condNames = XuProject.COND_NAMES;

    public BootstrapBean() {
    }

    /**
     * 极简 JSON 压缩：只在字符串外删除空白字符。
     * 为什么要自己写：Json 的字符串转义已经把换行写成 \\n，
     * 所以扫描时只要正确跟踪“是否在字符串内 / 是否在转义”，就不会破坏内容。
     */
    private static String compact(String s) {
        StringBuilder out = new StringBuilder(s.length());
        boolean inString = false, escaped = false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (inString) {
                out.append(c);
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
            } else if (c == '"') {
                inString = true;
                out.append(c);
            } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                // 字符串外的空白全部丢掉
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }

    /**
     * 由 Controller 在转发前调用；只在服务端执行一次。
     */
    public void init(XuProject p) {
        // 徐某：Json.write 默认带缩进，16 Pattern × 64 步会让首页膨胀到 2MB+；
        // 这里做一次“字符串外空白压缩”，既保持合法 JSON，又把页面降到几百 KB。
        // 最后把 < 转成 \u003c，即使工程名里有人写 </script> 也无法提前闭合脚本标签。
        this.projectJson = compact(Json.write(p.toMap())).replace("<", "\\u003c");
    }

    public String getProjectJson() {
        return projectJson;
    }

    public String getAppTitle() {
        return appTitle;
    }

    public String getVersion() {
        return version;
    }

    public String[] getCodes() {
        return codes;
    }

    public String[] getNames() {
        return names;
    }

    public int[] getMidiNotes() {
        return midiNotes;
    }

    public String[] getCondNames() {
        return condNames;
    }
}
