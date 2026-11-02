idf_component_register(
    SRCS
        "app_main.cpp"
        "hidecare_client.cpp"
        "hidecare_setup.cpp"
        "hidecare_ble.cpp"
        "hidecare_led.cpp"
        "hidecare_dns_fallback.c"
        "vitals.cpp"
        "vitals_core.c"
    INCLUDE_DIRS
        "."
        "../../espectre"
        "../../.."
    REQUIRES
        espectre
        esp_event
        esp_netif
        esp_wifi
        freertos
        log
        lwip
        mqtt
        app_update
        esp_http_client
        esp_http_server
        esp_https_ota
        esp-tls
        mbedtls
        nvs_flash
        bt
        improv__improv
)

set(_native_ota_channel "${NATIVE_OTA_CHANNEL}")
if(_native_ota_channel STREQUAL "")
    set(_native_ota_channel "$ENV{NATIVE_OTA_CHANNEL}")
endif()
if(_native_ota_channel STREQUAL "")
    set(_native_ota_channel "release")
endif()

if(_native_ota_channel STREQUAL "preview")
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE ESPECTRE_OTA_PREVIEW_BUILD=1
    )
elseif(_native_ota_channel STREQUAL "develop")
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE ESPECTRE_OTA_DEVELOP_BUILD=1
    )
elseif(NOT _native_ota_channel STREQUAL "release")
    message(FATAL_ERROR "Invalid Native OTA channel: ${_native_ota_channel}")
endif()

# Hidecare: DNS fallback to a fixed server IP (see hidecare_dns_fallback.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lwip_getaddrinfo" "-Wl,-u,__wrap_lwip_getaddrinfo")
