/**
 * @file hw_identity.c
 * @brief Hardware Identity Management Implementation
 */

#include "hw_identity.h"
#include "storage_mgr.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "HW-IDENTITY";

/* Global hardware identity instance */
static hw_identity_t s_hw_identity = {
    .cpu_name = "CPU",
    .gpu_name = "GPU",
    .identity_hash = "00000000",
    .device_name = ""
};

/* Set by the USB task when names changed; consumed by the UI thread
 * (display_update_task) which applies the labels under the LVGL mutex.
 * Direct lv_label_set_text from the USB task would race with rendering. */
static volatile bool s_names_dirty = false;

hw_identity_t *hw_identity_get(void)
{
    return &s_hw_identity;
}

void hw_identity_load(void)
{
    /* Load names */
    FILE *f = fopen(HW_NAMES_FILE_PATH, "r");
    if (f != NULL) {
        char line[64];
        while (fgets(line, sizeof(line), f) != NULL) {
            /* Remove newline/carriage return */
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(line, '\r');
            if (cr) *cr = '\0';

            /* Parse KEY=VALUE */
            if (strncmp(line, "CPU_NAME=", 9) == 0) {
                strncpy(s_hw_identity.cpu_name, line + 9, sizeof(s_hw_identity.cpu_name) - 1);
                s_hw_identity.cpu_name[sizeof(s_hw_identity.cpu_name) - 1] = '\0';
                ESP_LOGI(TAG, "Loaded CPU name: %s", s_hw_identity.cpu_name);
            }
            else if (strncmp(line, "GPU_NAME=", 9) == 0) {
                strncpy(s_hw_identity.gpu_name, line + 9, sizeof(s_hw_identity.gpu_name) - 1);
                s_hw_identity.gpu_name[sizeof(s_hw_identity.gpu_name) - 1] = '\0';
                ESP_LOGI(TAG, "Loaded GPU name: %s", s_hw_identity.gpu_name);
            }
            else if (strncmp(line, "DEVICE_NAME=", 12) == 0) {
                strncpy(s_hw_identity.device_name, line + 12, sizeof(s_hw_identity.device_name) - 1);
                s_hw_identity.device_name[sizeof(s_hw_identity.device_name) - 1] = '\0';
                ESP_LOGI(TAG, "Loaded device name: %s", s_hw_identity.device_name);
            }
        }
        fclose(f);
    } else {
        ESP_LOGW(TAG, "No names.txt found, using defaults");
    }

    /* Load identity hash */
    FILE *hf = fopen(HW_HASH_FILE_PATH, "r");
    if (hf != NULL) {
        char hash_buf[16];
        if (fgets(hash_buf, sizeof(hash_buf), hf) != NULL) {
            /* Remove newline */
            char *nl = strchr(hash_buf, '\n');
            if (nl) *nl = '\0';
            if (strlen(hash_buf) == 8) {
                strncpy(s_hw_identity.identity_hash, hash_buf, sizeof(s_hw_identity.identity_hash) - 1);
                ESP_LOGI(TAG, "Loaded identity hash: %s", s_hw_identity.identity_hash);
            }
        }
        fclose(hf);
    } else {
        ESP_LOGW(TAG, "No host.hash found, using default 00000000");
    }
}

void hw_identity_save(void)
{
    /* Save names */
    FILE *f = fopen(HW_NAMES_FILE_PATH, "w");
    if (f != NULL) {
        fprintf(f, "CPU_NAME=%s\n", s_hw_identity.cpu_name);
        fprintf(f, "GPU_NAME=%s\n", s_hw_identity.gpu_name);
        if (s_hw_identity.device_name[0] != '\0') {
            fprintf(f, "DEVICE_NAME=%s\n", s_hw_identity.device_name);
        }
        fclose(f);
        ESP_LOGI(TAG, "Saved names to LittleFS");
    } else {
        ESP_LOGE(TAG, "Failed to save names.txt");
    }

    /* Save hash */
    FILE *hf = fopen(HW_HASH_FILE_PATH, "w");
    if (hf != NULL) {
        fprintf(hf, "%s\n", s_hw_identity.identity_hash);
        fclose(hf);
        ESP_LOGI(TAG, "Saved hash to LittleFS: %s", s_hw_identity.identity_hash);
    } else {
        ESP_LOGE(TAG, "Failed to save host.hash");
    }
}

void hw_identity_set_cpu_name(const char *name)
{
    if (name) {
        strncpy(s_hw_identity.cpu_name, name, sizeof(s_hw_identity.cpu_name) - 1);
        s_hw_identity.cpu_name[sizeof(s_hw_identity.cpu_name) - 1] = '\0';
    }
}

void hw_identity_set_gpu_name(const char *name)
{
    if (name) {
        strncpy(s_hw_identity.gpu_name, name, sizeof(s_hw_identity.gpu_name) - 1);
        s_hw_identity.gpu_name[sizeof(s_hw_identity.gpu_name) - 1] = '\0';
    }
}

void hw_identity_set_hash(const char *hash)
{
    if (hash && strlen(hash) >= 8) {
        strncpy(s_hw_identity.identity_hash, hash, 8);
        s_hw_identity.identity_hash[8] = '\0';
    }
}

void hw_identity_set_device_name(const char *name)
{
    if (!name) return;

    /* Copy, skipping '|' (handshake field delimiter) */
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0' && j < sizeof(s_hw_identity.device_name) - 1; i++) {
        if (name[i] != '|') {
            s_hw_identity.device_name[j++] = name[i];
        }
    }
    s_hw_identity.device_name[j] = '\0';
}

bool hw_identity_consume_names_dirty(void)
{
    if (s_names_dirty) {
        s_names_dirty = false;
        return true;
    }
    return false;
}

bool hw_identity_handle_command(const char *line)
{
    bool needs_save = false;
    bool needs_ui_update = false;

    /* NAME_CPU=<cpu_name> */
    if (strncmp(line, "NAME_CPU=", 9) == 0) {
        if (strcmp(s_hw_identity.cpu_name, line + 9) != 0) {
            hw_identity_set_cpu_name(line + 9);
            ESP_LOGI(TAG, "Received CPU name: %s", s_hw_identity.cpu_name);
            needs_save = true;
            needs_ui_update = true;
        }
    }
    /* NAME_GPU=<gpu_name> */
    else if (strncmp(line, "NAME_GPU=", 9) == 0) {
        if (strcmp(s_hw_identity.gpu_name, line + 9) != 0) {
            hw_identity_set_gpu_name(line + 9);
            ESP_LOGI(TAG, "Received GPU name: %s", s_hw_identity.gpu_name);
            needs_save = true;
            needs_ui_update = true;
        }
    }
    /* NAME_HASH=<8-char hex> */
    else if (strncmp(line, "NAME_HASH=", 10) == 0) {
        hw_identity_set_hash(line + 10);
        ESP_LOGI(TAG, "Received identity hash: %s", s_hw_identity.identity_hash);
        needs_save = true;
    }
    /* SET_ID:<device_name> - user-assigned name, reported in handshake |N: */
    else if (strncmp(line, "SET_ID:", 7) == 0) {
        hw_identity_set_device_name(line + 7);
        ESP_LOGI(TAG, "Received device name: %s", s_hw_identity.device_name);
        needs_save = true;
    }
    else {
        return false;  /* Not a NAME command */
    }

    if (needs_save) {
        hw_identity_save();
    }

    if (needs_ui_update) {
        s_names_dirty = true;  /* UI thread picks this up via hw_identity_consume_names_dirty() */
    }

    return true;
}
