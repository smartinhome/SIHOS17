# Panel WWW jest wbudowany w firmware jako spakowany gzipem blob.
# Zrodlo: webui/index.html (edytowalny HTML/JS), skompresowany plik
# webui/index.html.gz siedzi w repo (regenerowany po edycji index.html).
#
# Kopiujemy .gz do build/ przez configure_file(COPYONLY), zeby symbol binarny
# tworzony przez EMBED_FILES mial krotka nazwe (_binary_index_html_gz_start)
# zamiast dlugiej z podkreslnikami dla kazdego segmentu sciezki.
#
# WAZNE: uzywamy CMAKE_CURRENT_LIST_DIR zamiast CMAKE_CURRENT_SOURCE_DIR.
# ESP-IDF w fazie component_get_requirements wczytuje CMakeLists.txt komponentu
# przez include() ze swojego skryptu w /opt/esp/idf/tools/cmake/scripts/ - w tym
# kontekscie CMAKE_CURRENT_SOURCE_DIR wskazuje na SKRYPT ESP-IDF, nie na folder
# komponentu. CMAKE_CURRENT_LIST_DIR jest ZAWSZE folderem TEGO pliku, nawet gdy
# jest include'd - jedyna bezpieczna opcja dla resolwowania sciezek wzgledem
# folderu komponentu.
set(WEB_GZ_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../webui/index.html.gz")
set(WEB_GZ_DST "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
if(NOT EXISTS "${WEB_GZ_SRC}")
    message(FATAL_ERROR
        "Brak webui/index.html.gz (szukam: ${WEB_GZ_SRC}). "
        "Wygeneruj go z webui/index.html komenda:\n"
        "  python -c \"import gzip; open('webui/index.html.gz','wb').write("
        "gzip.compress(open('webui/index.html','rb').read(), 9))\"")
endif()
configure_file("${WEB_GZ_SRC}" "${WEB_GZ_DST}" COPYONLY)

idf_component_register(
    SRCS "webserver.c" "api_handlers.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server log_buffer wifi_manager wmbus_decoder nvs_config ota_manager log esp_timer history esp_hw_support driver app_update esp_app_format spi_flash led_rx led_status
    EMBED_FILES "${WEB_GZ_DST}"
)
