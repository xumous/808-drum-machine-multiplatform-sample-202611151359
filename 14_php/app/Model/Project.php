<?php
// ============================================================================
//  app/Model/Project.php —— 工程数据模型（PHP 版）
//
//  Model 只做三件事：造默认工程、读写 JSON、导出 MIDI。
//  它不知道 HTML 长什么样，也不直接输出任何东西——这就是 MVC 的 M。
// ============================================================================
declare(strict_types=1);

final class Project
{
    public const PADS = 16;
    public const MAX_STEPS = 64;
    public const PATTERNS = 16;

    public const CODES = ['BD','SD','LT','MT','HT','LC','MC','HC','RS','CL','CP','MA','CB','CY','OH','CH'];
    public const NAMES_CN = ['底鼓','军鼓','低嗵','中嗵','高嗵','低康加','中康加','高康加',
                             '边击','击木','拍手','沙锤','牛铃','吊镲','开镲','闭镲'];
    public const MIDI_NOTES = [36,38,45,47,50,64,62,60,37,75,39,70,56,49,46,42];

    /** 一个空步进。 */
    public static function step(): array
    {
        return [
            'on' => false, 'velocity' => 100, 'accent' => false, 'prob' => 100,
            'micro' => 0, 'ratchet' => 1, 'cond' => 0
        ];
    }

    /** 一个空 pattern（16 轨 × 64 步）。 */
    public static function pattern(int $idx): array
    {
        $data = [];
        for ($p = 0; $p < self::PADS; $p++) {
            $row = [];
            for ($s = 0; $s < self::MAX_STEPS; $s++) $row[] = self::step();
            $data[] = $row;
        }
        return [
            'name' => 'PATTERN ' . ($idx + 1),
            'steps' => 16,
            'fill' => false,
            'trackLen' => array_fill(0, self::PADS, 16),
            'data' => $data
        ];
    }

    /** 默认演示律动：一打开就有声音。 */
    public static function demoPattern(): array
    {
        $pat = self::pattern(0);
        $set = function (int $p, int $s, int $v, bool $a = false) use (&$pat): void {
            $pat['data'][$p][$s]['on'] = true;
            $pat['data'][$p][$s]['velocity'] = $v;
            $pat['data'][$p][$s]['accent'] = $a;
        };
        $set(0, 0, 118, true); $set(0, 8, 105); $set(0, 14, 90);
        $set(1, 4, 110); $set(1, 12, 110, true);
        for ($s = 0; $s < 16; $s += 2) $set(15, $s, $s % 4 === 0 ? 96 : 84);
        $set(14, 14, 92);
        $set(10, 12, 80);
        for ($s = 1; $s < 16; $s += 2) $set(11, $s, 42);
        $set(0, 15, 88);
        $pat['data'][0][15]['ratchet'] = 3;
        return $pat;
    }

    public static function createDefault(): array
    {
        $patterns = [];
        for ($i = 0; $i < self::PATTERNS; $i++) $patterns[] = self::pattern($i);
        $patterns[0] = self::demoPattern();

        $pads = [];
        for ($i = 0; $i < self::PADS; $i++) {
            $pads[] = [
                'tune' => 0.5, 'decay' => 0.5, 'tone' => 0.5,
                'snappy' => 0.55, 'tail' => 0.45, 'level' => 0.75, 'pan' => 0.5,
                'mute' => false, 'solo' => false
            ];
        }
        $pads[0]['decay'] = 0.55;
        $pads[0]['tone'] = 0.45;

        return [
            'format' => 'XU808', 'version' => 1,
            'name' => '徐某的 808 工程', 'seed' => 0x808808, 'current' => 0,
            'master' => [
                'volume' => 0.80, 'drive' => 1.35,
                'lpCutoff' => 18000, 'lpQ' => 0.90,
                'hpCutoff' => 28, 'hpQ' => 0.70,
                'lpOn' => true, 'hpOn' => true,
                'bpm' => 120, 'swing' => 0,
                'beatsPerBar' => 4, 'beatUnit' => 4, 'songMode' => false
            ],
            'pads' => $pads,
            'patterns' => $patterns,
            'song' => ['enabled' => false, 'entries' => [
                ['pattern' => 0, 'repeats' => 4],
                ['pattern' => 0, 'repeats' => 4]
            ]]
        ];
    }

    /** 读取工程；文件不存在或损坏时回退默认值，绝不白屏。 */
    public static function load(string $dataDir): array
    {
        $file = $dataDir . '/project.json';
        if (!is_file($file)) return self::createDefault();
        $text = @file_get_contents($file);
        if ($text === false || trim($text) === '') return self::createDefault();
        $p = json_decode($text, true);
        if (!is_array($p)) return self::createDefault();
        return self::normalize($p);
    }

    /** 缺字段补默认值，保证旧工程能打开。 */
    public static function normalize(array $p): array
    {
        $base = self::createDefault();
        $out = array_replace_recursive($base, $p);
        $out['current'] = max(0, min(self::PATTERNS - 1, (int)($p['current'] ?? 0)));
        $out['master']['bpm'] = max(40, min(300, (float)($out['master']['bpm'] ?? 120)));
        if (!isset($out['song']['entries']) || !is_array($out['song']['entries'])) {
            $out['song']['entries'] = [['pattern' => 0, 'repeats' => 4]];
        }
        return $out;
    }

    public static function save(string $dataDir, array $p): bool
    {
        if (!is_dir($dataDir)) @mkdir($dataDir, 0777, true);
        $p = self::normalize($p);
        $json = json_encode($p, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT);
        if ($json === false) return false;
        return file_put_contents($dataDir . '/project.json', $json) !== false;
    }

    // ------------------------------------------------------------------
    // MIDI 导出：SMF Format 1，PPQ=480
    // ------------------------------------------------------------------
    private static function vlq(int $v): string
    {
        $v = max(0, $v);
        $out = chr($v & 0x7F);
        $v >>= 7;
        while ($v > 0) {
            $out = chr(($v & 0x7F) | 0x80) . $out;
            $v >>= 7;
        }
        return $out;
    }

    public static function exportMidi(array $p): string
    {
        $ppq = 480;
        $sr = 48000;
        $master = $p['master'];
        $bpm = max(40.0, min(300.0, (float)$master['bpm']));
        $stepSamples = $sr * 60.0 / $bpm / 4.0;

        $seq = [];
        if (!empty($master['songMode']) && !empty($p['song']['entries'])) {
            foreach ($p['song']['entries'] as $e) {
                $pat = max(0, min(self::PATTERNS - 1, (int)$e['pattern']));
                for ($k = 0; $k < max(1, (int)$e['repeats']); $k++) $seq[] = $pat;
            }
        } else {
            $seq[] = (int)$p['current'];
        }

        $notes = [];
        $cursor = 0;
        foreach ($seq as $pi) {
            $pat = $p['patterns'][$pi];
            for ($s = 0; $s < (int)$pat['steps']; $s++) {
                $swingTick = ($s % 2) ? (int)round(((float)$master['swing']) * 60) : 0;
                for ($pad = 0; $pad < self::PADS; $pad++) {
                    $tl = max(1, min(64, (int)($pat['trackLen'][$pad] ?? 16)));
                    $idx = (int)(($cursor + $s) % $tl);
                    $st = $pat['data'][$pad][$idx];
                    if (empty($st['on']) || (int)$st['prob'] <= 0) continue;
                    $vel = (int)round(((int)$st['velocity']) * (!empty($st['accent']) ? 1.3 : 1.0));
                    $vel = max(1, min(127, $vel));
                    $base = (int)(($cursor + $s) * 120);
                    $micro = (int)round(((int)$st['micro']) / $stepSamples * 120.0);
                    $r = max(1, min(8, (int)$st['ratchet']));
                    for ($i = 0; $i < $r; $i++) {
                        $tick = max(0, $base + $swingTick + $micro + (int)($i * 120.0 / $r));
                        $notes[] = [$tick, self::MIDI_NOTES[$pad], $vel];
                    }
                }
            }
            $cursor += (int)$pat['steps'];
        }
        usort($notes, function ($a, $b) {
            return $a[0] !== $b[0] ? $a[0] <=> $b[0] : $a[1] <=> $b[1];
        });

        $track = '';
        $name = 'XU-808 by XuMou';
        $track .= self::vlq(0) . "\xFF\x03" . self::vlq(strlen($name)) . $name;
        $tempo = (int)(60000000.0 / $bpm);
        $track .= self::vlq(0) . "\xFF\x51\x03"
                . chr(($tempo >> 16) & 0xFF) . chr(($tempo >> 8) & 0xFF) . chr($tempo & 0xFF);
        $track .= self::vlq(0) . "\xFF\x58\x04"
                . chr((int)$master['beatsPerBar'])
                . chr((int)round(log(max(1, (int)$master['beatUnit']), 2)))
                . chr(24) . chr(8);
        $last = 0;
        foreach ($notes as $n) {
            $track .= self::vlq($n[0] - $last) . chr(0x99) . chr($n[1]) . chr($n[2]);
            $track .= self::vlq(10) . chr(0x89) . chr($n[1]) . chr(0x40);
            $last = $n[0] + 10;
        }
        $track .= self::vlq(0) . "\xFF\x2F\x00";

        return 'MThd' . pack('N', 6) . pack('n', 1) . pack('n', 1) . pack('n', $ppq)
             . 'MTrk' . pack('N', strlen($track)) . $track;
    }
}
