<?php
// ============================================================================
//  app/View/studio.php —— 视图（PHP MVC 的 V）
//
//  只负责输出 HTML；所有数据来自控制器传入的 $projectJson。
//  JS/CSS 放在 public/ 下，由 PHP 内置服务器直接当静态文件返回。
// ============================================================================
declare(strict_types=1);
?>
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <title>XU-808 鼓机 - 徐某 (PHP MVC)</title>
  <link rel="stylesheet" href="public/css/style.css">
  <script>
    // 服务端把工程直接嵌进页面：首屏即可显示默认 808 律动
    window.__INITIAL_PROJECT__ = <?= $projectJson ?>;
  </script>
</head>
<body>
  <header id="top">
    <div class="logo">
      <div class="logo-main">XU-808</div>
      <div class="logo-sub">RHYTHM COMPOSER · 徐某 · 16 VOICE ANALOG · PHP MVC</div>
    </div>

    <button id="btnPlay" class="big">▶ PLAY</button>
    <button id="btnStop" class="big">■ STOP</button>

    <div class="group">
      <label>BPM</label>
      <div class="row">
        <input id="bpm" type="number" min="40" max="300" step="0.5" value="120">
        <input id="bpmRange" type="range" min="40" max="300" step="1" value="120">
      </div>
    </div>

    <button id="btnTap" class="big small">TAP</button>

    <div class="group">
      <label>SWING</label>
      <div class="row">
        <input id="swing" type="range" min="0" max="75" step="1" value="0">
        <span id="swingLabel">0%</span>
      </div>
    </div>

    <div class="group">
      <label>PATTERN</label>
      <div class="row">
        <select id="pattern"></select>
        <label class="inline">步数</label>
        <input id="steps" type="number" min="1" max="64" value="16">
      </div>
    </div>

    <label class="check"><input id="songMode" type="checkbox"> SONG</label>
    <button id="btnSong">歌曲链…</button>

    <div class="spacer"></div>
    <label class="check"><input id="midiClock" type="checkbox"> MIDI CLK</label>
    <span id="midiStatus" class="dim">MIDI: -</span>
  </header>

  <section id="gridScroll"><div id="grid"></div></section>
  <section id="stripsScroll"><div id="strips"></div></section>

  <footer id="master">
    <div class="master-title">MASTER · 徐某的母线</div>
    <div class="master-slider"><label>LP 低通</label><input id="lp" type="range" min="200" max="20000" value="18000"><span id="lpV">18000</span></div>
    <div class="master-slider"><label>RES 共鸣</label><input id="rez" type="range" min="50" max="800" value="90"><span id="rezV">0.90</span></div>
    <div class="master-slider"><label>HP 高通</label><input id="hp" type="range" min="10" max="800" value="28"><span id="hpV">28</span></div>
    <div class="master-slider"><label>DRIVE</label><input id="drive" type="range" min="100" max="800" value="135"><span id="driveV">1.35</span></div>
    <div class="master-slider"><label>VOL</label><input id="vol" type="range" min="0" max="100" value="80"><span id="volV">80</span></div>
    <div class="meter"><div class="meter-bar"><i id="meterL"></i></div><div class="meter-bar"><i id="meterR"></i></div></div>
    <span id="status" class="dim">CPU -</span>
    <button id="btnSave">保存到服务器</button>
    <button id="btnLoad">从文件打开</button>
    <button id="btnWav">导出 WAV</button>
    <button id="btnMidi">导出 MIDI</button>
    <input id="fileLoad" type="file" accept=".xu808,.json" hidden>
  </footer>

  <div id="modal" class="modal hidden">
    <div class="modal-box">
      <h3 id="modalTitle">步进属性</h3>
      <div class="form">
        <label><input id="mOn" type="checkbox"> 启用</label>
        <label><input id="mAcc" type="checkbox"> 重音 Accent</label>
        <label>力度 Velocity <input id="mVel" type="number" min="1" max="127"></label>
        <label>概率 Probability <input id="mProb" type="number" min="0" max="100"></label>
        <label>微时 Micro（样本） <input id="mMicro" type="number" min="-2400" max="2400"></label>
        <label>滚奏 Ratchet <input id="mRat" type="number" min="1" max="8"></label>
        <label>条件触发 <select id="mCond"></select></label>
      </div>
      <div class="modal-buttons">
        <button id="mOk">确定</button>
        <button id="mCancel">取消</button>
      </div>
    </div>
  </div>

  <!-- Web Audio 合成放在 AudioWorklet 里，脚本由 PHP 静态托管 -->
  <script src="public/js/app.js"></script>
</body>
</html>
