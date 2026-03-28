#ifndef HANDLER_OTA_GITHUB_H
#define HANDLER_OTA_GITHUB_H

#include "esp_http_server.h"

esp_err_t POST_OTA_github(httpd_req_t *req);
esp_err_t GET_OTA_github_status(httpd_req_t *req);

#endif
