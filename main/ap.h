#pragma once

typedef struct {
	char ssid[64];
	char pass[64];
} user_config_t;

user_config_t* get_user_config(void);