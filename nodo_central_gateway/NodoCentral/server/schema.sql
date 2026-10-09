CREATE DATABASE IF NOT EXISTS nodo_sensor
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE nodo_sensor;

CREATE TABLE IF NOT EXISTS lecturas_lora (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    node_id VARCHAR(32) NOT NULL,
    sensor_timestamp DATETIME NOT NULL,
    distance_cm DECIMAL(7,2) NULL,
    sensor_ok TINYINT(1) NOT NULL,
    received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    INDEX idx_node_timestamp (node_id, sensor_timestamp)
);
