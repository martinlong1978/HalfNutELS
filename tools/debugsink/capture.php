<?php
// PHP drop-in alternative to debugsink.py, for a box that already runs a web
// server. Point the device's "Debug capture URL" at wherever this is served,
// e.g. http://hass.longhome.co.uk/els/capture.php
//
// The web server de-chunks the request body before PHP sees it, so unlike the
// Python version this needs no chunked-transfer handling at all - it is the
// short version of the same job. Prefer debugsink.py if you have the choice:
// it needs no web server, and it prints the same summary line to a console you
// are probably already watching.
//
// Writes captures next to this file, in ./captures. Make that directory
// writable by the web server user.

declare(strict_types=1);

$dir = __DIR__ . '/captures';

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    header('Content-Type: text/plain');
    echo "ELS debug sink is running. POST captures here.\n";
    exit;
}

$body = file_get_contents('php://input');
if ($body === false || $body === '') {
    http_response_code(400);
    header('Content-Type: text/plain');
    echo "empty body\n";
    exit;
}

if (!is_dir($dir) && !mkdir($dir, 0775, true) && !is_dir($dir)) {
    http_response_code(500);
    header('Content-Type: text/plain');
    echo "cannot create capture directory\n";
    exit;
}

// The headers come off the wire - never build a path out of them unfiltered.
$device = $_SERVER['HTTP_X_ELS_DEVICE'] ?? 'unknown';
$device = preg_replace('/[^A-Za-z0-9._-]/', '', $device);
$device = substr($device, 0, 32);
if ($device === '') {
    $device = 'unknown';
}

$name = sprintf('capture-%s-%s.csv', date('Ymd-His'), $device);
$path = $dir . '/' . $name;

if (file_put_contents($path, $body) === false) {
    http_response_code(500);
    header('Content-Type: text/plain');
    echo "could not write capture\n";
    exit;
}

// Row count for the reply, so the device's serial log says something useful.
$rows = max(0, substr_count($body, "\n") - 1);
error_log(sprintf('ELS capture stored %s (%d rows, %d bytes)', $name, $rows, strlen($body)));

header('Content-Type: text/plain');
echo sprintf("stored %s (%d rows)\n", $name, $rows);
