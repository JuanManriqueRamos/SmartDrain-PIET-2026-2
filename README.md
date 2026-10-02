# SmartDrain — Monitoreo Inteligente de Obstrucciones en el Alcantarillado

**Título técnico:** Monitoreo de condiciones asociadas a obstrucción para apoyar el mantenimiento preventivo del drenaje urbano.  
**Curso:** Proyecto Integrador | Ingeniería Electrónica y Telecomunicaciones (2026-2)  
**Docentes:** Mary Cristina Carrascal R. / Fernando Aparicio Urbano  

## 1. Descripción y Arquitectura Preliminar
Este repositorio contiene el código fuente, diseños de hardware, contratos de interfaz y registros crudos de pruebas del proyecto **SmartDrain**, enfocado en la problemática de inundaciones y obstrucciones en el alcantarillado urbano (caso de estudio: sector Campanario, Popayán).

La arquitectura funcional se divide en cuatro bloques:
1. **Entorno / Fenómeno:** Drenaje de aguas lluvias en alcantarillado urbano.
2. **Nodos Sensores Inalámbricos (A1 y A2):** Medición de nivel de agua (ultrasónico) y presión/caudal, con transmisión inalámbrica vía LoRa.
3. **Nodo Edge en Poste:** Recepción de paquetes LoRa de A1 y A2, validación, organización temporal, clasificación de condición (normal, alerta, crítico) y generación de alertas.
4. **Operador (Interfaz de Consulta):** Visualización de estado, mapa de ubicaciones, histórico de mediciones y alertas para priorización de mantenimiento.

## 2. Organización del Equipo y Módulos del Repositorio

| Integrante | Rol Principal | Carpeta Asignada |
| :--- | :--- | :--- |
| **Layla Vanessa Zúñiga Hidrobo** | Scrum Master + Integración y Pruebas | `/docs_interfaz`, `/evidencias_pruebas` |
| **Juan David Manrique Ramos** | Desarrollador del Nodo Edge y Procesamiento | `/nodo_edge` |
| **Deler Santiago Mendez Mendez** | Desarrollador de Sistemas Embebidos, Sensores y LoRa | `/firmware_nodos_a1_a2` |
| **Gabby Cecilia Ruiz Vivas** | Desarrolladora de Almacenamiento, Backend e Interfaz | `/backend_interfaz` |
| **Paola Andrea Ordoñez Lopez** | Desarrolladora de Hardware y Prototipo Físico | `/hardware_y_maqueta` |

## 3. Regla de Trazabilidad (Scrum + Ingeniería)
Todo commit y evidencia técnica en este repositorio responde a la cadena de trazabilidad del curso:
`REQUERIMIENTO → BACKLOG → TAREA → IMPLEMENTACIÓN → PRUEBA → EVIDENCIA → DECISIÓN`