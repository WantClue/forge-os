idf_component_register(
SRCS
    "utils.c"
    "mining.c"
    "stratum_api.c"
    "coinbase_decoder.c"
    "segwit_addr.c"
    "base58.c"
    "sha256.c"
    "job_id_filter.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "app_update"
    "esp_timer"
    "tcp_transport"
)
