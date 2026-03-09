idf_component_register(
SRCS
    "utils.c"
    "mining.c"
    "stratum_api.c"
    "base58.c"
    "segwit_addr.c"
    "coinbase_decoder.c"
                    
INCLUDE_DIRS
    "include"

REQUIRES
    "json"
    "mbedtls"
    "app_update"
)