<?php
// ============================================================================
//  app/Controller/StudioController.php —— 控制器（PHP MVC 的 C）
//
//  职责：接收请求、调用 Model、选择 View 或输出二进制。
//  所有输出都在这里决定，Model 不 echo，View 不查数据库。
// ============================================================================
declare(strict_types=1);

require_once __DIR__ . '/../Model/Project.php';

final class StudioController
{
    private string $dataDir;

    public function __construct(string $dataDir)
    {
        $this->dataDir = $dataDir;
    }

    /** 渲染工作室页面。 */
    public function studio(): void
    {
        $project = Project::load($this->dataDir);
        // 把工程 JSON 嵌进页面，避免首屏再发一次 AJAX（离线也更快）
        $projectJson = json_encode($project, JSON_UNESCAPED_UNICODE);
        header('Content-Type: text/html; charset=utf-8');
        require __DIR__ . '/../View/studio.php';
    }

    /** 返回当前工程 JSON（供前端刷新/调试）。 */
    public function project(): void
    {
        header('Content-Type: application/json; charset=utf-8');
        echo json_encode(Project::load($this->dataDir), JSON_UNESCAPED_UNICODE);
    }

    /** 保存工程：接受 application/json 或表单字段 project。 */
    public function save(): void
    {
        $raw = file_get_contents('php://input');
        $data = json_decode($raw ?: '', true);
        if (!is_array($data) && isset($_POST['project'])) {
            $data = json_decode((string)$_POST['project'], true);
        }
        header('Content-Type: application/json; charset=utf-8');
        if (!is_array($data)) {
            http_response_code(400);
            echo json_encode(['ok' => false, 'error' => '无效的工程 JSON']);
            return;
        }
        $ok = Project::save($this->dataDir, $data);
        echo json_encode(['ok' => $ok]);
    }

    /** 服务端导出 MIDI（浏览器直接下载）。 */
    public function exportMidi(): void
    {
        $raw = file_get_contents('php://input');
        $data = json_decode($raw ?: '', true);
        if (!is_array($data)) $data = Project::load($this->dataDir);
        $bin = Project::exportMidi(Project::normalize($data));
        header('Content-Type: audio/midi');
        header('Content-Disposition: attachment; filename="xu808.mid"');
        header('Content-Length: ' . strlen($bin));
        echo $bin;
    }
}
