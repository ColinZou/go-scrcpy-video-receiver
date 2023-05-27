﻿// scrcpy_desktop.cpp : Defines the entry point for the application.
//

#include "scrcpy_support.h"
#include <direct.h>
#include <deque>
#include <stdint.h>
#include <vector>
#include <Windows.h>
#include <stdlib.h>
#include <string>
#include <cstring>
#include <algorithm>
#include <stdio.h>

int global_frame_no = 0;
bool save_frame_images = false;
char filename_buf[256];
const char *device_id = "session001";
scrcpy_listener_t listener = nullptr;
void test_internal_video_frame_callback(char *token, char* device_id, uint8_t* frame_data, uint32_t frame_data_size, scrcpy_rect img_size,
	scrcpy_rect screen_size) {
	global_frame_no++;
	printf("Got video frame for token = %s device = %s data size = %d scaled from %dx%d to %dx%d\n", token, device_id, frame_data_size, screen_size.width, screen_size.height, img_size.width, img_size.height);

    if (!save_frame_images) {
        return;
    }

	_mkdir("images");

	sprintf_s(filename_buf, "images/frame-%02d.png", global_frame_no);
	FILE* f;
	errno_t err = fopen_s(&f, filename_buf, "wb");
	if (err) {
		printf("Could not open file %s for write\n", filename_buf);
		return;
	}
	fwrite(frame_data, sizeof(uint8_t), frame_data_size, f);
	fclose(f);
}
void test_deque() {
	std::deque<int> my_queue = { 1, 2, 3 };
	int size = my_queue.size();
	while (size > 0) {
		printf("Got %d \n", my_queue.back());
		my_queue.pop_back();
		size --;
	}
}
std::vector<uint8_t> int_to_bytes(int paramInt)
{
    std::vector<uint8_t> arrayOfByte(4);
     for (int i = 0; i < 4; i++)
         arrayOfByte[3 - i] = (paramInt >> (i * 8));
     return arrayOfByte;
}
void device_info_callback(char *token, char* device_id, int w, int h) {
	printf("device_info_callback device_id=%s screen_width=%d screen_height=%d\n", device_id, w, h);
    Sleep(1000);
    printf("About to send a key event");
    // try sending a ctrl msg
    uint8_t ctrl_msg[14];
    //clean first
    for(int i = 0; i < 14; i++) {
        ctrl_msg[i] = 0;
    }
    // action type = key event
    ctrl_msg[0] = 0;
    // action down 
    ctrl_msg[1] = 0;

    // keycode home = 3
    auto home_key =  int_to_bytes(3);
    ctrl_msg[2] = home_key[0];
    ctrl_msg[3] = home_key[1];
    ctrl_msg[4] = home_key[2];
    ctrl_msg[5] = home_key[3];

    // repeat
    auto repeat_times = int_to_bytes(2);
    ctrl_msg[6] = repeat_times[0];
    ctrl_msg[7] = repeat_times[1];
    ctrl_msg[8] = repeat_times[2];
    ctrl_msg[9] = repeat_times[3];

    // meta state
    ctrl_msg[10] = 0;
    ctrl_msg[11] = 0;
    ctrl_msg[12] = 0;
    ctrl_msg[13] = 0;
   
    std::string msg_id = "msg001";
    scrcpy_device_send_ctrl_msg(listener, (char *)device_id, (char *)msg_id.c_str(), ctrl_msg, 14);
}

void device_ctrl_msg_callback(char *token, char *device_id, char* msg_id, int status, int data_len) {
    printf("device_ctrl_msg_callback invoked, token=%s device_id=%s msg_id=%s status=%d data_len=%d\n", token, device_id, msg_id, status, data_len);
}
void config_from_env() {
    const char* env_name = "SCRCPY_SAVE_FRAMES";
    auto env_value = std::getenv(env_name);
    if (!env_value || std::string(env_value).empty()){
        printf("No env SCRCPY_SAVE_FRAMES configured\n");
        return;
    }
    printf("Env SCRCPY_SAVE_FRAMES=%s\n", env_value);
    auto env_value_str = std::string(env_value);
    std::transform(env_value_str.begin(), env_value_str.end(), env_value_str.begin(), ::tolower);
    if (strcmp(env_value_str.c_str(), "y") == 0) {
        save_frame_images = true;
    }
    printf("Will save frames to image files? %s\n", save_frame_images ? "YES": "NO");
}
int main(){
	char address[] = "27183";
	int kb_2048 = 1024 * 2;
	printf("Trying to listen at port %s \n", (char *) address);
	char token[] = "test";
    config_from_env();
	listener = scrcpy_new_receiver(token);
	scrcpy_device_info_register_callback(listener, (char*)device_id, device_info_callback);
	scrcpy_set_image_size(listener, (char*)device_id, 540, 1076);
	scrcpy_frame_register_callback(listener, (char*)device_id, test_internal_video_frame_callback);
    scrcpy_device_set_ctrl_msg_send_callback(listener, (char*)device_id, device_ctrl_msg_callback);
	printf("Trying to start listener\n");
	scrcpy_start_receiver(listener, address, kb_2048, kb_2048 * 2);
	free((char*)device_id);
	return 0;
}
