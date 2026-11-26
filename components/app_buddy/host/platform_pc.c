// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "app_buddy.h"
#include "gsp_sim_bridge.h"

static struct {
    app_buddy_handle_t controller;
} backend;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    return app_buddy_create(ui, &backend.controller);
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    app_buddy_snapshot_t s;
    if (app_buddy_read(backend.controller, &s) == ESP_GSP_OK) {
        fprintf(stderr, "buddy_backend: state=%d frames=%u blinks=%u taps=%u last_error=%d\n",
                (int)s.state, (unsigned)s.frames, (unsigned)s.blinks, (unsigned)s.taps,
                (int)s.last_error);
    }
    const esp_gsp_err_t err = app_buddy_destroy(&backend.controller);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "buddy_backend: cleanup failed (%d)\n", err);
    }
}
