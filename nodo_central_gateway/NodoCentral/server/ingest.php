<?php
declare(strict_types=1);
header('Content-Type: application/json');

/* Mueve estas credenciales a un archivo fuera del DocumentRoot en produccion. */
const API_KEY = 'cambie-esta-clave';
const DB_DSN = 'mysql:host=localhost;dbname=nodo_sensor;charset=utf8mb4';
const DB_USER = 'nodo_api';
const DB_PASSWORD = 'CAMBIE_ESTA_CONTRASENA';

if ($_SERVER['REQUEST_METHOD'] !== 'POST' || !hash_equals(API_KEY, $_SERVER['HTTP_X_API_KEY'] ?? '')) {
    http_response_code(401); echo json_encode(['ok' => false]); exit;
}
$data = json_decode(file_get_contents('php://input'), true);
if (!is_array($data) || ($data['node_id'] ?? '') !== 'NodoSensor1' ||
    !is_string($data['timestamp'] ?? null) || !is_bool($data['sensor_ok'] ?? null) ||
    !array_key_exists('distance_cm', $data)) {
    http_response_code(400); echo json_encode(['ok' => false, 'error' => 'payload invalido']); exit;
}
$time = DateTimeImmutable::createFromFormat('Y-m-d\\TH:i:s\\Z', $data['timestamp'], new DateTimeZone('UTC'));
if (!$time || ($data['sensor_ok'] && (!is_numeric($data['distance_cm']) || $data['distance_cm'] < 5 || $data['distance_cm'] > 400))) {
    http_response_code(422); echo json_encode(['ok' => false, 'error' => 'lectura invalida']); exit;
}
try {
    $db = new PDO(DB_DSN, DB_USER, DB_PASSWORD, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $stmt = $db->prepare('INSERT INTO lecturas_lora (node_id, sensor_timestamp, distance_cm, sensor_ok) VALUES (?, ?, ?, ?)');
    $stmt->execute([$data['node_id'], $time->format('Y-m-d H:i:s'), $data['sensor_ok'] ? (float)$data['distance_cm'] : null, (int)$data['sensor_ok']]);
    echo json_encode(['ok' => true]);
} catch (PDOException $e) {
    error_log($e->getMessage()); http_response_code(500); echo json_encode(['ok' => false]);
}
