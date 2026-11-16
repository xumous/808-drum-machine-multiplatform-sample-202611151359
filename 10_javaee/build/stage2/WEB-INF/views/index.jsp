<%--
  ============================================================================
   index.jsp —— XU-808 的视图（10_javaee 版，标准 Java EE + JSP）
   ----------------------------------------------------------------------------
   徐某把这一页当成“只会画界面的哑巴”：
     · 数据全部来自 request 里的 BootstrapBean（<jsp:useBean> 取用）；
     · 交互与声音全部交给 /js/xu808-ui.js 与 AudioWorklet；
     · 页面内不写任何业务规则，连默认 BPM 都从工程 JSON 读。
   与 09_jsp 的唯一区别在“谁来转发”：这里是 web.xml 显式声明的 HomeServlet，
   09_jsp 里是 @WebServlet 的 IndexServlet；视图本身保持 100% 相同。
   页码约定：所有静态资源都用 ${pageContext.request.contextPath} 拼绝对路径，
   这样 WAR 部署在 /xu808-javaee 还是 ROOT 都不会 404。
  ============================================================================
--%>
<%@ page contentType="text/html; charset=UTF-8" pageEncoding="UTF-8" session="true" %>
<%@ page import="xu808.web.BootstrapBean" %>
<jsp:useBean id="boot" class="xu808.web.BootstrapBean" scope="request"/>
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <%-- 移动端也要能横向滚动网格，所以 viewport 不允许缩放锁定 --%>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title><%= boot.getAppTitle() %>
    </title>
    <%-- 深色 808 主题：背景 #121212、面板 #1c1c1c、强调 #ff7a18，全部在 CSS 里集中定义 --%>
    <link rel="stylesheet" href="${pageContext.request.contextPath}/css/xu808.css">
</head>
<body>
<!-- ==========================================================================
     顶部工具条：LOGO、传输、BPM/Tap、Swing、Pattern、Song、工程 IO、状态
     徐某按规格书第 9 节从上到下排布，保证和 Swing/Qt 版的视觉顺序一致
     ========================================================================== -->
<header class="topbar">
    <div class="brand">
        <span class="logo">XU-808</span>
        <span class="tagline">徐某 · JavaEE MVC</span>
    </div>

    <div class="transport">
        <%-- 播放/停止按钮不直接调用音频 API，统一走 UI 脚本里的状态机 --%>
        <button id="btnPlay" class="btn primary" type="button">▶ 播放</button>
        <button id="btnStop" class="btn" type="button">■ 停止</button>
    </div>

    <div class="group tempo">
        <label class="lbl" for="bpm">BPM</label>
        <%-- 数字框与滑杆双向同步，范围严格按规格 40–300 --%>
        <input id="bpm" type="number" min="40" max="300" step="1" value="120" class="num">
        <input id="bpmRange" type="range" min="40" max="300" step="1" value="120" class="slider">
        <button id="btnTap" class="btn small" type="button" title="连续点击 4 次以上取平均">Tap</button>
    </div>

    <div class="group swing">
        <label class="lbl" for="swingRange">Swing <span id="swingVal" class="val">0%</span></label>
        <%-- 规格：0–75%，奇数 16 分步向后偏移 swing*stepSamples*0.5 --%>
        <input id="swingRange" type="range" min="0" max="75" step="1" value="0" class="slider">
    </div>

    <div class="group pattern">
        <label class="lbl" for="patternSel">Pattern</label>
        <%-- 16 个 Pattern；选项文字由 UI 脚本按工程数据填 --%>
        <select id="patternSel" class="sel"></select>
    </div>

    <div class="group song">
        <label class="check" title="按 Song 链顺序播放">
            <input type="checkbox" id="songMode"> Song
        </label>
        <button id="btnSong" class="btn small" type="button">编排</button>
    </div>

    <div class="group io">
        <button id="btnSave" class="btn" type="button" title="服务端规范化后下载 JSON">保存</button>
        <button id="btnLoad" class="btn" type="button" title="载入本地工程 JSON">加载</button>
        <%-- 隐藏的 file input：由“加载”按钮触发，避免浏览器默认丑陋样式 --%>
        <input type="file" id="fileLoad" accept=".json,application/json" hidden>
        <button id="btnWav" class="btn accent" type="button">导出 WAV</button>
        <button id="btnMidi" class="btn accent" type="button">导出 MIDI</button>
    </div>

    <div class="statusbox">
        <span id="status" class="status">就绪 · 点击播放启动音频</span>
        <%-- 双声道电平表：宽度由 worklet 每 ~50ms 回传的峰值驱动 --%>
        <div class="meter" title="峰值电平">
            <i id="meterL"></i><i id="meterR"></i>
        </div>
    </div>
</header>

<!-- ==========================================================================
     导出选项条：范围（单 Pattern / 整曲）与 WAV 分轨选择
     徐某把它单独放一行，是为了避免导出时误点播放，也让“分轨”这件事一眼可见
     ========================================================================== -->
<div class="exportbar">
    <span class="hint">导出范围</span>
    <select id="exportMode" class="sel">
        <option value="pattern">单 Pattern</option>
        <option value="song">整曲 Song</option>
    </select>
    <span class="hint">WAV 分轨</span>
    <select id="exportTrack" class="sel">
        <option value="-1">总混音</option>
        <%-- 服务端会复制工程、静音其它轨再离线渲染，不会污染当前工程 --%>
        <% for (int i = 0; i < boot.getCodes().length; i++) { %>
        <option value="<%= i %>"><%= boot.getCodes()[i] %> · <%= boot.getNames()[i] %>
        </option>
        <% } %>
    </select>
    <span class="hint">服务端离线渲染 · 与实时 DSP 同一套 XuEngine</span>
</div>

<main class="app">
    <!-- ========================================================================
         步进网格：16 行（声部固定）× 1–64 步
         行头由 JSP 在服务端渲染（声部代号/中文名/M/S/轨道长度），
         格子由 UI 脚本按当前 Pattern 的数据动态生成，切 Pattern 只重画格子。
         ======================================================================== -->
    <section class="panel gridPanel">
        <div class="panelHead">
            <h2>步进网格</h2>
            <div class="gridTools">
                <label class="lbl" for="stepsSel">步数</label>
                <select id="stepsSel" class="sel">
                    <option value="16">16</option>
                    <option value="32">32</option>
                    <option value="64">64</option>
                </select>
                <span class="hint">左键开关 · 右键编辑力度/概率/微时/ratchet/条件 · 点击声部名选中参数</span>
            </div>
        </div>

        <div class="gridScroll">
            <%-- 列号行在 JS 里生成，宽度与格子严格一致 --%>
            <div class="gridHead" id="gridHead"></div>
            <div class="gridRows" id="gridRows">
                <% for (int i = 0; i < boot.getCodes().length; i++) { %>
                <div class="trackRow" data-pad="<%= i %>">
                    <div class="trackHead">
                        <%-- 点击声部名 = 选中该声部，下部参数条立即切换 --%>
                        <button class="padBtn" type="button" data-pad="<%= i %>" data-act="select">
                            <b><%= boot.getCodes()[i] %>
                            </b><span><%= boot.getNames()[i] %></span>
                        </button>
                        <%-- M/S 只改原子标志，音频线程下一块就能听到变化 --%>
                        <button class="msBtn mute" type="button" data-pad="<%= i %>" data-act="mute" title="静音">M
                        </button>
                        <button class="msBtn solo" type="button" data-pad="<%= i %>" data-act="solo" title="独奏">S
                        </button>
                        <%-- 轨道长度：1–64，实现 polymeter（轨道步号 = 全局步号 % 长度） --%>
                        <input class="lenInput" type="number" min="1" max="64" step="1" value="16"
                               data-pad="<%= i %>" title="轨道长度（polymeter）">
                    </div>
                    <%-- 具体格子由 /js/xu808-ui.js 填充 --%>
                    <div class="steps" data-pad="<%= i %>"></div>
                </div>
                <% } %>
            </div>
        </div>
    </section>

    <!-- ========================================================================
         下部：左侧声部参数，右侧主效果
         ======================================================================== -->
    <div class="lower">
        <section class="panel inspector">
            <div class="panelHead">
                <h2 id="padTitle">BD · 底鼓</h2>
                <div class="padActions">
                    <button id="btnAudition" class="btn small" type="button">试听</button>
                    <button id="padMute" class="btn small ms" type="button">Mute</button>
                    <button id="padSolo" class="btn small ms" type="button">Solo</button>
                </div>
            </div>
            <%-- 参数滑块由脚本按声部类型动态生成：
                 BD/嗵鼓有 Tune/Decay/Tone，SD 有 Tune/Snappy/Decay，
                 拍手有 Decay/Tone/Tail，镲片只有 Decay/Tone，另加通用 Level/Pan --%>
            <div id="padParams" class="params"></div>
            <p class="hint" id="padHint">参数变化通过 port.postMessage 无锁送入音频线程，音频线程内做一阶平滑。</p>
        </section>

        <section class="panel master">
            <div class="panelHead">
                <h2>主效果链</h2>
                <span class="hint">HP → LP → Drive → 软限幅 → 音量</span>
            </div>
            <div class="masterGrid">
                <label class="field">主音量 <span id="mVolVal" class="val">0.80</span>
                    <input id="mVol" type="range" min="0" max="1" step="0.01" value="0.8">
                </label>
                <label class="field">Drive <span id="mDriveVal" class="val">1.35</span>
                    <input id="mDrive" type="range" min="1" max="8" step="0.05" value="1.35">
                </label>
                <label class="field checkline"><input type="checkbox" id="mHpOn" checked> 高通</label>
                <label class="field">HP 截止 <span id="mHpCutVal" class="val">28 Hz</span>
                    <input id="mHpCut" type="range" min="20" max="800" step="1" value="28">
                </label>
                <label class="field">HP Q <span id="mHpQVal" class="val">0.70</span>
                    <input id="mHpQ" type="range" min="0.5" max="8" step="0.01" value="0.7">
                </label>
                <label class="field checkline"><input type="checkbox" id="mLpOn" checked> 低通</label>
                <label class="field">LP 截止 <span id="mLpCutVal" class="val">18000 Hz</span>
                    <input id="mLpCut" type="range" min="200" max="20000" step="10" value="18000">
                </label>
                <label class="field">LP Q <span id="mLpQVal" class="val">0.90</span>
                    <input id="mLpQ" type="range" min="0.5" max="8" step="0.01" value="0.9">
                </label>
                <label class="field">拍号
                    <span class="pair">
            <select id="beatsPerBar" class="sel"></select>
            <span>/</span>
            <select id="beatUnit" class="sel"></select>
          </span>
                </label>
                <label class="field">工程名
                    <input id="projName" type="text" class="text" maxlength="60">
                </label>
            </div>
        </section>
    </div>
</main>

<footer class="foot">
    <span>XU-808 · 徐某</span>
    <span>采样率 48 kHz · 2 声道 · 16 声部 × 4 voice · AudioWorklet 实时合成</span>
    <span id="verTag"><%= boot.getVersion() %></span>
</footer>

<!-- ==========================================================================
     步进属性对话框：右键格子打开
     ========================================================================== -->
<dialog id="stepDlg" class="dlg">
    <form method="dialog" class="dlgForm" onsubmit="return false;">
        <h3 id="stepTitle">步进属性</h3>
        <div class="formGrid">
            <%-- 启用开关：右键编辑一个“关着”的格子时，可以先调参数再启用，不必先左键打开 --%>
            <label class="field checkline"><input type="checkbox" id="stOn"> 启用该步</label>
            <label class="field">力度 <span id="stVelVal" class="val">100</span>
                <input id="stVel" type="range" min="1" max="127" step="1" value="100">
            </label>
            <label class="field checkline"><input type="checkbox" id="stAccent"> 重音（音量 +35%，包络 +20%）</label>
            <label class="field">概率 <span id="stProbVal" class="val">100%</span>
                <input id="stProb" type="range" min="0" max="100" step="1" value="100">
            </label>
            <label class="field">微时（样本，±2400） <span id="stMicroVal" class="val">0</span>
                <input id="stMicro" type="range" min="-2400" max="2400" step="1" value="0">
            </label>
            <label class="field">Ratchet（1–8）
                <select id="stRatchet" class="sel"></select>
            </label>
            <label class="field">条件
                <select id="stCond" class="sel"></select>
            </label>
        </div>
        <div class="dlgBtns">
            <button id="stApply" class="btn primary" type="button">应用</button>
            <button id="stClear" class="btn" type="button">清除该步</button>
            <button id="stClose" class="btn" type="button">关闭</button>
        </div>
        <p class="hint">微时正数延后、负数提前；ratchet 把一步等分为 N 个子事件，力度每次递减 8%。</p>
    </form>
</dialog>

<!-- ==========================================================================
     Song 编排对话框：Pattern 序列 + 循环次数
     ========================================================================== -->
<dialog id="songDlg" class="dlg">
    <div class="dlgForm">
        <h3>Song 编排</h3>
        <p class="hint">播放到每一步边界时切换；整曲结束后从第一条循环。</p>
        <div id="songList" class="songList"></div>
        <div class="dlgBtns">
            <button id="btnSongAdd" class="btn" type="button">添加一条</button>
            <button id="btnSongClose" class="btn primary" type="button">完成</button>
        </div>
    </div>
</dialog>

<%-- 初始工程 JSON 由 Controller 放进 BootstrapBean；JSP 只做一次赋值。
     徐某：EL 输出的是已经转义过 < 的 JSON，不会被脚本标签截断。 --%>
<script>
    window.XU808_BASE = '${pageContext.request.contextPath}';
    window.XU808_INIT = ${boot.projectJson};
    window.XU808_VERSION = '${boot.version}';
</script>
<script src="${pageContext.request.contextPath}/js/xu808-ui.js"></script>
</body>
</html>
