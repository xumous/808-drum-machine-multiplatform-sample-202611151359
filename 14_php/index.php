<?php
// ============================================================================
//  index.php —— XU-808 PHP MVC 版前端控制器（徐某）
//
//  所有请求都从这里进入，根据 ?action= 分发到 Controller。
//  这样 URL 干净、入口唯一，也方便以后加路由。
// ============================================================================
declare(strict_types=1);

require_once __DIR__ . '/app/Model/Project.php';
require_once __DIR__ . '/app/Controller/StudioController.php';

$action = $_GET['action'] ?? 'studio';
$controller = new StudioController(__DIR__ . '/data');

switch ($action) {
    case 'studio':
        $controller->studio();
        break;
    case 'project':
        $controller->project();
        break;
    case 'save':
        $controller->save();
        break;
    case 'exportMidi':
        $controller->exportMidi();
        break;
    default:
        http_response_code(404);
        header('Content-Type: text/plain; charset=utf-8');
        echo "未知 action: " . htmlspecialchars($action, ENT_QUOTES, 'UTF-8');
}
