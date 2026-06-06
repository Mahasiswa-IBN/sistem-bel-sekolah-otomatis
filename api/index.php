<?php
header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");
header("Access-Control-Allow-Headers: Content-Type");
header("Access-Control-Allow-Methods: GET, POST");

// Tentukan rute dari query string (dari .htaccess) atau deteksi manual
$route = $_GET['route'] ?? '';
if (empty($route)) {
    // Fallback jika tidak menggunakan htaccess: api/index.php?route=status
    // Atau potong URL manual
    $requestUri = $_SERVER['REQUEST_URI'];
    $scriptName = $_SERVER['SCRIPT_NAME'];
    $basePath = dirname($scriptName);
    
    // Potong base path dari request URI
    $path = str_replace($basePath, '', $requestUri);
    $path = ltrim(parse_url($path, PHP_URL_PATH), '/');
    $route = $path;
}

$schedulesFile = __DIR__ . '/../schedules.json';
$configFile = __DIR__ . '/../config.json';
$statsFile = __DIR__ . '/../stats.json';

// Inisialisasi file stats jika belum ada
if (!file_exists($statsFile)) {
    $defaultStats = [
        "bells_today" => 12,
        "success_rate" => "12/12",
        "uptime" => "0d 04h 32m",
        "last_active" => "Hari ini - 10:00 WIB"
    ];
    file_put_contents($statsFile, json_encode($defaultStats));
}

// Inisialisasi file config jika belum ada
if (!file_exists($configFile)) {
    $defaultConfig = [
        "wifi_ssid" => "Localhost XAMPP",
        "wifi_password" => "12345678",
        "is_ap" => true
    ];
    file_put_contents($configFile, json_encode($defaultConfig));
}

// Inisialisasi schedules jika belum ada
if (!file_exists($schedulesFile)) {
    $defaultSchedules = [
        ["id" => 1, "name" => "Bel Masuk Pagi", "time" => "07:00", "days" => [1,2,3,4,5,6], "folder" => 1, "file" => 1, "active" => 1],
        ["id" => 2, "name" => "Bel Masuk Jam Pelajaran", "time" => "07:30", "days" => [1,2,3,4,5,6], "folder" => 1, "file" => 2, "active" => 1],
        ["id" => 3, "name" => "Bel Istirahat Pertama", "time" => "10:00", "days" => [1,2,3,4,5], "folder" => 1, "file" => 3, "active" => 1],
        ["id" => 4, "name" => "Bel Pulang Sekolah", "time" => "14:00", "days" => [1,2,3,4,5], "folder" => 1, "file" => 4, "active" => 1]
    ];
    file_put_contents($schedulesFile, json_encode($defaultSchedules, JSON_PRETTY_PRINT));
}

// Baca POST Body JSON jika ada
$input = json_decode(file_get_contents('php://input'), true);

switch ($route) {
    case 'status':
        $stats = json_decode(file_get_contents($statsFile), true);
        $config = json_decode(file_get_contents($configFile), true);
        $schedules = json_decode(file_get_contents($schedulesFile), true);
        
        // Dapatkan waktu saat ini dari PC
        $now = new DateTime();
        
        // Map PHP day 0 (Sunday) to 7
        $dayOfWeek = $now->format('w');
        if ($dayOfWeek == 0) $dayOfWeek = 7;
        
        echo json_encode([
            "time" => $now->format('H:i:s'),
            "date" => $now->format('Y-m-d'),
            "day" => (int)$dayOfWeek,
            "system_status" => "OK",
            "wifi_ssid" => $config['wifi_ssid'] ?? 'XAMPP_WiFi',
            "wifi_signal" => "Mode Simulasi (XAMPP)",
            "bells_today" => (int)$stats['bells_today'],
            "success_rate" => $stats['success_rate'],
            "uptime" => $stats['uptime'],
            "last_active" => $stats['last_active'],
            "total_schedules" => count($schedules)
        ]);
        break;

    case 'schedules':
        echo file_get_contents($schedulesFile);
        break;

    case 'schedules/save':
        if (!$input) {
            http_response_code(400);
            echo json_encode(["status" => "error", "message" => "POST data tidak valid"]);
            exit;
        }
        
        $schedules = json_decode(file_get_contents($schedulesFile), true);
        $id = $input['id'] ?? 0;
        
        if ($id === 0) {
            // Generate ID baru
            $maxId = 0;
            foreach ($schedules as $s) {
                if (($s['id'] ?? 0) > $maxId) $maxId = $s['id'];
            }
            $input['id'] = $maxId + 1;
            $schedules[] = $input;
        } else {
            // Update
            $found = false;
            foreach ($schedules as $index => $s) {
                if (($s['id'] ?? 0) == $id) {
                    $schedules[$index] = $input;
                    $found = true;
                    break;
                }
            }
            if (!$found) {
                $schedules[] = $input;
            }
        }
        
        file_put_contents($schedulesFile, json_encode($schedules, JSON_PRETTY_PRINT));
        echo json_encode(["status" => "success", "message" => "Jadwal berhasil disimpan"]);
        break;

    case 'schedules/delete':
        if (!$input || !isset($input['id'])) {
            http_response_code(400);
            echo json_encode(["status" => "error", "message" => "POST data tidak valid"]);
            exit;
        }
        
        $id = $input['id'];
        $schedules = json_decode(file_get_contents($schedulesFile), true);
        
        $newSchedules = [];
        foreach ($schedules as $s) {
            if (($s['id'] ?? 0) != $id) {
                $newSchedules[] = $s;
            }
        }
        
        file_put_contents($schedulesFile, json_encode($newSchedules, JSON_PRETTY_PRINT));
        echo json_encode(["status" => "success", "message" => "Jadwal berhasil dihapus"]);
        break;

    case 'play':
        if (!$input) {
            http_response_code(400);
            echo json_encode(["status" => "error", "message" => "POST data tidak valid"]);
            exit;
        }
        
        $folder = $input['folder'] ?? 1;
        $file = $input['file'] ?? 1;
        
        // Simulasikan bel berbunyi di XAMPP dengan mengupdate stats
        $stats = json_decode(file_get_contents($statsFile), true);
        $stats['bells_today'] = ($stats['bells_today'] ?? 0) + 1;
        
        // Keberhasilan bertambah
        $parts = explode('/', $stats['success_rate']);
        $num = (int)$parts[0] + 1;
        $den = (int)($parts[1] ?? 0) + 1;
        $stats['success_rate'] = "$num/$den";
        
        $now = new DateTime();
        $stats['last_active'] = "Hari ini - " . $now->format('H:i') . " WIB";
        
        file_put_contents($statsFile, json_encode($stats, JSON_PRETTY_PRINT));
        
        echo json_encode([
            "status" => "success", 
            "message" => "Folder $folder File $file berhasil dipicu (Simulasi XAMPP)"
        ]);
        break;

    case 'settings/time':
        if (!$input || !isset($input['datetime'])) {
            http_response_code(400);
            echo json_encode(["status" => "error", "message" => "POST data tidak valid"]);
            exit;
        }
        
        echo json_encode(["status" => "success", "message" => "Waktu RTC disinkronkan dengan " . $input['datetime']]);
        break;

    case 'settings/wifi':
        if (!$input || !isset($input['ssid'])) {
            http_response_code(400);
            echo json_encode(["status" => "error", "message" => "POST data tidak valid"]);
            exit;
        }
        
        $config = [
            "wifi_ssid" => $input['ssid'],
            "wifi_password" => $input['password'] ?? '',
            "is_ap" => false
        ];
        
        file_put_contents($configFile, json_encode($config, JSON_PRETTY_PRINT));
        echo json_encode(["status" => "success", "message" => "Koneksi Wi-Fi berhasil disimpan"]);
        break;

    default:
        http_response_code(404);
        echo json_encode(["status" => "error", "message" => "Endpoint '$route' tidak ditemukan"]);
        break;
}
