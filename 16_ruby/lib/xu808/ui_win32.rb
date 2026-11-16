# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/ui_win32.rb
# 作用：XU-808（徐某）Ruby 版的 Win32 GDI 自绘界面 + 主消息循环。
# 为什么不用 Tk？——Tk 需要额外安装且字体/配色不统一；Win32 GDI 是系统自带，
# 零依赖，深色 808 风格也能一像素一像素地画出来。
# 结构：
#   顶部：LOGO、传输、BPM、Tap、Swing、步数、Song/Fill、Pattern、存读、导出；
#   中部：16 轨 × N 步网格，左键开关，右键编辑步进属性；
#   底部：所选声部的参数滑杆 + 主滤波/Drive/音量/电平表；
#   最底：状态栏。
# 线程：UI 只读引擎快照；音频线程独立运行，界面卡顿不会让声音断。
# =============================================================================
require_relative 'constants'
require_relative 'engine'
require_relative 'audio_winmm'
require_relative 'project'
require_relative 'wav'
require_relative 'midi'
require_relative 'midi_in'
require_relative 'win32'

module Xu808
  class UI
    TOP_H = 84          # 顶部控制区高度
    BOTTOM_H = 200      # 底部参数区高度
    STATUS_H = 24       # 状态栏高度
    MARGIN = 14         # 四周留白

    # 参数中文名，底部滑杆和属性面板共用。
    PARAM_NAMES = {
      tune: '音高', decay: '衰减', tone: '音色', snappy: '噪声',
      tail: '尾巴', level: '音量', pan: '声像'
    }.freeze

    # 工程/导出默认路径都放在 16_ruby 根目录，双击 run.bat 就能找到。
    PROJECT_PATH = Project.default_path
    WAV_PATH = File.join(ROOT, 'xu808_export.wav')
    MIDI_PATH = File.join(ROOT, 'xu808_export.mid')

    def self.run(auto_play: false, auto_close_ms: nil)
      # 声明 DPI 感知：否则 125%/150% 缩放下窗口会被系统整体放大，
      # 1180×780 的客户区会超出 1536×864 屏幕，底部参数栏被切掉。
      Win32.SetProcessDPIAware
      ui = new
      ui.create_window(auto_close_ms: auto_close_ms)
      ui.start_audio
      ui.engine.play if auto_play
      ui.message_loop
      puts 'UISMOKE OK' if auto_close_ms
      0
    end

    attr_reader :engine

    def initialize
      @engine = Engine.new
      @audio = WinMMAudio.new(@engine)
      @status = '就绪 · 空格播放 · 右键步进格编辑属性'
      @selected = 0
      @drag = nil
      @prop = nil
      @hits = []
      @slider_meta = {}
      @brushes = {}
      @pens = {}
      @fonts = {}
      @auto_close_at = nil
      @auto_close_ms = nil
    end

    # ---------------------------------------------------------------------
    # 窗口创建与消息循环
    # ---------------------------------------------------------------------
    def create_window(auto_close_ms: nil)
      @auto_close_ms = auto_close_ms
      @auto_close_at = Win32.now_ms + auto_close_ms if auto_close_ms

      @hinst = Win32.GetModuleHandleW(0).to_i
      @class_ptr = Fiddle::Pointer[Win32.wstr('Xu808RubyWindowClass')]
      @title_ptr = Fiddle::Pointer[Win32.wstr('XU-808 鼓机 - 徐某')]
      @font_face = Fiddle::Pointer[Win32.wstr('Microsoft YaHei UI')]

      # 窗口过程：Fiddle 回调，必须长期持有 @wndproc，否则被 GC 回收后窗口会崩。
      @wndproc = Fiddle::Closure::BlockCaller.new(
        Fiddle::TYPE_LONG_LONG,
        [Fiddle::TYPE_VOIDP, Fiddle::TYPE_INT, Fiddle::TYPE_LONG_LONG, Fiddle::TYPE_LONG_LONG]
      ) do |hwnd, msg, wparam, lparam|
        begin
          handle_message(hwnd.to_i, msg, wparam, lparam)
        rescue StandardError => e
          warn "窗口过程异常: #{e.class}: #{e.message}"
          0
        end
      end

      wc = Win32.wndclassex(@wndproc.to_i, @hinst, 0, 0, 0, @class_ptr.to_i, 0)
      atom = Win32.RegisterClassExW(wc)
      raise 'RegisterClassExW 失败' if atom.zero?

      style = Win32::WS_OVERLAPPEDWINDOW
      # DPI 虚拟化：本进程未声明 DPI aware，GetSystemMetrics 返回的是逻辑像素。
      # 这里把客户区夹到屏幕以内，避免 125% 缩放下窗口底部被任务栏/屏幕切掉。
      sw = Win32.GetSystemMetrics(0)
      sh = Win32.GetSystemMetrics(1)
      cw = [WIN_W, sw - 50].min
      ch = [WIN_H, sh - 110].min
      ch = 560 if ch < 560
      cw = 900 if cw < 900
      r = Win32.rect(0, 0, cw, ch)
      Win32.AdjustWindowRect(r, style, 0)
      l, t, rr, b = Win32.read_rect(r)
      @hwnd = Win32.CreateWindowExW(0, @class_ptr, @title_ptr, style | Win32::WS_VISIBLE,
                                    2, 2, rr - l, b - t, 0, 0, @hinst, 0).to_i
      raise 'CreateWindowExW 失败' if @hwnd.zero?

      # 图标：app.ico 放在 16_ruby 根目录，和 Python/C/Qt 版本共用同一张图。
      icon_path = File.join(ROOT, 'app.ico')
      if File.exist?(icon_path)
        icon_ptr = Fiddle::Pointer[Win32.wstr(icon_path)]
        icon = Win32.LoadImageW(0, icon_ptr, Win32::IMAGE_ICON, 0, 0,
                                Win32::LR_LOADFROMFILE | Win32::LR_DEFAULTSIZE).to_i
        unless icon.zero?
          Win32.SendMessageW(@hwnd, Win32::WM_SETICON, 0, icon)
          Win32.SendMessageW(@hwnd, Win32::WM_SETICON, 1, icon)
        end
      end

      create_fonts
      Win32.ShowWindow(@hwnd, Win32::SW_SHOW)
      Win32.UpdateWindow(@hwnd)
      Win32.SetTimer(@hwnd, Win32::TIMER_ID, 33, 0)  # 30fps 刷新
      self
    end

    def message_loop
      msg = Win32.new_msg
      loop do
        ret = Win32.GetMessageW(msg, 0, 0, 0)
        break if ret <= 0
        Win32.TranslateMessage(msg)
        Win32.DispatchMessageW(msg)
      end
      destroy_gdi
      @audio.stop if @audio.running?
    end

    # 消息分派。所有分支都返回 0，其余交给 DefWindowProcW。
    def handle_message(hwnd, msg, wparam, lparam)
      case msg
      when Win32::WM_PAINT
        on_paint(hwnd)
        0
      when Win32::WM_ERASEBKGND
        1                                     # 自己画背景，禁止系统擦除，防闪烁
      when Win32::WM_TIMER
        on_timer(hwnd)
        0
      when Win32::WM_LBUTTONDOWN
        on_mouse_down(hwnd, Win32.lparam_x(lparam), Win32.lparam_y(lparam))
        0
      when Win32::WM_LBUTTONUP
        @drag = nil
        Win32.ReleaseCapture
        0
      when Win32::WM_MOUSEMOVE
        on_mouse_move(Win32.lparam_x(lparam), Win32.lparam_y(lparam))
        0
      when Win32::WM_RBUTTONDOWN
        on_right_click(Win32.lparam_x(lparam), Win32.lparam_y(lparam))
        0
      when Win32::WM_MOUSEWHEEL
        on_wheel(Win32.wheel_delta(wparam))
        0
      when Win32::WM_KEYDOWN
        on_key(wparam)
        0
      when Win32::WM_CLOSE
        Win32.DestroyWindow(hwnd)
        0
      when Win32::WM_DESTROY
        @midi.stop if @midi
        @audio.stop if @audio.running?
        Win32.KillTimer(hwnd, Win32::TIMER_ID)
        Win32.PostQuitMessage(0)
        0
      else
        Win32.DefWindowProcW(hwnd, msg, wparam, lparam)
      end
    end

    # 30fps 定时器：只做重绘检查，不参与音频时钟。
    def on_timer(hwnd)
      if @auto_close_at && Win32.now_ms >= @auto_close_at
        Win32.DestroyWindow(hwnd)
        return
      end
      if @audio.last_error && !@audio.running? && @status !~ /音频失败/
        @status = "音频失败：#{@audio.last_error}"
      end
      Win32.InvalidateRect(hwnd, 0, 0)
    end

    # ---------------------------------------------------------------------
    # 音频
    # ---------------------------------------------------------------------
    def start_audio
      return if @audio.running?
      ok = @audio.start
      @status = ok ? '音频已启动（waveOut 6×512 帧，约 64ms）' : "音频启动失败：#{@audio.last_error}"
      start_midi if ok
    rescue StandardError => e
      @status = "音频启动失败：#{e.message}"
    end

    # MIDI 输入是可选项：没有设备就静默跳过，不影响鼓机。
    def start_midi
      @midi ||= MidiInput.new(@engine)
      @status += ' · MIDI 已连接' if @midi.start
    rescue StandardError => e
      @midi = nil
      warn "MIDI 输入不可用：#{e.message}"
    end

    def ensure_audio
      start_audio unless @audio.running?
    end

    # ---------------------------------------------------------------------
    # 绘制
    # ---------------------------------------------------------------------
    def on_paint(hwnd)
      ps = Win32.new_paintstruct
      hdc = Win32.BeginPaint(hwnd, ps).to_i
      cw, ch = client_size(hwnd)
      ensure_backbuffer(hdc, cw, ch)

      @hits = []
      @slider_meta = {}
      draw_all(@mem_dc, cw, ch)

      Win32.BitBlt(hdc, 0, 0, cw, ch, @mem_dc, 0, 0, Win32::SRCCOPY)
      Win32.EndPaint(hwnd, ps)
    end

    def client_size(hwnd)
      r = Win32.rect(0, 0, 0, 0)
      Win32.GetClientRect(hwnd, r)
      l, t, rr, b = Win32.read_rect(r)
      [rr - l, b - t]
    end

    # 双缓冲：所有绘制先落到内存位图，最后一次性 BitBlt，杜绝闪烁。
    def ensure_backbuffer(hdc, w, h)
      return if @mem_dc && @mem_w == w && @mem_h == h
      if @mem_dc
        Win32.SelectObject(@mem_dc, @old_bmp) if @old_bmp
        Win32.DeleteObject(@mem_bmp) if @mem_bmp
        Win32.DeleteObject(@mem_dc)
      end
      @mem_dc = Win32.CreateCompatibleDC(hdc).to_i
      @mem_bmp = Win32.CreateCompatibleBitmap(hdc, w, h).to_i
      @old_bmp = Win32.SelectObject(@mem_dc, @mem_bmp).to_i
      @mem_w = w
      @mem_h = h
    end

    def draw_all(hdc, w, h)
      fill(hdc, 0, 0, w, h, COLOR_BG)
      draw_top(hdc, w)
      draw_grid(hdc, w, h)
      draw_bottom(hdc, w, h)
      draw_status(hdc, w, h)
      draw_prop_panel(hdc, w, h) if @prop
    end

    # ---- GDI 小工具 -------------------------------------------------------
    def fill(hdc, x, y, w, h, color)
      r = Win32.rect(x, y, x + w, y + h)
      Win32.FillRect(hdc, r, brush(color))
    end

    def brush(color)
      @brushes[color] ||= Win32.CreateSolidBrush(color).to_i
    end

    def pen(color, width = 1)
      key = [color, width]
      @pens[key] ||= Win32.CreatePen(Win32::PS_SOLID, width, color).to_i
    end

    def outline(hdc, x, y, w, h, color, width = 1)
      fill(hdc, x, y, w, width, color)
      fill(hdc, x, y + h - width, w, width, color)
      fill(hdc, x, y, width, h, color)
      fill(hdc, x + w - width, y, width, h, color)
    end

    def text(hdc, str, x, y, w, h, font, color, align = Win32::DT_LEFT)
      Win32.SetBkMode(hdc, Win32::TRANSPARENT)
      Win32.SelectObject(hdc, font)
      Win32.SetTextColor(hdc, color)
      r = Win32.rect(x, y, x + w, y + h)
      s = Win32.wstr(str)
      Win32.DrawTextW(hdc, s, -1, r, Win32::DT_SINGLELINE | Win32::DT_VCENTER | align)
    end

    def add_hit(id, x, y, w, h)
      @hits << [id, x, y, w, h]
    end

    def hit_at(px, py)
      i = @hits.length - 1
      while i >= 0
        h = @hits[i]
        return h[0] if px >= h[1] && px < h[1] + h[3] && py >= h[2] && py < h[2] + h[4]
        i -= 1
      end
      nil
    end

    # 按钮：普通 / 激活两种配色。
    def button(hdc, id, x, y, w, h, label, active = false)
      if active
        fill(hdc, x, y, w, h, COLOR_ACCENT)
        outline(hdc, x, y, w, h, COLOR_ACCENT2)
        text(hdc, label, x, y, w, h, @font_bold, 0x00101010, Win32::DT_CENTER)
      else
        fill(hdc, x, y, w, h, COLOR_PANEL2)
        outline(hdc, x, y, w, h, COLOR_GRID)
        text(hdc, label, x, y, w, h, @font, COLOR_TEXT, Win32::DT_CENTER)
      end
      add_hit(id, x, y, w, h)
    end

    # 滑杆：返回 bar 的 [x,w]，供拖动时换算。
    def slider(hdc, id, x, y, w, label, value, min, max, display)
      text(hdc, label, x, y, 62, 20, @font_small, COLOR_TEXT_DIM)
      bx = x + 64
      bw = w - 64 - 58
      bw = 60 if bw < 40
      by = y + 8
      fill(hdc, bx, by, bw, 6, COLOR_GRID)
      frac = ((value - min).to_f / (max - min)).clamp(0.0, 1.0)
      fill(hdc, bx, by, (bw * frac).round, 6, COLOR_ACCENT)
      fill(hdc, bx + (bw * frac).round - 3, by - 3, 6, 12, COLOR_ACCENT2)
      text(hdc, display, bx + bw + 4, y, 54, 20, @font_small, COLOR_TEXT, Win32::DT_RIGHT)
      @slider_meta[id] = [bx, bw, min, max]
      add_hit(id, bx, by - 4, bw, 14)
    end

    def create_fonts
      @font = make_font(-15, 400)
      @font_bold = make_font(-16, 700)
      @font_logo = make_font(-32, 800)
      @font_small = make_font(-13, 400)
      @font_big = make_font(-22, 700)
    end

    def make_font(height, weight)
      h = Win32.CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, 1, 0, 0, 5, 0, @font_face).to_i
      @fonts[[height, weight]] = h
      h
    end

    def destroy_gdi
      @brushes.each_value { |b| Win32.DeleteObject(b) }
      @pens.each_value { |p| Win32.DeleteObject(p) }
      @fonts.each_value { |f| Win32.DeleteObject(f) }
      Win32.DeleteObject(@mem_bmp) if @mem_bmp
      Win32.DeleteObject(@mem_dc) if @mem_dc
      @brushes.clear
      @pens.clear
      @fonts.clear
    end

    # ---------------------------------------------------------------------
    # 顶部控制区
    # ---------------------------------------------------------------------
    def draw_top(hdc, w)
      fill(hdc, 0, 0, w, TOP_H, COLOR_PANEL)
      fill(hdc, 0, TOP_H - 1, w, 1, COLOR_GRID)

      text(hdc, 'XU-808', MARGIN, 6, 210, 38, @font_logo, COLOR_ACCENT)
      text(hdc, '徐某 · Ruby 4.0 · 16 VOICE DSP', MARGIN, 46, 220, 20,
           @font_small, COLOR_TEXT_DIM)

      y1 = 12
      x = 238
      button(hdc, :play, x, y1, 58, 28, '播放', @engine.playing?)
      x += 62
      button(hdc, :stop, x, y1, 58, 28, '停止', false)
      x += 70

      text(hdc, 'BPM', x, y1, 36, 28, @font_small, COLOR_TEXT_DIM)
      x += 36
      button(hdc, :bpm_dec, x, y1, 24, 28, '-', false)
      x += 26
      text(hdc, @engine.bpm.round(1).to_s, x, y1, 58, 28, @font_big, COLOR_TEXT, Win32::DT_CENTER)
      x += 58
      button(hdc, :bpm_inc, x, y1, 24, 28, '+', false)
      x += 30
      button(hdc, :tap, x, y1, 46, 28, 'TAP', false)
      x += 56

      text(hdc, 'SWING', x, y1, 48, 28, @font_small, COLOR_TEXT_DIM)
      x += 48
      button(hdc, :swing_dec, x, y1, 24, 28, '-', false)
      x += 26
      text(hdc, "#{(@engine.swing * 100).round}%", x, y1, 44, 28, @font, COLOR_TEXT, Win32::DT_CENTER)
      x += 44
      button(hdc, :swing_inc, x, y1, 24, 28, '+', false)
      x += 32

      text(hdc, 'STEPS', x, y1, 44, 28, @font_small, COLOR_TEXT_DIM)
      x += 44
      button(hdc, :steps_dec, x, y1, 24, 28, '-', false)
      x += 26
      text(hdc, @engine.pattern.length.to_s, x, y1, 30, 28, @font, COLOR_TEXT, Win32::DT_CENTER)
      x += 30
      button(hdc, :steps_inc, x, y1, 24, 28, '+', false)
      x += 34

      button(hdc, :song, x, y1, 58, 28, 'SONG', @engine.song_mode)
      x += 62
      button(hdc, :fill, x, y1, 56, 28, 'FILL', @engine.fill)

      y2 = 48
      x = 238
      text(hdc, 'PAT', x, y2, 30, 24, @font_small, COLOR_TEXT_DIM)
      x += 32
      button(hdc, :pat_prev, x, y2, 22, 24, '<', false)
      x += 24
      text(hdc, "#{@engine.current_pattern_index + 1}/#{@engine.patterns.length}",
           x, y2, 46, 24, @font, COLOR_ACCENT, Win32::DT_CENTER)
      x += 46
      button(hdc, :pat_next, x, y2, 22, 24, '>', false)
      x += 30
      button(hdc, :pat_add, x, y2, 46, 24, '新建', false)
      x += 56
      button(hdc, :save, x, y2, 52, 24, '保存', false)
      x += 56
      button(hdc, :load, x, y2, 52, 24, '读取', false)
      x += 56
      button(hdc, :wav, x, y2, 64, 24, '导出WAV', false)
      x += 68
      button(hdc, :midi, x, y2, 64, 24, '导出MIDI', false)
      x += 72

      xrun = @audio.running? ? @audio.xruns : 0
      ovr = @audio.running? ? @audio.overruns : 0
      audio_txt = @audio.running? ? "waveOut OK · ovr #{ovr} · xrun #{xrun}" : 'audio off'
      text(hdc, audio_txt, x, y2, 170, 24, @font_small,
           @audio.running? ? COLOR_OK : COLOR_WARN)
    end

    # ---------------------------------------------------------------------
    # 中部 16 轨步进网格
    # ---------------------------------------------------------------------
    def grid_metrics(w, h)
      gy = TOP_H + 30
      label_w = 104
      ms_w = 48
      cell_x = MARGIN + label_w + ms_w
      steps = @engine.pattern.length
      cell_w = ((w - cell_x - MARGIN) / steps).to_f
      cell_w = 10.0 if cell_w < 10.0
      avail = h - gy - BOTTOM_H - STATUS_H - 16
      cell_h = (avail / VOICES.length).to_f
      cell_h = 14.0 if cell_h < 14.0
      cell_h = 30.0 if cell_h > 30.0
      [gy, label_w, ms_w, cell_x, cell_w, cell_h]
    end

    def draw_grid(hdc, w, h)
      gy, label_w, ms_w, cell_x, cell_w, cell_h = grid_metrics(w, h)
      pat = @engine.pattern
      steps = pat.length
      playing = @engine.playing?
      play_step = @engine.sequencer.display_step
      grid_w = (cell_w * steps).round

      # 步号行
      s = 0
      while s < steps
        cx = cell_x + (s * cell_w).round
        num = s + 1
        if cell_w >= 22 || ((s % 4).zero?)
          text(hdc, num.to_s, cx, gy - 22, cell_w.ceil, 18, @font_small,
               (playing && play_step == s) ? COLOR_PLAYHEAD : COLOR_TEXT_DIM, Win32::DT_CENTER)
        end
        s += 1
      end

      VOICES.each_index do |t|
        y = gy + (t * cell_h).round
        row_h = cell_h.ceil
        # 声部名 + M/S
        label_bg = (t == @selected) ? 0x002A2118 : COLOR_PANEL
        fill(hdc, MARGIN, y, label_w, row_h - 1, label_bg)
        if t == @selected
          fill(hdc, MARGIN, y, 3, row_h - 1, COLOR_ACCENT)
        end
        text(hdc, "#{VOICES[t][:code]} #{VOICES[t][:name]}", MARGIN + 8, y, label_w - 10, row_h - 1,
             @font_small, t == @selected ? COLOR_ACCENT2 : COLOR_TEXT)
        add_hit([:track, t], MARGIN, y, label_w, row_h - 1)

        mx = MARGIN + label_w
        button(hdc, [:mute, t], mx, y + 2, 20, row_h - 5,
               'M', @engine.mute?(t))
        button(hdc, [:solo, t], mx + 22, y + 2, 20, row_h - 5,
               'S', @engine.solo?(t))

        s = 0
        while s < steps
          cx = cell_x + (s * cell_w).round
          step = pat.step(t, s % (pat.track_lengths[t] || steps))
          on = step.on
          bg = if on
                 COLOR_ACCENT
               elsif (s / 4).even?
                 COLOR_PANEL
               else
                 0x001A1A1A
               end
          fill(hdc, cx, y, cell_w.ceil - 1, row_h - 1, bg)
          outline(hdc, cx, y, cell_w.ceil - 1, row_h - 1, COLOR_GRID)
          # 重音：底部黄条；播放头：蓝框。
          if on && step.accent
            fill(hdc, cx + 2, y + row_h - 6, [cell_w.ceil - 5, 2].max, 3, COLOR_ACCENT2)
          end
          if playing && play_step == s
            outline(hdc, cx, y, cell_w.ceil - 1, row_h - 1, COLOR_PLAYHEAD, 2)
          end
          if on && cell_w >= 24
            text(hdc, step.velocity.to_s, cx, y, cell_w.ceil - 1, row_h - 1, @font_small,
                 0x00101010, Win32::DT_CENTER)
          end
          add_hit([:cell, t, s], cx, y, cell_w.ceil - 1, row_h - 1)
          s += 1
        end
      end

      # 网格底部留白线条
      fill(hdc, MARGIN, gy + (VOICES.length * cell_h).round + 2,
           grid_w + label_w + ms_w, 1, COLOR_GRID)
    end

    # ---------------------------------------------------------------------
    # 底部参数区
    # ---------------------------------------------------------------------
    def draw_bottom(hdc, w, h)
      by = h - STATUS_H - BOTTOM_H
      fill(hdc, 0, by, w, BOTTOM_H, COLOR_PANEL)
      fill(hdc, 0, by, w, 1, COLOR_GRID)

      keys = @engine.param_keys(@selected)
      text(hdc, "TRACK #{format('%02d', @selected + 1)}  #{VOICES[@selected][:code]} #{VOICES[@selected][:name]}",
           MARGIN, by + 6, 420, 24, @font_bold, COLOR_ACCENT2)

      # 每轨独立长度（polymeter）：按钮改的是 track_lengths[track]，
      # 网格里该轨超过长度的格子会循环回第 0 步。
      len = @engine.pattern.track_lengths[@selected]
      lx = MARGIN + 360
      text(hdc, 'LEN', lx, by + 6, 32, 24, @font_small, COLOR_TEXT_DIM)
      button(hdc, :track_len_dec, lx + 34, by + 6, 22, 22, '-', false)
      text(hdc, len.to_s, lx + 58, by + 6, 30, 22, @font, COLOR_TEXT, Win32::DT_CENTER)
      button(hdc, :track_len_inc, lx + 90, by + 6, 22, 22, '+', false)

      left_w = (w * 0.56).to_i
      col_w = (left_w - MARGIN * 2) / 2
      keys.each_with_index do |key, i|
        col = i / 4
        row = i % 4
        x = MARGIN + col * col_w
        y = by + 34 + row * 34
        v = @engine.param(@selected, key)
        slider(hdc, [:param, key], x, y, col_w - 12, PARAM_NAMES[key] || key.to_s,
               v, 0.0, 1.0, "#{(v * 100).round}%")
      end

      # 左下角补充信息，填满空白也方便用户确认当前运行参数。
      info = "48kHz · 立体声 · 512 帧/块 × 6 · 约 64ms · 16 声部 × 4 voice · Ruby #{RUBY_VERSION}"
      text(hdc, info, MARGIN, by + BOTTOM_H - 22, left_w - MARGIN, 18,
           @font_small, COLOR_TEXT_DIM)

      # 右侧主效果
      x2 = left_w + 10
      w2 = w - x2 - MARGIN
      text(hdc, 'MASTER FX', x2, by + 6, 200, 24, @font_bold, COLOR_ACCENT2)
      hp_y = by + 34
      button(hdc, :hp_toggle, x2, hp_y, 56, 22, 'HP', @engine.hp_on)
      slider(hdc, [:master, :hp_freq], x2 + 62, hp_y - 2, w2 - 62, 'HP', @engine.hp_freq,
             20.0, 800.0, "#{@engine.hp_freq.round}Hz")

      slider(hdc, [:master, :lp_freq], x2, by + 62, w2, 'LP', @engine.lp_freq,
             200.0, 20_000.0, "#{@engine.lp_freq.round}Hz")
      slider(hdc, [:master, :lp_q], x2, by + 90, w2, 'Q', @engine.lp_q,
             0.5, 8.0, format('%.2f', @engine.lp_q))
      slider(hdc, [:master, :drive], x2, by + 118, w2, 'DRIVE', @engine.drive,
             1.0, 8.0, format('%.1f', @engine.drive))
      slider(hdc, [:master, :master], x2, by + 146, w2, 'VOL', @engine.master_volume,
             0.0, 1.0, "#{(@engine.master_volume * 100).round}%")

      # 电平表：两根横条 L/R
      my = by + 174
      text(hdc, 'LEVEL', x2, my - 4, 50, 18, @font_small, COLOR_TEXT_DIM)
      meter_w = w2 - 110
      [@engine.level_l, @engine.level_r].each_with_index do |lv, i|
        yy = my + i * 9
        fill(hdc, x2 + 52, yy, meter_w, 6, COLOR_GRID)
        fw = (meter_w * lv.clamp(0.0, 1.0)).round
        col = lv > 0.95 ? COLOR_WARN : COLOR_OK
        fill(hdc, x2 + 52, yy, fw, 6, col)
      end
      text(hdc, "xrun #{@engine.xrun}", x2 + 52 + meter_w + 6, my - 4, 70, 18,
           @font_small, @engine.xrun.positive? ? COLOR_WARN : COLOR_TEXT_DIM)
    end

    def draw_status(hdc, w, h)
      y = h - STATUS_H
      fill(hdc, 0, y, w, STATUS_H, 0x00101010)
      fill(hdc, 0, y, w, 1, COLOR_GRID)
      text(hdc, @status, MARGIN, y, (w * 0.62).to_i, STATUS_H, @font_small, COLOR_TEXT)
      text(hdc, '空格 播放/停止 · 右键格子编辑属性 · 滚轮 BPM · 点击声部名试听',
           (w * 0.45).to_i, y, (w * 0.54).to_i - MARGIN, STATUS_H,
           @font_small, COLOR_TEXT_DIM, Win32::DT_RIGHT)
    end

    # ---------------------------------------------------------------------
    # 步进属性面板（右键打开）
    # ---------------------------------------------------------------------
    def prop_step
      return nil unless @prop
      pat = @engine.pattern
      t = @prop[:track]
      s = @prop[:step] % (pat.track_lengths[t] || pat.length)
      pat.step(t, s)
    end

    def draw_prop_panel(hdc, w, h)
      step = prop_step
      return unless step
      pw = 540
      ph = 330
      x = (w - pw) / 2
      y = (h - ph) / 2
      # 先铺一层“面板背景”命中区，防止点面板空白处时穿透到下面的网格格子。
      add_hit([:prop, :panel], x, y, pw, ph)
      fill(hdc, x, y, pw, ph, COLOR_PANEL2)
      outline(hdc, x, y, pw, ph, COLOR_ACCENT, 2)
      t = @prop[:track]
      s = @prop[:step]
      text(hdc, "STEP 属性 · TRACK #{format('%02d', t + 1)} #{VOICES[t][:code]} · STEP #{s + 1}",
           x + 16, y + 8, pw - 32, 28, @font_bold, COLOR_ACCENT2)

      slider(hdc, [:prop, :velocity], x + 16, y + 48, pw - 32, '力度',
             step.velocity, 1.0, 127.0, step.velocity.to_s)
      button(hdc, [:prop, :accent], x + 16, y + 80, 120, 24, '重音 ACCENT', step.accent)
      slider(hdc, [:prop, :probability], x + 16, y + 112, pw - 32, '概率',
             step.probability, 0.0, 100.0, "#{step.probability}%")
      slider(hdc, [:prop, :micro], x + 16, y + 144, pw - 32, '微时(样本)',
             step.micro, -2400.0, 2400.0, step.micro.to_s)

      text(hdc, 'RATCHET', x + 16, y + 176, 80, 24, @font_small, COLOR_TEXT_DIM)
      button(hdc, [:prop, :ratchet_dec], x + 100, y + 174, 26, 24, '-', false)
      text(hdc, step.ratchet.to_s, x + 128, y + 174, 34, 24, @font, COLOR_TEXT, Win32::DT_CENTER)
      button(hdc, [:prop, :ratchet_inc], x + 164, y + 174, 26, 24, '+', false)

      text(hdc, '条件', x + 220, y + 176, 40, 24, @font_small, COLOR_TEXT_DIM)
      button(hdc, [:prop, :condition], x + 262, y + 174, 130, 24,
             CONDITION_NAMES[step.condition] || step.condition, false)

      button(hdc, [:prop, :close], x + pw - 116, y + ph - 44, 100, 30, '关闭', false)
      text(hdc, '修改立即生效；播放中也能改，下一步就听到。',
           x + 16, y + ph - 44, pw - 150, 30, @font_small, COLOR_TEXT_DIM)
    end

    # ---------------------------------------------------------------------
    # 鼠标 / 键盘
    # ---------------------------------------------------------------------
    def on_mouse_down(hwnd, px, py)
      id = hit_at(px, py)
      return unless id

      # 滑杆优先：按下即跳到点击位置，并开始拖动。
      if (meta = @slider_meta[id])
        @drag = { id: id, x: meta[0], w: meta[1], min: meta[2], max: meta[3] }
        apply_drag(px)
        Win32.SetCapture(hwnd)
        return
      end

      if id.is_a?(Array)
        case id[0]
        when :cell
          toggle_step(id[1], id[2])
        when :mute
          @engine.toggle_mute(id[1])
        when :solo
          @engine.toggle_solo(id[1])
        when :track
          @selected = id[1]
          ensure_audio
          @engine.trigger(@selected, 110, false)
        when :prop
          prop_action(id[1]) unless id[1] == :panel
        when :master
          # 理论上滑杆已在上面处理；这里是兜底。
        end
        return
      end

      case id
      when :play
        ensure_audio
        @engine.play
        @status = '播放中…'
      when :stop
        @engine.stop
        @status = '已停止（余音自然衰减）'
      when :bpm_dec then @engine.bpm = (@engine.bpm - 1).clamp(40.0, 300.0)
      when :bpm_inc then @engine.bpm = (@engine.bpm + 1).clamp(40.0, 300.0)
      when :tap
        @engine.tap!
        @status = "Tap Tempo → #{@engine.bpm.round(1)} BPM"
      when :swing_dec then @engine.swing = (@engine.swing - 0.01).clamp(0.0, 0.75)
      when :swing_inc then @engine.swing = (@engine.swing + 0.01).clamp(0.0, 0.75)
      when :steps_dec then cycle_steps(-1)
      when :steps_inc then cycle_steps(1)
      when :song
        @engine.song_mode = !@engine.song_mode
        # 第一次打开 Song 时，把所有 pattern 串成链（每个重复 2 遍），
        # 这样“新建 pattern”之后切到 Song 就能直接听到整首歌。
        if @engine.song_mode && @engine.patterns.length > 1
          @engine.song = (0...@engine.patterns.length).map { |i| { pattern: i, repeats: 2 } }
        end
      when :fill then @engine.fill = !@engine.fill
      when :track_len_dec
        tl = @engine.pattern.track_lengths[@selected]
        @engine.pattern.track_lengths[@selected] = (tl - 1).clamp(1, 64)
      when :track_len_inc
        tl = @engine.pattern.track_lengths[@selected]
        @engine.pattern.track_lengths[@selected] = (tl + 1).clamp(1, 64)
      when :pat_prev then @engine.select_pattern((@engine.current_pattern_index - 1) % @engine.patterns.length)
      when :pat_next then @engine.select_pattern((@engine.current_pattern_index + 1) % @engine.patterns.length)
      when :pat_add
        @engine.select_pattern(@engine.add_pattern)
        @status = "已新建 PATTERN #{@engine.patterns.length}"
      when :save then action_save
      when :load then action_load
      when :wav then action_export_wav
      when :midi then action_export_midi
      when :hp_toggle then @engine.hp_on = !@engine.hp_on
      end
    end

    def on_mouse_move(px, _py)
      apply_drag(px) if @drag
    end

    def apply_drag(px)
      d = @drag
      frac = ((px - d[:x]).to_f / d[:w]).clamp(0.0, 1.0)
      val = d[:min] + (d[:max] - d[:min]) * frac
      id = d[:id]
      case id[0]
      when :param
        @engine.set_param(@selected, id[1], val)
      when :master
        case id[1]
        when :hp_freq then @engine.hp_freq = val
        when :lp_freq then @engine.lp_freq = val
        when :lp_q then @engine.lp_q = val
        when :drive then @engine.drive = val
        when :master then @engine.master_volume = val
        end
      when :prop
        step = prop_step
        return unless step
        case id[1]
        when :velocity then step.velocity = val.round.clamp(1, 127)
        when :probability then step.probability = val.round.clamp(0, 100)
        when :micro then step.micro = val.round.clamp(-2400, 2400)
        end
      end
    end

    def on_right_click(px, py)
      id = hit_at(px, py)
      return unless id.is_a?(Array) && id[0] == :cell
      @prop = { track: id[1], step: id[2] }
      @selected = id[1]
    end

    def on_wheel(delta)
      @engine.bpm = (@engine.bpm + (delta.positive? ? 1 : -1)).clamp(40.0, 300.0)
    end

    def on_key(vk)
      case vk
      when 32 # 空格
        ensure_audio
        @engine.toggle_play
        @status = @engine.playing? ? '播放中…' : '已停止'
      when 27 # ESC
        @prop = nil
      end
    end

    # ---------------------------------------------------------------------
    # 交互动作
    # ---------------------------------------------------------------------
    def toggle_step(track, s)
      pat = @engine.pattern
      step = pat.step(track, s % (pat.track_lengths[track] || pat.length))
      step.on = !step.on
      @selected = track
      ensure_audio
      if step.on
        @engine.trigger(track, step.velocity, step.accent)
        @status = "TRACK #{track + 1} STEP #{s + 1} 开 · 力度 #{step.velocity}"
      else
        @status = "TRACK #{track + 1} STEP #{s + 1} 关"
      end
    end

    def prop_action(key)
      step = prop_step
      return unless step
      case key
      when :accent then step.accent = !step.accent
      when :ratchet_dec then step.ratchet = (step.ratchet - 1).clamp(1, 8)
      when :ratchet_inc then step.ratchet = (step.ratchet + 1).clamp(1, 8)
      when :condition
        idx = CONDITIONS.index(step.condition) || 0
        step.condition = CONDITIONS[(idx + 1) % CONDITIONS.length]
      when :close then @prop = nil
      end
    end

    def cycle_steps(dir)
      lengths = [16, 32, 64]
      cur = @engine.pattern.length
      idx = lengths.index(cur) || 0
      @engine.pattern.length = lengths[(idx + dir).clamp(0, lengths.length - 1)]
      @status = "Pattern 长度 → #{@engine.pattern.length} 步"
    end

    def action_save
      path = Project.save(@engine, PROJECT_PATH)
      @status = "工程已保存：#{path}"
    rescue StandardError => e
      @status = "保存失败：#{e.message}"
    end

    def action_load
      old = @engine
      old.stop
      @midi.stop if @midi
      @midi = nil
      @audio.stop
      new_engine = Project.load(PROJECT_PATH)
      @engine = new_engine
      @audio = WinMMAudio.new(@engine)
      @selected = 0
      @prop = nil
      ensure_audio
      @status = "工程已读取：#{PROJECT_PATH}"
    rescue StandardError => e
      @status = "读取失败：#{e.message}"
    end

    # 用工程 JSON 做一次深拷贝，导出离线渲染不会打断正在播放的引擎。
    def clone_engine
      tmp = File.join(ROOT, '.xu808_export_tmp.json')
      Project.save(@engine, tmp)
      copy = Project.load(tmp)
      File.delete(tmp) if File.exist?(tmp)
      copy
    end

    def export_seconds(eng)
      step_sec = 60.0 / eng.bpm / 4.0
      if eng.song_mode
        total = eng.song.sum { |e| (eng.patterns[e[:pattern]]&.length || 16) * e[:repeats].to_i }
        (total * step_sec).clamp(1.0, 60.0)
      else
        # 单 pattern 导出 4 遍，足够听清节奏与尾音。
        (eng.pattern.length * 4 * step_sec).clamp(1.0, 60.0)
      end
    end

    def action_export_wav
      eng = clone_engine
      eng.play
      secs = export_seconds(eng)
      _path, size, peak = Wav.render(eng, WAV_PATH, secs)
      @status = "WAV 已导出：#{WAV_PATH}（#{size} 字节，峰值 #{format('%.3f', peak)}）"
    rescue StandardError => e
      @status = "WAV 导出失败：#{e.message}"
    end

    def action_export_midi
      Midi.export(@engine, MIDI_PATH, pattern_index: @engine.current_pattern_index)
      @status = "MIDI 已导出：#{MIDI_PATH}（#{File.size(MIDI_PATH)} 字节）"
    rescue StandardError => e
      @status = "MIDI 导出失败：#{e.message}"
    end
  end
end
