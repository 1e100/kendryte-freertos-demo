/* Copyright 2018 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <FreeRTOS.h>
#include <devices.h>
#include <stdio.h>
#include <stdlib.h>
#include <task.h>
#include "dvp_camera.h"
#include "image_process.h"
#include "lcd.h"
#include "ov5640.h"
#include "region_layer.h"
#define INCBIN_STYLE INCBIN_STYLE_SNAKE
#define INCBIN_PREFIX
#include "incbin.h"

image_t kpu_image, display_image0, display_image1;
handle_t model_context;
camera_context_t camera_ctx;
static region_layer_t face_detect_rl;
static obj_info_t face_detect_info;
#define ANCHOR_NUM 5
static float anchor[ANCHOR_NUM * 2] = {1.889,    2.5245, 2.9465,   3.94056,
                                       3.99987,  5.3658, 5.155437, 6.92275,
                                       6.718375, 9.01025};

INCBIN(model, "../src/face_detect/aug.kmodel");

static void draw_edge(uint32_t* gram, obj_info_t* obj_info, uint32_t index,
                      uint16_t color) {
  uint32_t data = ((uint32_t)color << 16) | (uint32_t)color;
  volatile uint32_t *addr1, *addr2, *addr3, *addr4, x1, y1, x2, y2;

  x1 = obj_info->obj[index].x1;
  y1 = obj_info->obj[index].y1;
  x2 = obj_info->obj[index].x2;
  y2 = obj_info->obj[index].y2;

  if (x1 <= 0) x1 = 1;
  if (x2 >= 319) x2 = 318;
  if (y1 <= 0) y1 = 1;
  if (y2 >= 239) y2 = 238;

  addr1 = gram + (320 * y1 + x1) / 2;
  addr2 = gram + (320 * y1 + x2 - 8) / 2;
  addr3 = gram + (320 * (y2 - 1) + x1) / 2;
  addr4 = gram + (320 * (y2 - 1) + x2 - 8) / 2;
  for (uint32_t i = 0; i < 4; i++) {
    *addr1 = data;
    *(addr1 + 160) = data;
    *addr2 = data;
    *(addr2 + 160) = data;
    *addr3 = data;
    *(addr3 + 160) = data;
    *addr4 = data;
    *(addr4 + 160) = data;
    addr1++;
    addr2++;
    addr3++;
    addr4++;
  }
  addr1 = gram + (320 * y1 + x1) / 2;
  addr2 = gram + (320 * y1 + x2 - 2) / 2;
  addr3 = gram + (320 * (y2 - 8) + x1) / 2;
  addr4 = gram + (320 * (y2 - 8) + x2 - 2) / 2;
  for (uint32_t i = 0; i < 8; i++) {
    *addr1 = data;
    *addr2 = data;
    *addr3 = data;
    *addr4 = data;
    addr1 += 160;
    addr2 += 160;
    addr3 += 160;
    addr4 += 160;
  }
}

int main(void) {
  kpu_image.pixel = 3;
  kpu_image.width = 320;
  kpu_image.height = 240;
  image_init(&kpu_image);

  display_image0.pixel = 2;
  display_image0.width = 320;
  display_image0.height = 240;
  image_init(&display_image0);

  display_image1.pixel = 2;
  display_image1.width = 320;
  display_image1.height = 240;
  image_init(&display_image1);

  camera_ctx.dvp_finish_flag = 0;
  camera_ctx.ai_image = &kpu_image;
  camera_ctx.lcd_image0 = &display_image0;
  camera_ctx.lcd_image1 = &display_image1;
  camera_ctx.gram_mux = 0;

  model_context = kpu_model_load_from_buffer(model_data);

  face_detect_rl.anchor_number = ANCHOR_NUM;
  face_detect_rl.anchor = anchor;
  face_detect_rl.threshold = 0.7;
  face_detect_rl.nms_value = 0.3;
  region_layer_init(&face_detect_rl, 20, 15, 30, camera_ctx.ai_image->width,
                    camera_ctx.ai_image->height);

  printf("lcd init\n");
  lcd_init();
  printf("DVP init\n");
  dvp_init(&camera_ctx);
  ov5640_init();

  while (1) {
    while (camera_ctx.dvp_finish_flag == 0)
      ;
    camera_ctx.dvp_finish_flag = 0;
    uint32_t* lcd_gram = camera_ctx.gram_mux
                             ? (uint32_t*)camera_ctx.lcd_image1->addr
                             : (uint32_t*)camera_ctx.lcd_image0->addr;

    if (kpu_run(model_context, camera_ctx.ai_image->addr) != 0) {
      printf("Cannot run kmodel.\n");
      exit(-1);
    }

    float* output;
    size_t output_size;
    kpu_get_output(model_context, 0, (uint8_t**)&output, &output_size);

    face_detect_rl.input = output;
    region_layer_run(&face_detect_rl, &face_detect_info);
    for (uint32_t face_cnt = 0; face_cnt < face_detect_info.obj_number;
         face_cnt++) {
      draw_edge(lcd_gram, &face_detect_info, face_cnt, RED);
    }

    lcd_draw_picture(0, 0, 320, 240, lcd_gram);
    camera_ctx.gram_mux ^= 0x01;
  }
  io_close(model_context);
  image_deinit(&kpu_image);
  image_deinit(&display_image0);
  image_deinit(&display_image1);
  while (1)
    ;
}
