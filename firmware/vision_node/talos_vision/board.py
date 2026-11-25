"""TL-VN-001 build-1 pin table (ADR 0020). PROVISIONAL until the bench (no schematic: bought parts).

Mirrors contracts/manifests/boards/vision_node_pi_zero2w.yaml; tests/test_board.py holds the two together.
"""

BOARD_ID = "vision_node_pi_zero2w"
BOARD_REVISION = "build-1"
BOARD_FAMILY = "TL-VN-001"

# Lid tamper: a microswitch's normally-open contact from GPIO17 (header pin 11) to GND, pressed closed
# by the lid. Internal pull-up, so high = lid open or wire cut.
TAMPER_GPIO = 17
TAMPER_HEADER_PIN = 11
TAMPER_OPEN_LEVEL = 1
TAMPER_DEBOUNCE_MS = 50  # as on TL-SN-002 (HD-032)

# The camera's stream as MediaMTX serves it (mediamtx.yml.template).
STREAM_PATH = "cam"
STREAM_WIDTH = 1920
STREAM_HEIGHT = 1080
STREAM_FPS = 15
STREAM_BITRATE_KBPS = 4000

CAPABILITIES = ("camera.stream", "tamper.enclosure")
